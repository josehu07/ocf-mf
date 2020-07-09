/**
 * Define the workload to test with.
 *
 * OCF users submit io to the core volume, and the engine will
 * interfere and execute desired caching logic in this process.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache/cache-obj.h"
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
 * Callback functions to be called when operation completes.
 */
static void
write_cmpl_callback(struct ocf_io *io, int error)
{
    simfs_data_t *data = ocf_io_get_data(io);

    if (data != NULL)
        debug("W COMPLETE: error = %d, str = \"%s\"", error,
              (char *) data->ptr);
    else
        debug("W COMPLETE: error = %d", error);

    /** Free data buffer and I/O structure in callback. */
    simfs_data_free(data);
    ocf_io_put(io);
}

static void
read_cmpl_callback(struct ocf_io *io, int error)
{
    simfs_data_t *data = ocf_io_get_data(io);

    if (data != NULL)
        debug("R COMPLETE: error = %d, str = \"%s\"", error,
              (char *) data->ptr);
    else
        debug("R COMPLETE: error = %d", error);

    /** Free data buffer and I/O structure in callback. */
    simfs_data_free(data);
    ocf_io_put(io);
}


/**
 * Wrapper function for I/O submission.
 */
static int
submit_io(ocf_core_t core, simfs_data_t *simfs_data, uint64_t addr,
          uint64_t len, int dir, ocf_end_io_t callback_func)
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
 * Perform the following simple workload:
 *     1. write a string
 *     2. read out the string
 *     3. compare
 */
int
perform_workload(ocf_core_t core)
{
    simfs_data_t *data1, *data2;
    int ret;

    /**
     * Setup the scenario where there is data to write in a Linux FS
     * data buffer `data1`.
     */
    data1 = simfs_data_alloc(1);
    if (data1 == NULL)
        return -ENOMEM;

    strcpy(data1->ptr, "<<< This is some test data ABCDEFGH ;) >>>");

    /** Prepare & submit I/O to core. */
    ret = submit_io(core, data1, 0, 512, OCF_WRITE, write_cmpl_callback);
    if (ret)
        return ret;

    /**
     * We should wait here until this write is complete.
     * TODO: Now since async queue is implemented as sync, we are not
     *       doing actual waiting.
     */
    
    /** Setup OS data buffer for reading out the data. */
    data2 = simfs_data_alloc(1);
    if (data2 == NULL)
        return -ENOMEM;

    /** Prepare & submit I/O to core. */
    submit_io(core, data2, 0, 512, OCF_READ, read_cmpl_callback);
    if (ret)
        return ret;

    return 0;
}
