/**
 * Core volume header.
 */


#ifndef __CORE_VOL_H__
#define __CORE_VOL_H__


#include <ocf/ocf.h>
#include "ocf_env.h"

#include "simfs/simfs-ctx.h"


#define CORE_VOL_TYPE (2)
#define CORE_VOL_SIZE (40*1024*1024)    /** Core size: 40MiB, 1/4 actual. */
#define CORE_VOL_MAX_IO_SIZE (32*1024)     /** Max I/O size: 32KiB. */


/**
 * Core volume private data definition.
 */
struct core_vol_priv {
    const char *name;
    const char *sock_name;
    int sock_fd;
};

typedef struct core_vol_priv core_vol_priv_t;


/**
 * Core volume single I/O structure definition.
 */
struct core_vol_io_priv {
    simfs_data_t *data;
    uint32_t offset;
};

typedef struct core_vol_io_priv core_vol_io_priv_t;


int core_vol_register(ocf_ctx_t ctx);
void core_vol_unregister(ocf_ctx_t ctx);


#endif
