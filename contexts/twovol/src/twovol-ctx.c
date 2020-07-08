/**
 * Defines the `twovol` context-specific operations to fulfill OCF
 * interface requirements.
 * 
 * The interface type `ctx_data_t` in OCF is supposed to describe the
 * OS data buffer (occupying several data pages) for a piece of data
 * being I/Oed.
 *
 * It is typedefed as `void` in OCF interface. We cast it to our context-
 * specific `twovol_data_t` to hook up with OCF interface.
 */


#include <execinfo.h>
#include <ocf/ocf.h>
#include "ocf_env.h"
#include "twovol-ctx.h"


/**
 * Calculate valid size not exceeding allocated size of buffer, starting
 * from current offset.
 * Used as a subroutine in several operations below.
 */
static uint32_t
valid_size_from_offset(twovol_data_t *data, uint32_t size)
{
    uint32_t available_size, valid_size;

    available_size = data->pages * PAGE_SIZE - data->offset;
    valid_size = available_size < size ? available_size : size;

    return valid_size;
}

/**
 * Calculate valid size not exceeding allocated size of buffer, starting
 * from current offset.
 * Used as a subroutine in several operations below.
 */
static uint32_t
valid_size_from_begin(twovol_data_t *data, uint32_t size)
{
    uint32_t available_size, valid_size;

    available_size = data->pages * PAGE_SIZE;
    valid_size = available_size < size ? available_size : size;

    return valid_size;
}


/******* OCF Context Operations Implemention BEGIN. *******/

/**
 * Allocate the OS data structure for an I/O, occupying specified number
 * of pages.
 * 
 * This function is not set to static as we might use it in the workload
 * code in main.c.
 */
ctx_data_t *
twovol_data_alloc(uint32_t pages)
{
    twovol_data_t *data;

    data = malloc(sizeof(twovol_data_t));
    data->ptr = malloc(pages * PAGE_SIZE);
    data->offset = 0;
    data->pages = pages;

    return data;
}

/**
 * Free the OS data structure.
 */
void
twovol_data_free(ctx_data_t *twovol_data)
{
    twovol_data_t *data = twovol_data;

    if (data != NULL) {
        free(data->ptr);
        free(data);
    }
}

/**
 * Supposed to set protection of data pages against swapping.
 * Can be unimplemented if not needed.
 */
static int
twovol_data_mlock(ctx_data_t *twovol_data)
{
    return 0;
}

/**
 * Stop protecting data pages against swapping.
 * Can be unimplemented if not needed.
 */
static void
twovol_data_munlock(ctx_data_t *twovol_data)
{
    return;
}

/**
 * Read data from OS data buffer into destination app location.
 */
static uint32_t
twovol_data_read(void *dst, ctx_data_t *twovol_data, uint32_t size)
{
    uint32_t read_size;

    twovol_data_t *data = twovol_data;

    read_size = valid_size_from_offset(data, size);
    memcpy(dst, data->ptr + data->offset, read_size);

    return read_size;
}

/**
 * Write data from source app location into OS data buffer.
 */
static uint32_t
twovol_data_write(ctx_data_t *twovol_data, const void *src, uint32_t size)
{
    uint32_t write_size;

    twovol_data_t *data = twovol_data;

    write_size = valid_size_from_offset(data, size);
    memcpy(data->ptr + data->offset, src, write_size);

    return write_size;
}

/**
 * Fill data buffer with zeros.
 */
static uint32_t
twovol_data_zero(ctx_data_t *twovol_data, uint32_t size)
{
    uint32_t zero_size;

    twovol_data_t *data = twovol_data;

    zero_size = valid_size_from_offset(data, size);
    memset(data->ptr + data->offset, 0, zero_size);

    return zero_size;
}

/**
 * Seek on data buffer, changing the offset.
 */
static uint32_t
twovol_data_seek(ctx_data_t *twovol_data, ctx_data_seek_t seek,
                 uint32_t size)
{
    uint32_t seek_size;

    twovol_data_t *data = twovol_data;

    switch (seek) {
    case ctx_data_seek_begin:
        seek_size = valid_size_from_begin(data, size);
        data->offset = seek_size;
        break;
    case ctx_data_seek_current:
        seek_size = valid_size_from_offset(data, size);
        data->offset += seek_size;
        break;
    }

    return seek_size;
}

/**
 * Copy from one data buffer to another. NOT performing size checks.
 */
static uint64_t
twovol_data_copy(ctx_data_t *twovol_data_dst, ctx_data_t *twovol_data_src,
              uint64_t dst_offset, uint64_t src_offset, uint64_t bytes)
{
    twovol_data_t *data_dst = twovol_data_dst;
    twovol_data_t *data_src = twovol_data_src;

    memcpy(data_dst->ptr + dst_offset, data_src->ptr + src_offset, bytes);

    return bytes;
}

/**
 * Supposed to perform secure erase of data (e.g., fill with zeros).
 * Can be unimplemented if not needed.
 */
static void
twovol_data_secure_erase(ctx_data_t *twovol_data)
{
    return;
}

/**
 * Initialize cleaner thread.
 * Cleaning refers to the background flushing process running by the
 * cache system automatically, controlled by a cleaning policy.
 */
static int
twovol_cleaner_init(ocf_cleaner_t cleaner)
{
    return 0;   /** Unimplemented. */
}

/**
 * Kick off cleaner thread.
 */
static void
twovol_cleaner_kick(ocf_cleaner_t cleaner)
{
    return;
}

/**
 * Stop cleaner thread.
 */
static void
twovol_cleaner_stop(ocf_cleaner_t cleaner)
{
    return;
}

/**
 * Initialize metadata updater thread. 
 * Metadata updater refers to ??? [TODO].
 */
static int
twovol_metadata_updater_init(ocf_metadata_updater_t metadata_updater)
{
    return 0;   /** Unimplemented. */
}

/**
 * Kick off metadata updater thread.
 */
static void
twovol_metadata_updater_kick(ocf_metadata_updater_t metadata_updater)
{
    return;
}

/**
 * Stop metadata updater thread.
 */
static void
twovol_metadata_updater_stop(ocf_metadata_updater_t metadata_updater)
{
    return;
}

/**
 * Provide interface for printing to log use by OCF internal functions.
 * The lower level, the more urgent.
 */
static int
twovol_logger_print(ocf_logger_t logger, ocf_logger_lvl_t lvl,
                 const char *fmt, va_list args)
{
    FILE *logfile;

    if (lvl > log_info)
        return 0;

    logfile = lvl <= log_warn ? stderr : stdout;

    return vfprintf(logfile, fmt, args);
}

#define STACK_TRACE_DEPTH (16)    /** Backtracing stack depth. */

/**
 * Provide interface for printing current stack trace. Copied from the
 * `simple` example.
 */
static int
twovol_logger_dump_stack(ocf_logger_t logger)
{
    void *trace[STACK_TRACE_DEPTH];
    char **messages = NULL;
    int i, size;

    size = backtrace(trace, STACK_TRACE_DEPTH);
    messages = backtrace_symbols(trace, size);

    printf("[stack trace]>>>\n");
    for (i = 0; i < size; ++i)
        printf("%s\n", messages[i]);
    printf("<<<[stack trace]\n");
    
    free(messages);

    return 0;
}

/******* OCF Context Operations Implemention END. *******/


/**
 * This structure assigns the above implementations to the OCF interface.
 * Interface functions are splitted into four categories.
 */
static const struct ocf_ctx_config twovol_cfg = {
    .name = "Two Volume Context",

    .ops = {
        .data = {
            .alloc = twovol_data_alloc,
            .free = twovol_data_free,
            .mlock = twovol_data_mlock,
            .munlock = twovol_data_munlock,
            .read = twovol_data_read,
            .write = twovol_data_write,
            .zero = twovol_data_zero,
            .seek = twovol_data_seek,
            .copy = twovol_data_copy,
            .secure_erase = twovol_data_secure_erase,
        },

        .cleaner = {
            .init = twovol_cleaner_init,
            .kick = twovol_cleaner_kick,
            .stop = twovol_cleaner_stop,
        },

        .metadata_updater = {
            .init = twovol_metadata_updater_init,
            .kick = twovol_metadata_updater_kick,
            .stop = twovol_metadata_updater_stop,
        },

        .logger = {
            .print = twovol_logger_print,
            .dump_stack = twovol_logger_dump_stack,
        },
    },
};


/**
 * Initialize the `twovol` context, assigning the above operation
 * implementations to the OCF interface. Volume types also registered
 * here.
 */
int
twovol_ctx_init(ocf_ctx_t *ctx)
{
    int ret;

    ret = ocf_ctx_create(ctx, &twovol_cfg);
    if (ret)
        return ret;

    // ret = volume_init(*ctx);
    // if (ret) {
    //     ocf_ctx_put(*ctx);
    //     return ret;
    // }

    return 0;
}

/**
 * Clean up the context. Deregisters volume types and deinitializes
 * the context.
 */
void
twovol_ctx_cleanup(ocf_ctx_t ctx)
{
    // volume_cleanup(ctx);
    ocf_ctx_put(ctx);
}
