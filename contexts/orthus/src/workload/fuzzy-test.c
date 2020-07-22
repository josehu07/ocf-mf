/**
 * Fuzzy test.
 *
 * Performs random reads & writes and checks correctness.
 * 
 * Assumes all volumes and context buffers start with all zeros filled.
 * Assumes callbacks are sequential, in-order, & synchronous.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ocf/ocf.h>

#include "common.h"
#include "simfs/simfs-ctx.h"
#include "cache/cache-obj.h"
#include "core/core-vol.h"
#include "fuzzy-test.h"


/** Absolute buffer holding data we expect. */
static char *abs_buf;

/** Counters for read operations. */
static int total_reads_count = 0;
static int valid_reads_count = 0;


/**
 * Debug printing.
 */
static inline void
debug(const char *fmt, ...)
{
    if (CTX_PRINT_DEBUG_MSG) {
        va_list args;

        printf("[WORKLOAD] ");

        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);

        printf("\n");
    }
}


/**
 * Do the write IO to absolute buffer.
 */
static inline void
write_to_absolute(char *data, uint64_t addr, uint32_t size)
{
    memcpy(abs_buf + addr, data, size);
}

/**
 * Compare data in absolute buffer with result data read from device.
 */
static inline void
validate_read_result(char *data, uint64_t addr, uint32_t size)
{
    total_reads_count++;

    if (memcmp(abs_buf + addr, data, size) != 0) {
        fprintf(stderr, " -> Core addr 0x%08lx of len %u\n"
                        "    Expect 1st sec: %s\n"
                        "    Actual 1st sec: %s\n",
                addr, size, abs_buf + addr, data);
    } else
        valid_reads_count++;
}


/**
 * Callback functions to be called when operation completes.
 */
static void
write_cmpl_callback(struct ocf_io *io, int error)
{
    simfs_data_t *data = ocf_io_get_data(io);

    /** On success, write to absolute buffer. */
    if (error == 0)
        write_to_absolute((char *) data->ptr, io->addr, io->bytes);
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

    /** Validate every read result. */
    if (error == 0) 
        validate_read_result((char *) data->ptr, io->addr, io->bytes);
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
 * Perform fuzzy test for a given number of IOs.
 */
int
perform_workload_fuzzy(ocf_core_t core, int num_ios)
{
    int i, ret;

    /** Must do at least 2000 IOs. */
    if (num_ios < 2000)
        return -1;

    /**
     * Allocate absolute buffer to hold the data we expect, same size
     * as the backend core device.
     */
    abs_buf = malloc(CORE_VOL_SIZE);
    if (abs_buf == NULL)
        return -ENOMEM;

    memset(abs_buf, 0, CORE_VOL_SIZE);

    total_reads_count = 0;
    valid_reads_count = 0;

    printf("Progress:\n");

    /** Loop and perform random IOs. */
    for (i = 0; i < num_ios; ++i) {
        int j, dir;
        uint32_t size;
        uint64_t addr;
        simfs_data_t *data;

        /** First 1000 IOs are writes (to somewhat fill the device). */
        if (i < 1000)
            dir = OCF_WRITE;
        else
            dir = rand() % 2 == 0 ? OCF_WRITE : OCF_READ;

        if (dir == OCF_WRITE) {
            /**
             * For writes, IO size varies from 512B to 128KiB, multiple of
             * sector size 512B.
             */
            size = ((rand() % 256) + 1) * 512;

            /**
             * Address is chosen from 0 to VOL_SIZE - size, aligned to pages
             * (i.e., cache lines).
             */
            addr = (rand() % (((CORE_VOL_SIZE - size) / PAGE_SIZE) + 1)) \
                   * PAGE_SIZE;

            /** Put in random alpha char data. */
            data = simfs_data_alloc((size / PAGE_SIZE) + 1);
            if (data == NULL)
                return -ENOMEM;

            for (j = 0; j < size - 1; ++j) {
                if ((j + 1) % 512 != 0)
                    *((char *) data->ptr + j) = (rand() % 26) + 97;
                else
                    *((char *) data->ptr + size - 1) = '\0';
            }

            ret = submit_io(core, data, addr, size, dir, write_cmpl_callback);
            if (ret)
                return ret;
            
        } else {
            /**
             * For reads, read an aligned page.
             */
            size = PAGE_SIZE;
            addr = (rand() % (CORE_VOL_SIZE / PAGE_SIZE)) * PAGE_SIZE;

            data = simfs_data_alloc(1);
            if (data == NULL)
                return -ENOMEM;

            ret = submit_io(core, data, addr, size, dir, read_cmpl_callback);
            if (ret)
                return ret;
        }

        /** Progress printing. */
        if ((i + 1) % 1000 == 0)
            printf(" ... %d / %d reqs\n", i + 1, num_ios);
    }

    /** Check whether 100% of the reads are consistent. */
    ENV_BUG_ON(total_reads_count <= 0);

    printf("Result:\n");

    if (valid_reads_count != total_reads_count) {
        printf(" --- FAIL ---  %d / %d valid reads :(\n",
               valid_reads_count, total_reads_count);
        return -2; 
    } else {
        printf(" --- PASS ---  %d / %d valid reads :)\n",
               valid_reads_count, total_reads_count);
        return 0;
    }
}
