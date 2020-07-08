/**
 * The `twovol` application context.
 * 
 * This context is simulating a config of two volumes:
 *     - a small cache volume (16MiB)
 *     - a larger backend storage volume (64MiB)
 *     
 * Performing the workload:
 *     - TODO
 */


#include <stdio.h>
#include <stdlib.h>
#include <ocf/ocf.h>
#include "twovol-ctx.h"
#include "workload.h"


/**
 * Helper function for error handling.
 */
static void
error(char *msg)
{
    printf("ERROR: %s\n", msg);
    exit(1);
}


/**
 * Setup the cache object and attach cache device as a CACHE_VOL_TYPE
 * volume.
 * Should be called BEFORE `core_setup`.
 */
int
cache_setup(ocf_ctx_t ctx, ocf_cache_t *cache)
{
    return 0;
}

/**
 * Stop the cache.
 * Should be called AFTER `core_stop`.
 */
int
cache_stop(ocf_cache_t cache)
{
    return 0;
}


/**
 * Setup the core object, attach core device as a CORE_VOL_TYPE volume,
 * and link this core to the cache.
 */
int
core_setup(ocf_cache_t cache, ocf_core_t *core)
{
    return 0;
}

/**
 * Stop and detach core from cache.
 */
int
core_stop(ocf_core_t core)
{
    return 0;
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

    /** Initialize OCF context. */
    if (twovol_ctx_init(&ctx))
        error("Unable to initialize context `twovol`");

    /** Setup cache volume. */
    if (cache_setup(ctx, &cache))
        error("Unable to initialize cache");

    /** Setup core volume. */
    if (core_setup(cache, &core))
        error("Unable to initialize core");

    /** Perform a simple workload. */
    perform_workload(core);

    /** Stop and detach core from cache. */
    if (core_stop(core))
        error("Unable to remove core");

    /** Stop the cache. */
    if (cache_stop(cache))
        error("Unable to stop cache");

    /** Cleanup this context. */
    twovol_ctx_cleanup(ctx);

    return 0;
}
