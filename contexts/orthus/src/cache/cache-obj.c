/**
 * Cache logical object implementation.
 *
 * After volume types have been registered, a logical object should then
 * be created, and a volume of some registered type be attached to the
 * object.
 */


#include <stdio.h>
#include <stdlib.h>
#include <ocf/ocf.h>

#include "queue.h"
#include "cache-vol.h"
#include "cache-obj.h"


/**
 * Debug printing.
 */
static inline void
debug(const char *fmt, ...)
{
    va_list args;

    printf("[CACHE OBJ] ");

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
}


/**
 * Callback states shared between OCF routines and callbacks during
 * cache setup. This callback function will be assigned to any OCF
 * routine executed during cache setup.
 */
struct cache_setup_callback_states {
    int *error;     /** Pointer to host's return value. */
};

static void
cache_setup_callback(ocf_cache_t cache, void *callback_states,
                     int error)
{
    struct cache_setup_callback_states *states = callback_states;

    *states->error = error;
}


/**
 * Callback states shared between OCF routines and callbacks during
 * cache stop. This callback function will be assigned to any OCF
 * routine executed during cache stop.
 */
struct cache_stop_callback_states {
    int *error;     /** Pointer to host's return value. */
};

static void
cache_stop_callback(ocf_cache_t cache, void *callback_states,
                    int error)
{
    struct cache_stop_callback_states *states = callback_states;

    *states->error = error;
}


/**
 * Setup the cache object and attach cache device as a CACHE_VOL_TYPE
 * volume. Using the default cache config here.
 * Should be called BEFORE `core_setup`.
 */
int
cache_obj_setup(ocf_ctx_t ctx, ocf_cache_t *cache)
{
    struct ocf_mngt_cache_config cache_cfg = { .name = "cache" };
    struct ocf_mngt_cache_device_config device_cfg;
    cache_obj_priv_t *cache_obj_priv;
    struct cache_setup_callback_states callback_states;
    int ret;

    /** Let the callback state point to this functions return value. */
    callback_states.error = &ret;

    /**
     * Set cache configuration to default. Default config details can
     * be found in ocf_def.h. Look for the `_default` field in each
     * enum.
     * 
     * We are using memory to simulate a cache device, so we set the
     * volatile property to true.
     */
    ocf_mngt_cache_config_set_default(&cache_cfg);
    cache_cfg.metadata_volatile = true;

    /**
     * Set cache device configuration to default, and assign volume type
     * as CACHE_VOL_TYPE.
     */
    ocf_mngt_cache_device_config_set_default(&device_cfg);
    device_cfg.volume_type = CACHE_VOL_TYPE;
    ret = ocf_uuid_set_str(&device_cfg.uuid, "cache");
    if (ret)
        return ret;

    /** Allocate cache object private data. */
    cache_obj_priv = malloc(sizeof(cache_obj_priv_t));
    if (cache_obj_priv == NULL)
        return -ENOMEM;

    /** Start the cache. */
    ret = ocf_mngt_cache_start(ctx, cache, &cache_cfg);
    if (ret) {
        free(cache_obj_priv);
        return ret;
    }

    /* Assign cache private structure to cache object. */
    ocf_cache_set_priv(*cache, cache_obj_priv);

    /**
     * Create management queue, used for asynchronous management
     * oprations, such as attaching volume or adding core object.
     */
    ret = ocf_queue_create(*cache, &cache_obj_priv->mngt_queue,
                           &queue_ops);
    if (ret) {
        ocf_mngt_cache_stop(*cache, cache_setup_callback,
                            &callback_states);
        free(cache_obj_priv);
        return ret;
    }

    /** Assign management queue to cache. */
    ocf_mngt_cache_set_mngt_queue(*cache, cache_obj_priv->mngt_queue);

    /** Create I/O queue, used for I/O submission. */
    ret = ocf_queue_create(*cache, &cache_obj_priv->io_queue,
                           &queue_ops);
    if (ret) {
        ocf_mngt_cache_stop(*cache, cache_setup_callback,
                            &callback_states);
        ocf_queue_put(cache_obj_priv->mngt_queue);
        free(cache_obj_priv);
        return ret;
    }

    /** Attach the cache volume to cache object. */
    ocf_mngt_cache_attach(*cache, &device_cfg, cache_setup_callback,
                          &callback_states);
    if (ret) {
        ocf_mngt_cache_stop(*cache, cache_setup_callback,
                            &callback_states);
        ocf_queue_put(cache_obj_priv->mngt_queue);
        free(cache_obj_priv);
        return ret;
    }

    debug("SETUP: done");

    return 0;
}

/**
 * Stop the cache.
 * Should be called AFTER `core_stop`.
 */
int
cache_obj_stop(ocf_cache_t cache)
{
    cache_obj_priv_t *cache_obj_priv;
    struct cache_stop_callback_states callback_states;
    int ret = 0;

    /** Let the callback state point to this functions return value. */
    callback_states.error = &ret;

    ocf_mngt_cache_stop(cache, cache_stop_callback,
                        &callback_states);
    if (ret)
        return ret;

    cache_obj_priv = ocf_cache_get_priv(cache);
    ocf_queue_put(cache_obj_priv->mngt_queue);
    free(cache_obj_priv);

    debug("STOP: done");

    return 0;
}
