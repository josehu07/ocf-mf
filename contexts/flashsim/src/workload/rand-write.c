/**
 * Performs random writes to the device.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache/cache-obj.h"
#include "core/core-vol.h"
#include "rand-write.h"


extern bool CTX_PRINT_DEBUG_MSG;


/**
 * Debug printing.
 */
static inline void
debug(const char *fmt, ...)
{
    if (CTX_PRINT_DEBUG_MSG) {
        va_list args;

        printf("[ WORKLOAD] ");

        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);

        printf("\n");
    }
}


/**
 * Callback functions to be called when operation completes.
 */
static void
write_cmpl_callback(struct ocf_io *io, int error)
{
    simfs_data_t *data = ocf_io_get_data(io);

    if (error != 0)
        debug("WR COMPLETE: error = %d", error);

    /** Free data buffer and I/O structure in callback. */
    simfs_data_free(data);
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
 * Perform random writes to the device.
 */
int
perform_workload_rand_write(ocf_core_t core, int num_ios)
{
    int i, ret;

    printf("\nProgress:\n\n");

    /** Loop and perform sequential writes, one page each. */
    for (i = 0; i < num_ios; ++i) {
        int j, k, dir = OCF_WRITE;
        uint32_t size = PAGE_SIZE;
        uint64_t addr = (rand() % (CORE_VOL_SIZE / PAGE_SIZE)) * PAGE_SIZE;
        simfs_data_t *data = simfs_data_alloc(1);

        if (data == NULL)
            return -ENOMEM;

        debug("Perform IO #%d", i);

        /** Put in ID + random alpha char data in each sector. */
        for (j = 0; j < size / 512; ++j) {
            char id[20];
            char *sec_base = (char *) (data->ptr + data->offset)
                             + (j * 512);

            snprintf(id, 20, "%08d-%03d", i, j);

            *(sec_base + 0) = '<';
            memcpy(sec_base + 1, id, 12);
            *(sec_base + 13) = '>';

            for (k = 14; k < 511; ++k)
                *(sec_base + k) = (rand() % 26) + 97;

            *(sec_base + 511) = '\0';
        }

        ret = submit_io(core, data, addr, size, dir, write_cmpl_callback);
        if (ret)
            return ret;

        /** Progress printing. */
        if ((i + 1) % 1000 == 0)
            printf("  ... %d / %d reqs\n", i + 1, num_ios);
    }

    return 0;
}
