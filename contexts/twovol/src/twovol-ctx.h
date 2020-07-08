/**
 * The `twovol` context header.
 * 
 * The interface type `ctx_data_t` in OCF is supposed to describe the
 * OS data buffer (occupying several data pages) for a piece of data
 * being I/Oed.
 *
 * It is typedefed as `void` in OCF interface. We cast it to our context-
 * specific `twovol_data_t` to hook up with OCF interface.
 */


#ifndef _TWOVOL_CTX_H_
#define _TWOVOL_CTX_H_


#include <ocf/ocf.h>


/**
 * The OS data buffer structure to be used in this context.
 */
struct twovol_data {
    void *ptr;
    int offset;
    uint32_t pages;     /** Total allocated size in pages. */
};

typedef struct twovol_data twovol_data_t;


/** Might be used in workload code. */
ctx_data_t *twovol_data_alloc(uint32_t pages);
void twovol_data_free(ctx_data_t *twovol_data);


int twovol_ctx_init(ocf_ctx_t *ctx);
void twovol_ctx_cleanup(ocf_ctx_t ctx);


#endif
