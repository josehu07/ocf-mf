/**
 * Simulating a Linux paged FS application context `simfs`.
 * 
 * Simulating a config of two volumes:
 *     - a small cache volume (32MiB)
 *     - a larger backend storage volume (64MiB)
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
#include "workload/simple.h"
#include "workload/fuzzy-test.h"


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

    /** 0. Set random seed. */
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
    ret = monitor_init(core);
    if (ret)
        error("Unable to start monitor thread", ret);

    /** 6. Perform workload. */
    ret = perform_workload_fuzzy(core, 12000);
    if (ret)
        error("Error when performing workload", ret);

    /** 7. Stop and detach core from cache. */
    ret = core_obj_stop(core);
    if (ret)
        error("Unable to stop core", ret);

    /** 8. Stop the cache. */
    ret = cache_obj_stop(cache);
    if (ret)
        error("Unable to stop cache", ret);

    /** 9. Unregister volume types. */
    core_vol_unregister(ctx);
    cache_vol_unregister(ctx);

    /** 10. Cleanup this context. */
    simfs_ctx_cleanup(ctx);

    return 0;
}
