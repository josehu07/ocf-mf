/**
 * Cache volume type header.
 */


#ifndef __CACHE_VOL_H__
#define __CACHE_VOL_H__


#include <ocf/ocf.h>
#include "ocf_env.h"

#include "simfs/simfs-ctx.h"


#define CACHE_VOL_TYPE (1)
#define CACHE_VOL_SIZE (32*1024*1024)   /** Cache volume size: 32MiB. */
#define CACHE_VOL_MAX_IO_SIZE (128*1024)    /** Max I/O size: 128KiB. */


/**
 * Cache volume private data definition.
 */
struct cache_vol_priv {
    const char *name;
    uint8_t *addr;
    uint64_t capacity;  /** Capacity in bytes. */
};

typedef struct cache_vol_priv cache_vol_priv_t;


/**
 * Cache volume single I/O structure definition.
 */
struct cache_vol_io_priv {
    simfs_data_t *data;
    uint32_t offset;
};

typedef struct cache_vol_io_priv cache_vol_io_priv_t;


int cache_vol_register(ocf_ctx_t ctx);
void cache_vol_unregister(ocf_ctx_t ctx);


#endif
