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
#include "core/core-vol.h"
#include "common.h"
#include "tp-hacking.h"


/**
 * Callback functions to be called when operation completes.
 */
static void
write_cmpl_callback(struct ocf_io *io, int error)
{
    if (error != 0)
        DEBUG("WR COMPLETE: error = %d", error);

    ocf_io_put(io);
}

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
 * Supports multi-factor caching algorithm.
 */
static int
which_page_workload_1st()
{
    const int cache_capacity = CACHE_VOL_SIZE / PAGE_SIZE;
    const int workload_size = (int) (1.25 * cache_capacity);

    if (rand() % 100 < 95)
        return rand() % cache_capacity;
    else
        return cache_capacity + (rand() % (workload_size - cache_capacity));
}

static int
which_page_workload_2nd()
{
    const int core_capacity = CORE_VOL_SIZE / PAGE_SIZE;
    const int cache_capacity = CACHE_VOL_SIZE / PAGE_SIZE;
    const int workload_size = (int) (1.25 * cache_capacity);

    if (rand() % 100 < 95)
        return core_capacity - rand() % cache_capacity - 1;
    else
        return core_capacity - workload_size - 1
               + (rand() % (workload_size - cache_capacity));
}

int
perform_workload_tp_hack(ocf_core_t core, int intensity, int secs)
{
    double delta_ms = 1000.0 / (double) intensity;
    int i, ret, total_pages = CORE_VOL_SIZE / PAGE_SIZE;
    simfs_data_t *data = simfs_data_alloc(1);

    if (data == NULL)
        return -ENOMEM;

    for (i = 0; i < PAGE_SIZE; ++i)
        *((char *) data->ptr + i) = (rand() % 26) + 97;

    /** Fill every page so that we can read any of them. */
    for (i = 0; i < total_pages; ++i) {
        int dir = OCF_WRITE;
        uint32_t size = PAGE_SIZE;
        uint64_t addr = i * PAGE_SIZE;

        ret = submit_io(core, data, addr, size, dir, write_cmpl_callback);
        if (ret)
            return ret;
    }

    printf("\nProgress:\n\n");

    /**
     * Loop and perform random reads, one page each, following the
     * defined workload pattern.
     */
    for (i = 0; i < secs * intensity; ++i) {
        int dir = OCF_READ;
        uint32_t size = PAGE_SIZE;
        uint64_t addr = which_page_workload_1st() * PAGE_SIZE;

        DEBUG("Perform IO #%d", i);

        ret = submit_io(core, data, addr, size, dir, read_cmpl_callback);
        if (ret)
            return ret;

        time_elapse_ms(delta_ms);

        /** Progress printing. */
        if ((i + 1) % 1000 == 0)
            printf("  ... %d reqs @ %.3lf ms\n", i + 1, get_cur_time_ms());
    }

    simfs_data_free(data);

    return 0;
}
