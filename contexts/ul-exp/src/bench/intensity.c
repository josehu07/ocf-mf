/**
 * Benchmark - System action on given intensity.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache/cache-obj.h"
#include "cache/cache-vol.h"
#include "core/core-obj.h"
#include "core/core-vol.h"
#include "common.h"
#include "intensity.h"

#include "../../../src/engine/mf_monitor.h"


/** Proportion of reads. */
// double proportion_reads = 1.0;
double proportion_reads = 0.95;
// double proportion_reads = 0.5;
// double proportion_reads = 0.0;


// Exposed for device logging.
double base_time_ms = 0.0;


/**
 * Callback functions to be called when operation completes.
 */
static void
write_cmpl_callback(struct ocf_io *io, int error)
{
    if (error != 0)
        DEBUG("WR COMPLETE: error = %d", error);

    simfs_data_t *data = ocf_io_get_data(io);
    simfs_data_free(data);

    ocf_io_put(io);
}

static void
read_cmpl_callback(struct ocf_io *io, int error)
{
    if (error != 0)
        DEBUG("RD COMPLETE: error = %d", error);

    simfs_data_t *data = ocf_io_get_data(io);
    simfs_data_free(data);

    ocf_io_put(io);
}


/**
 * For result plotting purpose.
 */
static inline double
_get_miss_ratio(ocf_core_t core)
{
    return ocf_core_get_read_miss_ratio(core);
}

static inline double
_get_load_admit()
{
    return monitor_query_load_admit();
}

static inline double
_get_cache_throughput(double begin_time_ms, double end_time_ms)
{
    return cache_log_query_throughput(begin_time_ms, end_time_ms);
}

static inline double
_get_core_throughput(double begin_time_ms, double end_time_ms)
{
    return core_log_query_throughput(begin_time_ms, end_time_ms);
}


/**
 * Define a workload - choosing a page.
 */
static int
which_page_workload_small()
{
    const int cache_capacity = cache_capacity_bytes / PAGE_SIZE;
    const int workload_size = (int) (0.1 * cache_capacity);

    return rand() % workload_size;
}

// static int
// which_page_workload_1st()
// {
//     const int cache_capacity = cache_capacity_bytes / PAGE_SIZE;
//     const int workload_size = (int) (1.25 * cache_capacity);

//     if (rand() % 100 < 95)
//         return rand() % cache_capacity;
//     else
//         return cache_capacity + (rand() % (workload_size - cache_capacity));
// }

// static int
// which_page_workload_2nd()
// {
//     const int core_capacity = core_capacity_bytes / PAGE_SIZE;
//     const int cache_capacity = cache_capacity_bytes / PAGE_SIZE;
//     const int workload_size = (int) (1.25 * cache_capacity);

//     if (rand() % 100 < 95)
//         return core_capacity - rand() % cache_capacity - 1;
//     else
//         return core_capacity - workload_size - 1
//                + (rand() % (workload_size - cache_capacity));
// }


/**
 * Wrapper functions for I/O submission.
 */
static int
submit_io(ocf_core_t core, simfs_data_t *simfs_data, uint64_t addr,
          uint32_t len, int dir, ocf_end_io_t callback_func)
{
    ocf_cache_t cache = ocf_core_get_cache(core);
    cache_obj_priv_t *cache_obj_priv = ocf_cache_get_priv(cache);
    struct ocf_io* io;

    /** Allocate new I/O in queue. */
    io = ocf_core_new_io(core, cache_obj_priv->io_queue, addr,
                         len, dir, 0, 0);
    if (io == NULL)
        return -ENOMEM;

    /** Assign data to I/O. */
    ocf_io_set_data(io, simfs_data, 0);

    /** Assign completion callback function. */
    ocf_io_set_cmpl(io, NULL, NULL, callback_func);

    /** Submit this I/O. */
    ocf_core_submit_io(io);

    return 0;
}

static int
submit_10_ios_in_a_row(ocf_core_t core)
{
    int i, ret;

    for (i = 0; i < 10; ++i) {
        int dir;

        if ((((double) rand()) / ((double) RAND_MAX)) < proportion_reads)
            dir = OCF_READ;
        else
            dir = OCF_WRITE;

        uint32_t size = PAGE_SIZE;
        uint64_t addr = which_page_workload_small() * PAGE_SIZE;

        simfs_data_t *data = simfs_data_alloc(1);

        DEBUG("ISSUE: dir = %s, core pos = 0x%08lx, len = %u",
              dir == OCF_WRITE ? "WR <-" : "RD ->", addr, size);

        ret = submit_io(core, data, addr, size, dir,
                        dir == OCF_READ ? read_cmpl_callback
                                        : write_cmpl_callback);
        if (ret)
            return ret;
    }

    return 0;
}


int
bench_intensity(ocf_core_t core, int num_args, char **bench_args)
{
    int intensity;
    
    if (num_args != 1) {
        fprintf(stderr, "Wrong number of arguments\n");
        return -1;
    }

    intensity = (int) strtol(bench_args[0], NULL, 10);

    printf("\nExperiment parameters:\n\n");
    printf("  Intensity: %d 4KiB-Reqs/s\n", intensity);

    /** Must have ENABLE_DATA == false when doing this benchmarking. */
    if (flashsim_enable_data) {
        fprintf(stderr, "Recommend having PAGE_ENABLE_DATA option off "
                        "when benchmarking.\n");
        return -1;
    }

    /** Intensity must be a multiple of 10. */
    if (intensity % 10 != 0) {
        fprintf(stderr, "Intensity must be a multiple of 10.\n");
        return -2;
    }

    int ret;

    /**
     * We will issue 10 requests in a row every time the benchmarking code
     * wakes up. This makes the actual sleep time between wake ups more
     * reasonable.
     */
    intensity /= 10;
    double delta_ms = (1000.0 / (double) intensity);

    base_time_ms = get_cur_time_ms();
    double cur_time_ms = base_time_ms;
    double log_interval_ms = 0.0;

    int num_reqs = 0;

    /**
     * Stage 1 -
     *   The first 30 secs are totally unstable.
     */
    printf("\nBegin stabilizing stage... (0 - 30 secs)\n\n");

    do {
        double new_time_ms = get_cur_time_ms();
        log_interval_ms += new_time_ms - cur_time_ms;
        cur_time_ms = new_time_ms;

        if (log_interval_ms > 500.0) {
            printf("  *** #%d @ %.3lf ms: "
                   "miss_ratio = %.5lf, "
                   "load_admit = %.3lf, "
                   "cache_tp = %.3lf, "
                   "core_tp = %.3lf\n",
                   num_reqs, cur_time_ms - base_time_ms,
                   _get_miss_ratio(core),
                   _get_load_admit(),
                   _get_cache_throughput(cur_time_ms - log_interval_ms,
                                         cur_time_ms),
                   _get_core_throughput(cur_time_ms - log_interval_ms,
                                        cur_time_ms));

            log_interval_ms = 0.0;
        }

        ret = submit_10_ios_in_a_row(core);
        if (ret)
            return ret;

        usleep((int) (delta_ms * 1000));
    } while (cur_time_ms < base_time_ms + 1000.0 * 30);

    /**
     * Stage 2 -
     *   Measure delta overhead in 30 - 60 secs region. Then, adjust delta
     *   to be accurate.
     */
    printf("\nMeasuring delta overhead... (30 - 60 secs)\n\n");

    double avg_submit_elapsed_ms = 0.0;
    int count = 0;

    do {
        double submit_start_ms = get_cur_time_ms();

        double new_time_ms = get_cur_time_ms();
        log_interval_ms += new_time_ms - cur_time_ms;
        cur_time_ms = new_time_ms;

        if (log_interval_ms > 500.0) {
            printf("  ??? #%d @ %.3lf ms: "
                   "miss_ratio = %.5lf, "
                   "load_admit = %.3lf, "
                   "cache_tp = %.3lf, "
                   "core_tp = %.3lf\n",
                   num_reqs, cur_time_ms - base_time_ms,
                   _get_miss_ratio(core),
                   _get_load_admit(),
                   _get_cache_throughput(cur_time_ms - log_interval_ms,
                                         cur_time_ms),
                   _get_core_throughput(cur_time_ms - log_interval_ms,
                                        cur_time_ms));

            log_interval_ms = 0.0;
        }

        ret = submit_10_ios_in_a_row(core);
        if (ret)
            return ret;

        avg_submit_elapsed_ms += get_cur_time_ms() - submit_start_ms;
        count++;

        usleep((int) (delta_ms * 1000));
    } while (cur_time_ms < base_time_ms + 1000.0 * 60);

    avg_submit_elapsed_ms /= count;
    delta_ms -= avg_submit_elapsed_ms;

    /**
     * Stage 3 -
     *   Perform the accurate experiment for 100 secs.
     */
    printf("\nStart the experiment... (60 - 160 secs)\n\n");

    do {
        double new_time_ms = get_cur_time_ms();
        log_interval_ms += new_time_ms - cur_time_ms;
        cur_time_ms = new_time_ms;

        if (log_interval_ms > 500.0) {
            printf("  ... #%d @ %.3lf ms: "
                   "miss_ratio = %.5lf, "
                   "load_admit = %.3lf, "
                   "cache_tp = %.3lf, "
                   "core_tp = %.3lf\n",
                   num_reqs, cur_time_ms - base_time_ms,
                   _get_miss_ratio(core),
                   _get_load_admit(),
                   _get_cache_throughput(cur_time_ms - log_interval_ms,
                                         cur_time_ms),
                   _get_core_throughput(cur_time_ms - log_interval_ms,
                                        cur_time_ms));

            log_interval_ms = 0.0;
        }

        ret = submit_10_ios_in_a_row(core);
        if (ret)
            return ret;

        num_reqs += 10;

        usleep((int) (delta_ms * 1000));
    } while (cur_time_ms < base_time_ms + 1000.0 * 160);

    /**
     * Stage 4 -
     *   Wait for some extra secs.
     */
    printf("\nWait for extra secs... (160 - 180 secs)\n\n");

    do {
        double new_time_ms = get_cur_time_ms();
        log_interval_ms += new_time_ms - cur_time_ms;
        cur_time_ms = new_time_ms;

        if (log_interval_ms > 500.0) {
            printf("  ~~~ #%d @ %.3lf ms: "
                   "miss_ratio = %.5lf, "
                   "load_admit = %.3lf, "
                   "cache_tp = %.3lf, "
                   "core_tp = %.3lf\n",
                   num_reqs, cur_time_ms - base_time_ms,
                   _get_miss_ratio(core),
                   _get_load_admit(),
                   _get_cache_throughput(cur_time_ms - log_interval_ms,
                                         cur_time_ms),
                   _get_core_throughput(cur_time_ms - log_interval_ms,
                                        cur_time_ms));

            log_interval_ms = 0.0;
        }

        usleep((int) (delta_ms * 1000));
    } while (cur_time_ms < base_time_ms + 1000.0 * 180);

    /** Force device volume submission threads to stop. */
    cache_vol_force_stop();
    core_vol_force_stop();

    return 0;
}
