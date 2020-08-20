/**
 * Core volume type implementation.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "core-obj.h"
#include "common.h"
#include "core-vol.h"


extern bool FLASHSIM_ENABLE_DATA;
extern unsigned long FLASHSIM_PAGE_SIZE;


extern const char * const core_sock_name;
extern const int core_parallelism;


/**
 * A double-linked list as a request submitting queue. Request submission
 * operation pushes an entry into the queue and then ACKs immediately. A
 * separate submit thread processes the queue synchronously.
 */
struct req_entry {
    struct list_head head;
    struct ocf_io *io;
    double start_time_ms;
};

static struct req_entry submit_queue;

static env_mutex submit_queue_lock;
static env_completion submit_queue_sem;


/**
 * Request header (1st message) format.
 * Message size MUST exactly match in bytes!
 */
struct __attribute__((__packed__)) req_header {
    uint32_t direction     : 32;
    uint64_t addr          : 64;
    uint32_t size          : 32;
    uint64_t start_time_us : 64;
};

static const size_t REQ_HEADER_LENGTH = 24;

static const int FLASHSIM_DIR_READ  = 0;
static const int FLASHSIM_DIR_WRITE = 1;


/** This lock protects the FlashSim socket file. */
static env_mutex core_device_lock;


/**
 * Routines to read from or write to the storage device.
 */
static int
_submit_write_io(struct ocf_io *io, simfs_data_t *data, int sock_fd,
                 double start_time_ms)
{
    struct req_header header;
    int rbytes, wbytes;
    uint64_t time_used_us;

    /** Request header. */
    header.direction = FLASHSIM_DIR_WRITE;
    header.addr = io->addr;
    header.size = io->bytes;
    header.start_time_us = (uint64_t) (1000 * start_time_ms);

    /** Critical section: ensuring the three messages are always in order. */
    env_mutex_lock(&core_device_lock);

    wbytes = write(sock_fd, &header, REQ_HEADER_LENGTH);
    if (wbytes != REQ_HEADER_LENGTH) {
        DEBUG("IO: write request header send failed");
        env_mutex_unlock(&core_device_lock);
        return 1;
    }

    /** Data to write, only if passing actual data. */
    if (FLASHSIM_ENABLE_DATA) {
        wbytes = write(sock_fd, data->ptr + data->offset, header.size);
        if (wbytes != (int) header.size) {
            DEBUG("IO: write request data send failed");
            env_mutex_unlock(&core_device_lock);
            return 2;
        }
    }

    /** Processing time respond. */
    rbytes = read(sock_fd, &time_used_us, 8);
    if (rbytes != 8) {
        DEBUG("IO: write processing time recv failed");
        env_mutex_unlock(&core_device_lock);
        return 3;
    }

    env_mutex_unlock(&core_device_lock);
    /** Critical section ends. */

    usleep(time_used_us);   /** Simulate latency here. */

    return 0;
}

static int
_submit_read_io(struct ocf_io *io, simfs_data_t *data, int sock_fd,
                double start_time_ms)
{
    struct req_header header;
    int rbytes, wbytes;
    uint64_t time_used_us;

    /** Request header. */
    header.direction = FLASHSIM_DIR_READ;
    header.addr = io->addr;
    header.size = io->bytes;
    header.start_time_us = (uint64_t) (1000 * start_time_ms);

    /** Critical section: ensuring the three messages are always in order. */
    env_mutex_lock(&core_device_lock);

    wbytes = write(sock_fd, &header, REQ_HEADER_LENGTH);
    if (wbytes != REQ_HEADER_LENGTH) {
        DEBUG("IO: read request header send failed");
        env_mutex_unlock(&core_device_lock);
        return 1;
    }

    /** Data read out, only if passing actual data. */
    if (FLASHSIM_ENABLE_DATA) {
        rbytes = read(sock_fd, data->ptr + data->offset, header.size);
        if (rbytes != (int) header.size) {
            DEBUG("IO: read request data recv failed");
            env_mutex_unlock(&core_device_lock);
            return 2;
        }
    }

    /** Processing time respond. */
    rbytes = read(sock_fd, &time_used_us, 8);
    if (rbytes != 8) {
        DEBUG("IO: read processing time recv failed");
        env_mutex_unlock(&core_device_lock);
        return 3;
    }

    env_mutex_unlock(&core_device_lock);
    /** Critical section ends. */

    usleep(time_used_us);   /** Simulate latency here. */

    return 0;
}

/**
 * Submission thread runs separately. ARGS is a pointer to package id.
 */
static void *
_submit_thread_func(void *args)
{
    struct req_entry *entry;
    struct ocf_io *io;
    double start_time_ms;

    int pkg = *((int *) args);
    free((int *) args);

    DEBUG("SUBMIT: submission thread for package %d launched", pkg);

    while (1) {
        /** Wait when list is empty. */
        env_completion_wait(&submit_queue_sem);

        /** Extract an entry from queue head. */
        env_mutex_lock(&submit_queue_lock);

        entry = list_first_entry(&submit_queue.head, struct req_entry, head);
        io = entry->io;
        start_time_ms = entry->start_time_ms;

        list_del(&entry->head);
        free(entry);

        env_mutex_unlock(&submit_queue_lock);

        /** Process the request. */
        simfs_data_t *data = ocf_io_get_data(io);
        core_vol_priv_t *vol_priv = ocf_volume_get_priv(ocf_io_get_volume(io));

        // DEBUG("IO: dir = %s, core pos = 0x%08lx, len = %u",
        //       io->dir == OCF_WRITE ? "WR <-" : "RD ->", io->addr, io->bytes);

        switch (io->dir) {
        case OCF_WRITE:
            _submit_write_io(io, data, vol_priv->sock_fd, start_time_ms);
            break;
        case OCF_READ:
            _submit_read_io(io, data, vol_priv->sock_fd, start_time_ms);
            break;
        }

        if (io->dir == OCF_READ) {
            core_log_push_entry(pkg, start_time_ms, get_cur_time_ms(),
                                io->bytes);
        }

        io->end(io, 0);
    }

    // Not reached.
    pthread_exit(NULL);
    return NULL;
}


/*========== Core Volume Operations Implemention BEGIN. ==========*/

/**
 * Open core volume.
 * Here we store uuid as volume name and connect to FlashSim socket.
 *
 * At any time, at most CORE_PARALLELISM requests are being processed
 * concurrently. This models in-device parallelism.
 */
static int
core_vol_open(ocf_volume_t core_vol, void *params)
{
    const struct ocf_volume_uuid *uuid = ocf_volume_get_uuid(core_vol);
    core_vol_priv_t *vol_priv = ocf_volume_get_priv(core_vol);
    struct sockaddr_un saddr;
    int ret, i;

    vol_priv->name = ocf_uuid_to_str(uuid);
    
    /** Prepare socket here. */
    vol_priv->sock_name = core_sock_name;
    vol_priv->sock_fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (vol_priv->sock_fd < 0) {
        DEBUG("OPEN: socket() failed");
        return 1;
    }

    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_LOCAL;
    strncpy(saddr.sun_path, vol_priv->sock_name, sizeof(saddr.sun_path) - 1);

    ret = connect(vol_priv->sock_fd, (struct sockaddr *) &saddr,
                  sizeof(saddr));
    if (ret) {
        DEBUG("OPEN: connect() failed");
        return ret;
    }

    /** Initialize submission queue. */
    INIT_LIST_HEAD(&submit_queue.head);

    ret = env_mutex_init(&submit_queue_lock);
    if (ret) {
        DEBUG("OPEN: queue lock initialization failed");
        return ret;
    }

    env_completion_init(&submit_queue_sem);

    /** Initialize device lock. */
    ret = env_mutex_init(&core_device_lock);
    if (ret) {
        DEBUG("OPEN: core device lock initialization failed");
        return ret;
    }

    /** Start CORE_PARALLELISM submit threads at volume open. */
    for (i = 0; i < core_parallelism; ++i) {
        pthread_t submit_thread_id;
        pthread_attr_t submit_thread_attr;
        int *pkg_ptr;

        ret = pthread_attr_init(&submit_thread_attr);
        if (ret) {
            DEBUG("OPEN: submit thread %d creation failed", i);
            return ret;
        }

        ret = pthread_attr_setdetachstate(&submit_thread_attr,
                                          PTHREAD_CREATE_DETACHED);
        if (ret) {
            DEBUG("OPEN: submit thread %d creation failed", i);
            return ret;
        }

        pkg_ptr = malloc(sizeof(int));
        *pkg_ptr = i;

        ret = pthread_create(&submit_thread_id, &submit_thread_attr,
                             _submit_thread_func, (void *) pkg_ptr);
        if (ret) {
            DEBUG("OPEN: submit thread %d creation failed", i);
            return ret;
        }
    }

    DEBUG("OPEN: name = %s, sock = %s", vol_priv->name, vol_priv->sock_name);
    return 0;
}

/**
 * Close core volume.
 * Here we simply close the socket.
 */
static void
core_vol_close(ocf_volume_t core_vol)
{
    core_vol_priv_t *vol_priv = ocf_volume_get_priv(core_vol);

    DEBUG("CLOSE: name = %s", vol_priv->name);

    close(vol_priv->sock_fd);

    ENV_BUG_ON(! list_empty(&submit_queue.head));

    env_mutex_destroy(&submit_queue_lock);
    env_completion_destroy(&submit_queue_sem);

    env_mutex_destroy(&core_device_lock);
}

// Exposed for device logging.
extern double base_time_ms;

/**
 * Submit an IO request to volume.
 * Here we simply push to the tail of queue.
 */
static void
core_vol_submit_io(struct ocf_io *io)
{
    struct req_entry *entry;
    int queue_depth;

    /** Address must be page-aligned. */
    if (io->addr % FLASHSIM_PAGE_SIZE != 0) {
        DEBUG("IO: unaligned addr 0x%08lx", io->addr);
        io->end(io, 1);
        return;
    }

    entry = malloc(sizeof(struct req_entry));
    if (entry == NULL) {
        DEBUG("IO: push to submit queue failed");
        return;
    }

    entry->io = io;
    entry->start_time_ms = get_cur_time_ms();

    env_mutex_lock(&submit_queue_lock);

    list_add_tail(&entry->head, &submit_queue.head);
    env_completion_complete(&submit_queue_sem);

    sem_getvalue(&submit_queue_sem.sem, &queue_depth);
    fprintf(fdevice, "core queue: @ %.3lf, depth = %d\n",
            entry->start_time_ms - base_time_ms, queue_depth);
    
    env_mutex_unlock(&submit_queue_lock);
}

/**
 * Submit flush request.
 * Can be unimplemented if not needed.
 */
static void
core_vol_submit_flush(struct ocf_io *io)
{
    io->end(io, 0);     /** Unimplemented. */
}

/**
 * Submit discard request.
 * Can be unimplemented if not needed.
 */
static void
core_vol_submit_discard(struct ocf_io *io)
{
    io->end(io, 0);     /** Unimplemented. */
}


/**
 * Define the max I/O size for this volume.
 * Here we use 128KiB.
 */
static unsigned int
core_vol_get_max_io_size(ocf_volume_t core_vol)
{
    return CORE_VOL_MAX_IO_SIZE;
}

/**
 * Get volume capacity.
 */
static uint64_t
core_vol_get_length(ocf_volume_t core_vol)
{
    return CORE_VOL_SIZE;
}


/**
 * Define how to setup a single I/O structure given OS data buffer
 * structure.
 */
static int
core_vol_io_set_data(struct ocf_io *io, ctx_data_t *simfs_data,
                     uint32_t offset)
{
    core_vol_io_priv_t *io_priv = ocf_io_get_priv(io);

    /** This offset is meant to control I/O in finer granularity. */
    io_priv->data = simfs_data;
    io_priv->offset = offset;   /** Unused in this context case. */

    return 0;
}

/**
 * Define how to retrieve OS buffer structured data from a single
 * I/O structure.
 */
static ctx_data_t *
core_vol_io_get_data(struct ocf_io *io)
{
    core_vol_io_priv_t *io_priv = ocf_io_get_priv(io);

    return io_priv->data;
}


/**
 * This structure assigns the above implementations to the OCF volume
 * interface. This structure essentially defines a volume type, which
 * can be used when setting up volumes during context initialization.
 */
const struct ocf_volume_properties core_vol_properties = {
    .name = "Core Volume",
    
    .io_priv_size = sizeof(core_vol_io_priv_t),
    .volume_priv_size = sizeof(core_vol_priv_t),

    .caps = {
        .atomic_writes = 0,
    },

    .ops = {
        .open = core_vol_open,
        .close = core_vol_close,
        .submit_io = core_vol_submit_io,
        .submit_flush = core_vol_submit_flush,
        .submit_discard = core_vol_submit_discard,
        .get_max_io_size = core_vol_get_max_io_size,
        .get_length = core_vol_get_length,
    },

    .io_ops = {
        .set_data = core_vol_io_set_data,
        .get_data = core_vol_io_get_data,
    },
};

/*========== Core Volume Operations Implemention END. ==========*/


/**
 * Registers the above structure as volume type CORE_VOL_TYPE in
 * this OCF context.
 * Should be called just AFTER context initialization.
 */
int
core_vol_register(ocf_ctx_t ctx)
{
    int ret = ocf_ctx_register_volume_type(ctx, CORE_VOL_TYPE,
                                           &core_vol_properties);

    DEBUG("REGISTER: as type = %d", CORE_VOL_TYPE);

    return ret;
}

/**
 * Unregisters core volume type.
 * Should be called just BEFORE context cleanup.
 */
void
core_vol_unregister(ocf_ctx_t ctx)
{
    ocf_ctx_unregister_volume_type(ctx, CORE_VOL_TYPE);

    DEBUG("UNREGISTER: done");
}
