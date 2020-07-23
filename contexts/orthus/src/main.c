/**
 * An example context of using the OCF library.
 * 
 * Upwards: simulating a simple paged FS application context `simfs`.
 * 
 * Downwards: simulating an in-memory device config of two volumes:
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
#include "workload/fuzzy-test.h"


const bool CTX_PRINT_DEBUG_MSG = false;
const bool OCF_LOGGER_INFO_MSG = false;


/**
 * Helper function for error handling.
 */
static void
error(const char *msg, int error)
{
    fprintf(stderr, "ERROR: %s, code = %d\n", msg, error);
    exit(1);
}


/**
 * Display collected statistics.
 */
static void
print_statistics(struct ocf_stats_usage *stats_usage,
                 struct ocf_stats_requests *stats_reqs,
                 struct ocf_stats_blocks *stats_blocks,
                 struct ocf_stats_errors *stats_errors)
{
    printf("\nStatistics:\n\n"
           "   usage | cache |  occupied   %8lu pages    %3lu.%02lu %%\n"
           "         |       |      free   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     clean   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     dirty   %8lu pages    %3lu.%02lu %%\n"
           "\n"
           "  blocks | cache |   read ->   %8lu pages    %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     total   %8lu pages    %3lu.%02lu %%\n"
           "         |  core |   read ->   %8lu pages    %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     total   %8lu pages    %3lu.%02lu %%\n"
           "         |     total           %8lu pages    %3lu.%02lu %%\n"
           "\n"
           "    reqs |  read |     hit $   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | part miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | full miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |     total   %8lu reqs     %3lu.%02lu %%\n"
           "         | write |    hits $   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | part miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | full miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |     total   %8lu reqs     %3lu.%02lu %%\n"
           "         |   pt  |   read ->   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | writes <-   %8lu reqs     %3lu.%02lu %%\n"
           "         |     total           %8lu reqs     %3lu.%02lu %%\n"
           "\n"
           "  errors | cache |   read ->   %8lu errors   %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu errors   %3lu.%02lu %%\n"
           "         |  core |   read ->   %8lu errors   %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu errors   %3lu.%02lu %%\n"
           "         |     total           %8lu errors   %3lu.%02lu %%\n",
           stats_usage->occupancy.value,
           stats_usage->occupancy.fraction / 100,
           stats_usage->occupancy.fraction % 100,
           stats_usage->free.value,
           stats_usage->free.fraction / 100,
           stats_usage->free.fraction % 100,
           stats_usage->clean.value,
           stats_usage->clean.fraction / 100,
           stats_usage->clean.fraction % 100,
           stats_usage->dirty.value,
           stats_usage->dirty.fraction / 100,
           stats_usage->dirty.fraction % 100,
           stats_blocks->cache_volume_rd.value,
           stats_blocks->cache_volume_rd.fraction / 100,
           stats_blocks->cache_volume_rd.fraction % 100,
           stats_blocks->cache_volume_wr.value,
           stats_blocks->cache_volume_wr.fraction / 100,
           stats_blocks->cache_volume_wr.fraction % 100,
           stats_blocks->cache_volume_total.value,
           stats_blocks->cache_volume_total.fraction / 100,
           stats_blocks->cache_volume_total.fraction % 100,
           stats_blocks->core_volume_rd.value,
           stats_blocks->core_volume_rd.fraction / 100,
           stats_blocks->core_volume_rd.fraction % 100,
           stats_blocks->core_volume_wr.value,
           stats_blocks->core_volume_wr.fraction / 100,
           stats_blocks->core_volume_wr.fraction % 100,
           stats_blocks->core_volume_total.value,
           stats_blocks->core_volume_total.fraction / 100,
           stats_blocks->core_volume_total.fraction % 100,
           stats_blocks->volume_total.value,
           stats_blocks->volume_total.fraction / 100,
           stats_blocks->volume_total.fraction % 100,
           stats_reqs->rd_hits.value,
           stats_reqs->rd_hits.fraction / 100,
           stats_reqs->rd_hits.fraction % 100,
           stats_reqs->rd_partial_misses.value,
           stats_reqs->rd_partial_misses.fraction / 100,
           stats_reqs->rd_partial_misses.fraction % 100,
           stats_reqs->rd_full_misses.value,
           stats_reqs->rd_full_misses.fraction / 100,
           stats_reqs->rd_full_misses.fraction % 100,
           stats_reqs->rd_total.value,
           stats_reqs->rd_total.fraction / 100,
           stats_reqs->rd_total.fraction % 100,
           stats_reqs->wr_hits.value,
           stats_reqs->wr_hits.fraction / 100,
           stats_reqs->wr_hits.fraction % 100,
           stats_reqs->wr_partial_misses.value,
           stats_reqs->wr_partial_misses.fraction / 100,
           stats_reqs->wr_partial_misses.fraction % 100,
           stats_reqs->wr_full_misses.value,
           stats_reqs->wr_full_misses.fraction / 100,
           stats_reqs->wr_full_misses.fraction % 100,
           stats_reqs->wr_total.value,
           stats_reqs->wr_total.fraction / 100,
           stats_reqs->wr_total.fraction % 100,
           stats_reqs->rd_pt.value,
           stats_reqs->rd_pt.fraction / 100,
           stats_reqs->rd_pt.fraction % 100,
           stats_reqs->wr_pt.value,
           stats_reqs->wr_pt.fraction / 100,
           stats_reqs->wr_pt.fraction % 100,
           stats_reqs->total.value,
           stats_reqs->total.fraction / 100,
           stats_reqs->total.fraction % 100,
           stats_errors->cache_volume_rd.value,
           stats_errors->cache_volume_rd.fraction / 100,
           stats_errors->cache_volume_rd.fraction % 100,
           stats_errors->cache_volume_wr.value,
           stats_errors->cache_volume_wr.fraction / 100,
           stats_errors->cache_volume_wr.fraction % 100,
           stats_errors->core_volume_rd.value,
           stats_errors->core_volume_rd.fraction / 100,
           stats_errors->core_volume_rd.fraction % 100,
           stats_errors->core_volume_wr.value,
           stats_errors->core_volume_wr.fraction / 100,
           stats_errors->core_volume_wr.fraction % 100,
           stats_errors->total.value,
           stats_errors->total.fraction / 100,
           stats_errors->total.fraction % 100);
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
    struct ocf_stats_usage stats_usage;
    struct ocf_stats_requests stats_reqs;
    struct ocf_stats_blocks stats_blocks;
    struct ocf_stats_errors stats_errors;
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
    ret = ocf_mngt_mf_monitor_init(core);
    if (ret)
        error("Unable to start monitor thread", ret);

    /** 6. Perform workload. */
    perform_workload_fuzzy(core, 36000);

    /** 7. Stop the multi-factor monitor. */
    ocf_mngt_mf_monitor_stop();

    /** 8. Collect & show statistics. */
    ocf_stats_collect_core(core, &stats_usage, &stats_reqs,
                           &stats_blocks, &stats_errors);
    print_statistics(&stats_usage, &stats_reqs,
                     &stats_blocks, &stats_errors);

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
