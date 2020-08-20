/**
 * Supports multi-factor caching algorithm.
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
#include "tp-hacking.h"

#include "../../../src/engine/engine_mf.h"


// Exposed for device logging.
double base_time_ms = 0.0;


/**
 * Callback functions to be called when operation completes.
 */
// static void
// write_cmpl_callback(struct ocf_io *io, int error)
// {
//     if (error != 0)
//         DEBUG("WR COMPLETE: error = %d", error);

//     ocf_io_put(io);
// }

static void
read_cmpl_callback(struct ocf_io *io, int error)
{
    if (error != 0)
        DEBUG("RD COMPLETE: error = %d", error);

    ocf_io_put(io);
}


/**
 * Wrapper function for I/O submission.
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
 * Supports multi-factor caching algorithm.
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

int
perform_workload_tp_hack(ocf_core_t core, int intensity,
                         int smashing_secs, int display_secs)
{
    // int total_pages = core_capacity_bytes / PAGE_SIZE;
    int i, ret, num_reqs = 0;
    double cur_time_ms, log_interval_ms = 0.0;

    /** Must have ENABLE_DATA == false when doing this benchmarking. */
    if (flashsim_enable_data) {
        fprintf(stderr, "Recommend having ENABLE_DATA option off "
                        "when benchmarking.\n");
        return -1;
    }

    /** Intensity must be a multiple of 10. */
    if (intensity % 10 != 0) {
        fprintf(stderr, "Intensity must be a multiple of 10.\n");
        return -1;
    }

    /**
     * We will issue 10 requests in a row every time the benchmarking code
     * wakes up. This makes the actual sleep time between wake ups more
     * reasonable.
     */
    intensity = (int) (intensity / 10);

    simfs_data_t *data = simfs_data_alloc(1);

    /**
     * ATTENTION: we are here compensating for extra overheads in the 
     * experiment code. If we don't do that, actual intensity will be
     * much lower than the value we set as intensity.
     */
    double delta_ms = (1000.0 / (double) intensity) * 0.94 - 0.08;

    // if (data == NULL)
    //     return -ENOMEM;

    // for (i = 0; i < PAGE_SIZE; ++i)
    //     *((char *) data->ptr + i) = (rand() % 26) + 97;

    // printf("\nFilling devices with random data...\n");

    // /** Fill every page so that we can read any of them. */
    // for (i = 0; i < total_pages; ++i) {
    //     int dir = OCF_WRITE;
    //     uint32_t size = PAGE_SIZE;
    //     uint64_t addr = i * PAGE_SIZE;

    //     ret = submit_io(core, data, addr, size, dir, write_cmpl_callback);
    //     if (ret)
    //         return ret;

    //     usleep(3000);
    // }

    printf("\nStart experiment... Progress:\n\n");

    /**
     * Loop and perform random reads, one page each, following the
     * defined workload pattern.
     */
    // printf("\n Workload #1:\n\n");

    base_time_ms = get_cur_time_ms();
    cur_time_ms = base_time_ms;
    log_interval_ms = 0.0;

    do {
        double fluctuation = 0.75 + ((double) rand() / RAND_MAX) * 0.5;

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

        for (i = 0; i < 10; ++i) {  /** 10 requests in a row. */
            int dir = OCF_READ;
            uint32_t size = PAGE_SIZE;
            uint64_t addr = which_page_workload_small() * PAGE_SIZE;

            ret = submit_io(core, data, addr, size, dir, read_cmpl_callback);
            if (ret)
                return ret;

            num_reqs++;
        }

        usleep((int) (delta_ms * fluctuation * 1000));
    } while (cur_time_ms < base_time_ms + 1000.0 * smashing_secs);

    do {
        cur_time_ms = get_cur_time_ms();

        printf("  ... #%d @ %.3lf ms: "
               "miss_ratio = %.5lf, "
               "load_admit = %.3lf, "
               "cache_tp = %.3lf, "
               "core_tp = %.3lf\n",
               num_reqs, cur_time_ms - base_time_ms,
               _get_miss_ratio(core),
               _get_load_admit(),
               _get_cache_throughput(cur_time_ms - 500.0,
                                     cur_time_ms),
               _get_core_throughput(cur_time_ms - 500.0,
                                    cur_time_ms));

        usleep(500 * 1000);
    } while (cur_time_ms < base_time_ms + 1000.0 * display_secs);

    /** Force stop. */
    cache_vol_force_stop();
    core_vol_force_stop();

    simfs_data_free(data);

    return 0;
}
