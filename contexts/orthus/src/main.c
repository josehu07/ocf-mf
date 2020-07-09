/**
 * Simulating a Linux paged FS application context `simfs`.
 * 
 * Simulating a config of two volumes:
 *     - a small cache volume (32MiB)
 *     - a larger backend storage volume (64MiB)
 *     
 * Performing a simple workload:
 *     1. write a string
 *     2. read out the string
 *     3. compare
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache/cache-vol.h"
#include "cache/cache-obj.h"
#include "core/core-vol.h"
#include "core/core-obj.h"
#include "workload.h"


/**
 * Debug printing.
 */
static inline void
debug(const char *fmt, ...)
{
    va_list args;

    printf("[MAIN] ");

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
}

/**
 * Helper function for error handling.
 */
static void
error(const char *msg, int error)
{
    debug("ERROR: %s, code = %d", msg, error);
    exit(1);
}


/**
 * Main entrance for a round of testing.
 */
int
main(int argc, char *argv[])
{
    ocf_ctx_t ctx;
    ocf_cache_t cache;
    ocf_core_t core;
    int ret;

    /** 1. Initialize OCF context. */
    ret = simfs_ctx_init(&ctx);
    if (ret)
        error("Unable to initialize app context", ret);

    /** 2. Register two volume types. */
    ret = cache_vol_register(ctx);
    if (ret)
        error("Unable to register cache volume type", ret);

    ret = core_vol_register(ctx);
    if (ret)
        error("Unable to register core volume type", ret);

    /** 3. Setup cache object. */
    ret = cache_obj_setup(ctx, &cache);
    if (ret)
        error("Unable to initialize cache", ret);

    /** 4. Setup core object. */
    ret = core_obj_setup(cache, &core);
    if (ret)
        error("Unable to initialize core", ret);

    /** 5. Perform a simple workload. */
    ret = perform_workload(core);
    if (ret)
        error("Error when performing workload", ret);

    /** 6. Stop and detach core from cache. */
    ret = core_obj_stop(core);
    if (ret)
        error("Unable to stop core", ret);

    /** 7. Stop the cache. */
    ret = cache_obj_stop(cache);
    if (ret)
        error("Unable to stop cache", ret);

    /** 8. Unregister volume types. */
    core_vol_unregister(ctx);
    cache_vol_unregister(ctx);

    /** 9. Cleanup this context. */
    simfs_ctx_cleanup(ctx);

    return 0;
}
