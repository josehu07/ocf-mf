/**
 * Core volume type implementation.
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

#include "simfs/simfs-ctx.h"
#include "core-vol.h"


extern bool CTX_PRINT_DEBUG_MSG;

extern bool FLASHSIM_ENABLE_DATA;
extern unsigned long FLASHSIM_PAGE_SIZE;


/**
 * Debug printing.
 */
static inline void
debug(const char *fmt, ...)
{
    if (CTX_PRINT_DEBUG_MSG) {
        va_list args;

        printf("[ CORE VOL] ");

        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);

        printf("\n");
    }
}


/*========== Core Volume Operations Implemention BEGIN. ==========*/

/**
 * Open a volume.
 * Here we store uuid as volume name and connect to FlashSim socket.
 */
static int
core_vol_open(ocf_volume_t core_vol, void *params)
{
    const struct ocf_volume_uuid *uuid = ocf_volume_get_uuid(core_vol);
    core_vol_priv_t *vol_priv = ocf_volume_get_priv(core_vol);
    struct sockaddr_un saddr;

    vol_priv->name = ocf_uuid_to_str(uuid);
    
    /** Prepare socket here. */
    vol_priv->sock_name = "core-simssd";
    vol_priv->sock_fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (vol_priv->sock_fd < 0) {
        debug("OPEN: socket() failed");
        return 1;
    }

    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_LOCAL;
    strncpy(saddr.sun_path, vol_priv->sock_name, sizeof(saddr.sun_path) - 1);

    if (connect(vol_priv->sock_fd, (struct sockaddr *) &saddr,
                sizeof(saddr))) {
        debug("OPEN: connect() failed");
        return 2;
    }

    debug("OPEN: name = %s, sock = %s", vol_priv->name, vol_priv->sock_name);
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

    debug("CLOSE: name = %s", vol_priv->name);

    close(vol_priv->sock_fd);
}


/**
 * Request header (1st message) format.
 * Message size MUST exactly match in bytes!
 */
struct __attribute__((__packed__)) req_header {
    int           direction : 32;
    unsigned long addr      : 64;
    unsigned int  size      : 32;
    double        start_time;
};

static const size_t REQ_HEADER_LENGTH = 24;

static const int FLASHSIM_DIR_READ  = 0;
static const int FLASHSIM_DIR_WRITE = 1;

/**
 * Handle read or write from or to the storage device.
 */
static double
_submit_write_io(struct ocf_io *io, simfs_data_t *data, int sock_fd,
                 double start_time)
{
    struct req_header header;
    int rbytes, wbytes;
    char time_used_buf[8];
    double time_used;

    /** Request header. */
    header.direction = FLASHSIM_DIR_WRITE;
    header.addr = io->addr;
    header.size = io->bytes;
    header.start_time = start_time;

    wbytes = write(sock_fd, &header, REQ_HEADER_LENGTH);
    if (wbytes != REQ_HEADER_LENGTH) {
        debug("IO: write request header send failed");
        return -1;
    }

    /** Data to write, only if passing actual data. */
    if (FLASHSIM_ENABLE_DATA) {
        wbytes = write(sock_fd, data->ptr + data->offset, header.size);
        if (wbytes != (int) header.size) {
            debug("IO: write request data send failed");
            return -2;
        }
    }

    /** Processing time respond. */
    rbytes = read(sock_fd, time_used_buf, 8);
    if (rbytes != 8) {
        debug("IO: write processing time recv failed");
        return -3;
    }

    time_used = *((double *) time_used_buf);
    return time_used;
}

static double
_submit_read_io(struct ocf_io *io, simfs_data_t *data, int sock_fd,
                double start_time)
{
    struct req_header header;
    int rbytes, wbytes;
    char time_used_buf[8];
    double time_used;

    /** Request header. */
    header.direction = FLASHSIM_DIR_READ;
    header.addr = io->addr;
    header.size = io->bytes;
    header.start_time = start_time;

    wbytes = write(sock_fd, &header, REQ_HEADER_LENGTH);
    if (wbytes != REQ_HEADER_LENGTH) {
        debug("IO: read request header send failed");
        return -1;
    }

    /** Data read out, only if passing actual data. */
    if (FLASHSIM_ENABLE_DATA) {
        rbytes = read(sock_fd, data->ptr + data->offset, header.size);
        if (rbytes != (int) header.size) {
            debug("IO: read request data recv failed");
            return -2;
        }
    }

    /** Processing time respond. */
    rbytes = read(sock_fd, time_used_buf, 8);
    if (rbytes != 8) {
        debug("IO: read processing time recv failed");
        return -3;
    }

    time_used = *((double *) time_used_buf);
    return time_used;
}

static void
core_vol_submit_io(struct ocf_io *io)
{
    simfs_data_t *data = ocf_io_get_data(io);
    core_vol_priv_t *vol_priv = ocf_volume_get_priv(ocf_io_get_volume(io));
    double start_time = 789.0, time_used = 1.0;   /** TODO. */

    /** Address must be page-aligned. */
    if (io->addr % FLASHSIM_PAGE_SIZE != 0) {
        debug("IO: unaligned addr 0x%08lx", io->addr);
        io->end(io, 1);
        return;
    }

    debug("IO: dir = %s, cache pos = 0x%08lx, len = %u",
          io->dir == OCF_WRITE ? "WR <-" : "RD ->", io->addr, io->bytes);

    switch (io->dir) {
    case OCF_WRITE:
        time_used = _submit_write_io(io, data, vol_priv->sock_fd, start_time);
        break;
    case OCF_READ:
        time_used = _submit_read_io(io, data, vol_priv->sock_fd, start_time);
        break;
    }

    if (time_used <= 0.0)
        io->end(io, 2);
    else
        io->end(io, 0);
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

    debug("REGISTER: as type = %d", CORE_VOL_TYPE);

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

    debug("UNREGISTER: done");
}
