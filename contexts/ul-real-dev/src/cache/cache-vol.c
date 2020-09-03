/**
 * Cache volume type implementation.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ocf/ocf.h>
#include <fcntl.h>
#include <libaio.h>

#include "simfs/simfs-ctx.h"
#include "cache-obj.h"
#include "common.h"
#include "cache-vol.h"


/** Indicates whether we should exit without finishing pending requests. */
static env_atomic should_stop;


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


/** This lock protects the FlashSim socket file. */
static env_mutex cache_device_lock;

char * io_buf ;
io_context_t ctx_;

//#define MAX_COUNT  65536
#define MAX_COUNT  32000
struct io_event events[MAX_COUNT];
struct timespec timeout;

// hacking io queue to avoid the lock overhead
#define IO_QUEUE_SIZE 100000000
uint64_t io_addrs[IO_QUEUE_SIZE];
static env_atomic last_io_pointer;
static env_atomic next_io_pointer;

int cache_sock_fd = 0;

/**
 * Submission thread runs separately. ARGS is a pointer to package id.
 */
static void *
_completion_thread_func(void *args)
{
    int pkg = *((int *) args);
    free((int *) args);

    DEBUG("SUBMIT: completion thread for package %d launched", pkg);

    double last_timestamp = 0.0;
    int counter = 0;
    int ret = 0;
     
    while (1) {
        ret = io_getevents(ctx_, 0, MAX_COUNT, events, &timeout);
        if (ret < 0) {
            printf("Getevents Error\n");
            exit(1);
        }

	if (ret == 0)	
	    continue;
        
	counter += ret;
        
	if (counter % 1000000 == 0) {
	    double cur_timestamp = get_cur_time_ms();
	    printf("Finished %d ios in last %f ms, throughput: %f iops\n", counter, cur_timestamp - last_timestamp,
			    counter / (cur_timestamp - last_timestamp) * 1000.0);
	    counter = 0;
	    last_timestamp = get_cur_time_ms();
        }
    }

    // Not reached.
    return NULL;
}

static void *
_submit_thread_func(void *args)
{
    int thread_id = *((int *) args);
    free((int *) args);

    DEBUG("SUBMIT: submission thread for package %d launched", thread_id);

    printf("====== Started a submit thread for submitting IOs\n");
    int counter = 0;
    int next_io = 0;
    while (1) {
	next_io = env_atomic_inc_return(&next_io_pointer);
	while (next_io >= env_atomic_read(&last_io_pointer)) ;
        next_io = next_io % IO_QUEUE_SIZE;

    
	struct iocb * p = (struct iocb *)malloc(sizeof(struct iocb));
        //io_prep_pread(p, vol_priv->sock_fd, io_buf, io->bytes * 2, io->addr);
        // this get higher bw, perhaps because the cache size is too small
	
        //io_prep_pread(p, cache_sock_fd, io_buf, 4096, (fastrand() % 1000000) * 4096);
        //io_prep_pread(p, cache_sock_fd, io_buf, 4096, (counter * (thread_id+1) % 1000000) * 4096);
	io_prep_pread(p, cache_sock_fd, io_buf, 4096, io_addrs[next_io]);
	p->data = (void *) io_buf;
    
        if (io_submit(ctx_, 1, &p) != 1) {
            io_destroy(ctx_);
            int errnum = errno;
	    printf("io submit error: %d %s\n", errnum, strerror( errnum ));
	    exit(1);
        }
        counter += 1;
	//printf("thread %d, to submit an IO: %d, %ld\n", thread_id, counter, (counter * thread_id % 1000000));
    }

    // Not reached.
    return NULL;
}


/*========== Cache Volume Operations Implemention BEGIN. ==========*/

/**
 * Open cache volume.
 * Here we store uuid as volume name and connect to FlashSim socket.
 *
 * At any time, at most CACHE_PARALLELISM requests are being processed
 * concurrently. This models in-device parallelism.
 */
static int
cache_vol_open(ocf_volume_t cache_vol, void *params)
{
    env_atomic_set(&next_io_pointer, -1);
    env_atomic_set(&last_io_pointer, 0);

    const struct ocf_volume_uuid *uuid = ocf_volume_get_uuid(cache_vol);
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(cache_vol);
    int ret, i;

    vol_priv->name = ocf_uuid_to_str(uuid);

    /** Open real device(file). */
    vol_priv->sock_name = cache_sock_name;
    printf("cache sock name: %s, size: %ld \n", cache_sock_name, cache_capacity_bytes);
    //remove(cache_sock_name);
    int fd = open(cache_sock_name, O_RDWR | O_DIRECT | O_CREAT, 0);      // O_DIRECT
    //int td = ftruncate(fd, cache_capacity_bytes);
    int td = 0;
    if (fd < 0 || td < 0) {
      printf("Raw Device Open failed\n");
      return 1;
    } 
    vol_priv->sock_fd = fd;
    cache_sock_fd = fd;

    /** Prepare for async I/Os. */
    io_buf = (char *) malloc(sizeof(char) * (4096 * 32));
    ret = posix_memalign((void **)&io_buf, 4096*32, 4096 * 32); 
    
    memset(&ctx_, 0, sizeof(ctx_));
    if (io_setup(MAX_COUNT, &ctx_) != 0) {
        printf("io_context_t set failed");
        exit(1);
    }
    timeout.tv_sec = 0;
    timeout.tv_nsec = 100;

    /** Start a I/O completion check threads. */
    pthread_t submit_thread_id;
    int *pkg_ptr;
    
    pkg_ptr = malloc(sizeof(int));
    *pkg_ptr = 1;

    ret = pthread_create(&submit_thread_id, NULL, _completion_thread_func, (void *)pkg_ptr);
    if (ret) {
        DEBUG("OPEN: submit thread %d creation failed", i);
        return ret;
    }
   

    for (int i = 0; i < 2; i++) {
        int *thread_id = malloc(sizeof(int));
        *thread_id = i;
        ret = pthread_create(&submit_thread_id, NULL, _submit_thread_func, (void *)thread_id);
        if (ret) {
            DEBUG("OPEN: submit thread %d creation failed", i);
            return ret;
	}
    } 
    //ret = pthread_create(&submit_thread_id, &submit_thread_attr,
    //                         _submit_thread_func, (void *) pkg_ptr);
    
    
    //ret = pthread_create(&submit_thread_id, &submit_thread_attr,
    //                         _submit_thread_func, (void *) pkg_ptr);

    DEBUG("OPEN: name = %s, sock = %s", vol_priv->name, vol_priv->sock_name);
    return 0;
}

/**
 * Close cache volume.
 * Here we simply close the socket.
 */
static void
cache_vol_close(ocf_volume_t cache_vol)
{
    /** Close real device(file). */
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(cache_vol);

    DEBUG("CLOSE: name = %s", vol_priv->name);

    close(vol_priv->sock_fd);

    ENV_BUG_ON(! list_empty(&submit_queue.head));

    env_mutex_destroy(&submit_queue_lock);
    env_completion_destroy(&submit_queue_sem);

    env_mutex_destroy(&cache_device_lock);
}

// Exposed for device logging.
extern double base_time_ms;

/**
 * Submit an IO request to volume.
 * Here we simply push to the tail of queue.
 */
unsigned int g_seed = 2333;
inline int fastrand() {
  g_seed = (214013*g_seed+2531011);
  return (g_seed>>16)&0x7FFF;
}
	
static void
cache_vol_submit_io(struct ocf_io *io)
{
    
    /** Address must be page-aligned. */
    if (io->addr % flashsim_page_size != 0) {
        DEBUG("IO: unaligned addr 0x%08lx", io->addr);
        io->end(io, 1);
        return;
    }
    
    if (io->bytes != 4096)
        printf("Submitting a special IO, size of %d\n", io->bytes);
    
    //printf("cache vol, to submit an IO %d, %ld \n", env_atomic_read(&last_io_pointer), io->addr);
    io_addrs[env_atomic_read(&last_io_pointer) % IO_QUEUE_SIZE] = io->addr; 
    env_atomic_inc(&last_io_pointer);

    
    //usleep(10000);
    io->end(io, 0);
    return;	
}

/**
 * Submit flush request.
 * Can be unimplemented if not needed.
 */
static void
cache_vol_submit_flush(struct ocf_io *io)
{
    io->end(io, 0);     /** Unimplemented. */
}

/**
 * Submit discard request.
 * Can be unimplemented if not needed.
 */
static void
cache_vol_submit_discard(struct ocf_io *io)
{
    io->end(io, 0);     /** Unimplemented. */
}


/**
 * Define the max I/O size for this volume.
 * Here we use 128KiB.
 */
static unsigned int
cache_vol_get_max_io_size(ocf_volume_t cache_vol)
{
    return CACHE_VOL_MAX_IO_SIZE;
}

/**
 * Get volume capacity.
 */
static uint64_t
cache_vol_get_length(ocf_volume_t cache_vol)
{
    return cache_capacity_bytes;
}


/**
 * Define how to setup a single I/O structure given OS data buffer
 * structure.
 */
static int
cache_vol_io_set_data(struct ocf_io *io, ctx_data_t *simfs_data,
                      uint32_t offset)
{
    cache_vol_io_priv_t *io_priv = ocf_io_get_priv(io);

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
cache_vol_io_get_data(struct ocf_io *io)
{
    cache_vol_io_priv_t *io_priv = ocf_io_get_priv(io);

    return io_priv->data;
}


/**
 * This structure assigns the above implementations to the OCF volume
 * interface. This structure essentially defines a volume type, which
 * can be used when setting up volumes during context initialization.
 */
const struct ocf_volume_properties cache_vol_properties = {
    .name = "Cache Volume",
    
    .io_priv_size = sizeof(cache_vol_io_priv_t),
    .volume_priv_size = sizeof(cache_vol_priv_t),

    .caps = {
        .atomic_writes = 0,
    },

    .ops = {
        .open = cache_vol_open,
        .close = cache_vol_close,
        .submit_io = cache_vol_submit_io,
        .submit_flush = cache_vol_submit_flush,
        .submit_discard = cache_vol_submit_discard,
        .get_max_io_size = cache_vol_get_max_io_size,
        .get_length = cache_vol_get_length,
    },

    .io_ops = {
        .set_data = cache_vol_io_set_data,
        .get_data = cache_vol_io_get_data,
    },
};

/*========== Cache Volume Operations Implemention END. ==========*/


/**
 * Indicate that submission threads should stop, without finishing pending
 * requests in queue.
 */
void
cache_vol_force_stop()
{
    struct req_entry *entry;
    struct ocf_io *io;

    env_mutex_lock(&submit_queue_lock);

    while (! list_empty(&submit_queue.head)) {
        entry = list_first_entry(&submit_queue.head,
                                 struct req_entry, head);
        io = entry->io;

        list_del(&entry->head);
        free(entry);

        io->end(io, 0);
    }

    env_mutex_unlock(&submit_queue_lock);

    env_atomic_inc(&should_stop);
}


/**
 * Registers the above structure as volume type CACHE_VOL_TYPE in
 * this OCF context.
 * Should be called just AFTER context initialization.
 */
int
cache_vol_register(ocf_ctx_t ctx)
{
    int ret = ocf_ctx_register_volume_type(ctx, CACHE_VOL_TYPE,
                                           &cache_vol_properties);

    DEBUG("REGISTER: as type = %d", CACHE_VOL_TYPE);

    return ret;
}

/**
 * Unregisters cache volume type.
 * Should be called just BEFORE context cleanup.
 */
void
cache_vol_unregister(ocf_ctx_t ctx)
{
    ocf_ctx_unregister_volume_type(ctx, CACHE_VOL_TYPE);

    DEBUG("UNREGISTER: done");
}
