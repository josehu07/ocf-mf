/**
 * Simulating a Linux paged FS application context `simfs`.
 * 
 * Simulating a config of two volumes:
 *     - a small cache volume (32MiB)
 *     - a larger backend storage volume (64MiB)
 *     
 * Performing a simple workload:
 *     1. write a string A
 *     2. read out string A
 *     3. write a string B to the same address
 *     4. read out string B
 *     5. write a string C to a new address (no overlapping lines)
 *     6. read out string C
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache/cache-vol.h"
#include "cache/cache-obj.h"
#include "core/core-vol.h"
#include "core/core-obj.h"
#include "monitor/monitor.h"
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

    srand(time(NULL));

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

    /** 5. Init and start the monitor. */
    ret = monitor_init();
    if (ret)
        error("Unable to start monitor thread", ret);

    /** 6. Perform a simple workload. */
    ret = perform_workload(core);
    if (ret)
        error("Error when performing workload (1)", ret);

    /**
     * 7. Wait several secs to let the monitor kick in and make
     *    a config change.
     */
    sleep(2);

    /** 8. Perform the workload again to check config change. */
    ret = perform_workload(core);
    if (ret)
        error("Error when performing workload (2)", ret);

    /** 9. Stop and detach core from cache. */
    ret = core_obj_stop(core);
    if (ret)
        error("Unable to stop core", ret);

    /** 10. Stop the cache. */
    ret = cache_obj_stop(cache);
    if (ret)
        error("Unable to stop cache", ret);

    /** 11. Unregister volume types. */
    core_vol_unregister(ctx);
    cache_vol_unregister(ctx);

    /** 12. Cleanup this context. */
    simfs_ctx_cleanup(ctx);

    return 0;
}
