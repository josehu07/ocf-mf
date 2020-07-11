/**
 * Multi-factor cache mode implementation.
 *
 * Writes always follow Write-Around (for now). Reads swtich between
 * cache & core according to `load_admit`. Reads populate lines into
 * cache only if `data_admit` is on (i.e., in workload probing stage).
 */


#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "../ocf_request.h"
#include "../utils/utils_io.h"
#include "../utils/utils_cache_line.h"
#include "../utils/utils_part.h"
#include "../concurrency/ocf_concurrency.h"
#include "../metadata/metadata.h"
#include "engine_debug.h"
#include "engine_rd.h"
#include "engine_pt.h"
#include "engine_inv.h"
#include "engine_bf.h"
#include "engine_common.h"
#include "cache_engine.h"
#include "engine_mf.h"


#define OCF_ENGINE_DEBUG_IO_NAME "mf"


/*========== Multi-factor read implementation BEGIN. ==========*/

static void _ocf_read_mf_to_cache_complete(struct ocf_request *req, int error)
{
    if (error)
        req->error |= error;

    if (req->error)
        inc_fallback_pt_error_counter(req->cache);

    /* Handle callback-caller race to let only one of the two complete the
     * request. Also, complete original request only if this is the last
     * sub-request to complete
     */
    if (env_atomic_dec_return(&req->req_remaining) == 0) {
        OCF_DEBUG_RQ(req, "TO_CACHE completion");

        if (req->error) {
            ocf_core_stats_cache_error_update(req->core, OCF_READ);
            ocf_engine_push_req_front_pt(req);
        } else {

            ocf_req_unlock(req);

            /* Complete request */
            req->complete(req, req->error);

            /* Free the request at the last point
             * of the completion path
             */
            ocf_req_put(req);
        }
    }
}

static inline void _ocf_read_mf_submit_to_cache(struct ocf_request *req)
{
    env_atomic_set(&req->req_remaining, ocf_engine_io_count(req));

    ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0, req->byte_length,
        ocf_engine_io_count(req), _ocf_read_mf_to_cache_complete);
}

static void _ocf_read_mf_to_core_complete(struct ocf_request *req, int error)
{
    struct ocf_cache *cache = req->cache;

    if (error)
        req->error = error;

    /* Handle callback-caller race to let only one of the two complete the
     * request. Also, complete original request only if this is the last
     * sub-request to complete
     */
    if (env_atomic_dec_return(&req->req_remaining) == 0) {
        OCF_DEBUG_RQ(req, "TO_CORE completion");

        if (req->error) {
            /*
             * --- Do not submit this request to write-back-thread.
             * Stop it here ---
             */
            req->complete(req, req->error);

            req->info.core_error = 1;
            ocf_core_stats_core_error_update(req->core, OCF_READ);

            ctx_data_free(cache->owner, req->cp_data);
            req->cp_data = NULL;

            /* Invalidate metadata */
            ocf_engine_invalidate(req);

            return;
        }

        /* Copy pages to copy vec, since this is the one needed
         * by the above layer
         */
        ctx_data_cpy(cache->owner, req->cp_data, req->data, 0, 0,
                req->byte_length);

        /* Complete request */
        req->complete(req, req->error);

        ocf_engine_backfill(req);
    }
}

static inline void _ocf_read_mf_submit_to_core(struct ocf_request *req)
{
    struct ocf_cache *cache = req->cache;
    int ret;

    env_atomic_set(&req->req_remaining, 1);

    req->cp_data = ctx_data_alloc(cache->owner,
            BYTES_TO_PAGES(req->byte_length));
    if (!req->cp_data) {
        _ocf_read_mf_to_core_complete(req, -OCF_ERR_NO_MEM);
        return;
    }

    ret = ctx_data_mlock(cache->owner, req->cp_data);
    if (ret) {
        _ocf_read_mf_to_core_complete(req, -OCF_ERR_NO_MEM);
        return;
    }

    /* Submit read request to core device. */
    ocf_submit_volume_req(&req->core->volume, req,
            _ocf_read_mf_to_core_complete);
}

static int _ocf_read_mf_do(struct ocf_request *req)
{
    if (ocf_engine_is_miss(req) && req->map->rd_locked) {
        /* Miss can be handled only on write locks.
         * Need to switch to PT
         */
        OCF_DEBUG_RQ(req, "Switching to PT");
        ocf_read_pt_do(req);
        return 0;
    }

    /* Get OCF request - increase reference counter */
    ocf_req_get(req);

    if (ocf_engine_is_miss(req)) {
        if (req->info.dirty_any) {
            ocf_req_hash_lock_rd(req);

            /* Request is dirty need to clean request */
            ocf_engine_clean(req);

            ocf_req_hash_unlock_rd(req);

            /* We need to clean request before processing, return */
            ocf_req_put(req);

            return 0;
        }

        ocf_req_hash_lock_rd(req);

        /* Set valid status bits map */
        ocf_set_valid_map_info(req);

        ocf_req_hash_unlock_rd(req);
    }

    if (req->info.re_part) {
        OCF_DEBUG_RQ(req, "Re-Part");

        ocf_req_hash_lock_wr(req);

        /* Probably some cache lines are assigned into wrong
         * partition. Need to move it to new one
         */
        ocf_part_move(req);

        ocf_req_hash_unlock_wr(req);
    }

    OCF_DEBUG_RQ(req, "Submit");

    /* Submit IO */
    if (ocf_engine_is_hit(req))
        _ocf_read_mf_submit_to_cache(req);
    else
        _ocf_read_mf_submit_to_core(req);

    /* Update statistics */
    ocf_engine_update_request_stats(req);
    ocf_engine_update_block_stats(req);

    /* Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

static const struct ocf_io_if _io_if_read_mf_resume = {
    .read = _ocf_read_mf_do,
    .write = _ocf_read_mf_do,
};

static enum ocf_engine_lock_type ocf_read_mf_get_lock_type(struct ocf_request *req)
{
    if (ocf_engine_is_hit(req))
        return ocf_engine_lock_read;
    else
        return ocf_engine_lock_write;
}

static const struct ocf_engine_callbacks _read_mf_engine_callbacks =
{
    .get_lock_type = ocf_read_mf_get_lock_type,
    .resume = ocf_engine_on_resume,
};

/**
 * Multi-factor read.
 * For now, it follows generic read from cache.
 */
int ocf_read_mf(struct ocf_request *req)
{
    int lock = OCF_LOCK_NOT_ACQUIRED;
    struct ocf_cache *cache = req->cache;

    ocf_io_start(&req->ioi.io);

    if (env_atomic_read(&cache->pending_read_misses_list_blocked)) {
        /* There are conditions to bypass IO */
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
        return 0;
    }

    /* Get OCF request - increase reference counter */
    ocf_req_get(req);

    /* Set resume call backs */
    req->io_if = &_io_if_read_mf_resume;

    lock = ocf_engine_prepare_clines(req, &_read_mf_engine_callbacks);

    if (!req->info.mapping_error) {
        if (lock >= 0) {
            if (lock != OCF_LOCK_ACQUIRED) {
                /* Lock was not acquired, need to wait for resume */
                OCF_DEBUG_RQ(req, "NO LOCK");
            } else {
                /* Lock was acquired can perform IO */
                _ocf_read_mf_do(req);
            }
        } else {
            OCF_DEBUG_RQ(req, "LOCK ERROR %d", lock);
            req->complete(req, lock);
            ocf_req_put(req);
        }
    } else {
        ocf_req_clear(req);
        ocf_get_io_if(ocf_cache_mode_pt)->read(req);
    }


    /* Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

/*========== Multi-factor read implementation END. ==========*/


/*========== Multi-factor write implementation BEGIN. ==========*/

static void _ocf_write_mf_complete(struct ocf_request *req, int error)
{
    if (error)
        req->error |= error;

    if (env_atomic_dec_return(&req->req_remaining))
        return;

    if (req->error) {
        req->info.core_error = 1;
        ocf_core_stats_core_error_update(req->core, OCF_WRITE);
    }

    /* Complete request */
    req->complete(req, req->error);

    OCF_DEBUG_RQ(req, "Completion");

    /* Release OCF request */
    ocf_req_put(req);
}

/**
 * Multi-factor write.
 * For now, it is the same as doing Write-Around.
 */
int ocf_write_mf(struct ocf_request *req)
{
    ocf_io_start(&req->ioi.io);

    /* Get OCF request - increase reference counter */
    ocf_req_get(req);

    ocf_req_hash(req);

    ocf_req_hash_lock_rd(req); /*- Metadata RD access -----------------------*/

    /* Traverse request to check if there are mapped cache lines */
    ocf_engine_traverse(req);

    ocf_req_hash_unlock_rd(req); /*- END Metadata RD access -----------------*/

    if (ocf_engine_is_hit(req)) {
        ocf_req_clear(req);

        /* There is HIT, do WT */
        ocf_get_io_if(ocf_cache_mode_wt)->write(req);

    } else if (ocf_engine_mapped_count(req)) {
        ocf_req_clear(req);

        /* Partial MISS, do WI */
        ocf_get_io_if(ocf_cache_mode_wi)->write(req);
    } else {

        /* There is no mapped cache line, write directly into core */

        OCF_DEBUG_RQ(req, "Submit");

        /* Submit write IO to the core */
        env_atomic_set(&req->req_remaining, 1);
        ocf_submit_volume_req(&req->core->volume, req,
                _ocf_write_mf_complete);

        /* Update statistics */
        ocf_engine_update_block_stats(req);
        ocf_core_stats_request_pt_update(req->core, req->part_id, req->rw,
                req->info.hit_no, req->core_line_count);
    }

    /* Put OCF request - decrease reference counter */
    ocf_req_put(req);

    return 0;
}

/*========== Multi-factor write implementation END. ==========*/
