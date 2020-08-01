/**
 * Cache volume type implementation.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "common.h"
#include "cache-vol.h"


/*========== Cache Volume Operations Implemention BEGIN. ==========*/

/**
 * Open a volume.
 * Here we store uuid as volume name and allocate VOL_SIZE memory to
 * simulate a storage device.
 */
static int
cache_vol_open(ocf_volume_t cache_vol, void *params)
{
    const struct ocf_volume_uuid *uuid = ocf_volume_get_uuid(cache_vol);
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(cache_vol);

    vol_priv->name = ocf_uuid_to_str(uuid);
    vol_priv->addr = malloc(CACHE_VOL_SIZE);
    vol_priv->capacity = CACHE_VOL_SIZE;

    memset(vol_priv->addr, 0, CACHE_VOL_SIZE);

    DEBUG("OPEN: name = %s, mem = 0x%p, capacity = %lu",
          vol_priv->name, vol_priv->addr, vol_priv->capacity);

    return 0;
}

/**
 * Close cache volume.
 * Here we simply free up memory allocated in open().
 */
static void
cache_vol_close(ocf_volume_t cache_vol)
{
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(cache_vol);

    DEBUG("CLOSE: name = %s", vol_priv->name);

    free(vol_priv->addr);
}

/**
 * Handle read or write from or to the storage device.
 * Here we are simulating it with memcpy(). NOT doing size checks.
 */
static void
cache_vol_submit_io(struct ocf_io *io)
{
    simfs_data_t *data = ocf_io_get_data(io);
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(ocf_io_get_volume(io));

    switch (io->dir) {
    case OCF_WRITE:
        memcpy(vol_priv->addr + io->addr,
               data->ptr + data->offset, io->bytes);
        break;
    case OCF_READ:
        memcpy(data->ptr + data->offset,
               vol_priv->addr + io->addr, io->bytes);
        break;
    }

    DEBUG("IO: dir = %s, cache pos = 0x%08lx, len = %u",
          io->dir == OCF_WRITE ? "WR <-" : "RD ->", io->addr, io->bytes);

    io->end(io, 0);
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
    cache_vol_priv_t *vol_priv = ocf_volume_get_priv(cache_vol);

    return vol_priv->capacity;
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
