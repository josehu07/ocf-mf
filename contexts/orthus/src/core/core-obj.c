/**
 * Core logical object implementation.
 *
 * After volume types have been registered, a logical object should then
 * be created, and a volume of some registered type be attached to the
 * object.
 */


#include <stdio.h>
#include <stdlib.h>
#include <ocf/ocf.h>

#include "../common.h"
#include "core-vol.h"
#include "core-obj.h"


/**
 * Debug printing.
 */
static inline void
debug(const char *fmt, ...)
{
    if (CTX_PRINT_DEBUG_MSG) {
        va_list args;

        printf("[CORE OBJ] ");

        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);

        printf("\n");
    }
}


/**
 * Callback states shared between OCF routines and callbacks during
 * add core. This callback function will be assigned to any OCF
 * routine executed during add core.
 */
struct add_core_callback_states {
    ocf_core_t *core;
    int *error;     /** Pointer to host's return value. */
};

static void
add_core_callback(ocf_cache_t cache, ocf_core_t core,
                  void *callback_states, int error)
{
    struct add_core_callback_states *states = callback_states;

    *states->core = core;
    *states->error = error;
}


/**
 * Callback states shared between OCF routines and callbacks during
 * remove core. This callback function will be assigned to any OCF
 * routine executed during remove core.
 */
struct remove_core_callback_states {
    int *error;     /** Pointer to host's return value. */
};

static void
remove_core_callback(void *callback_states, int error)
{
    struct remove_core_callback_states *states = callback_states;

    *states->error = error;
}


/**
 * Setup the core object and attach core device as a CORE_VOL_TYPE
 * volume. Then, add this core to previously setted up cache.
 * Should be called AFTER `cache_setup`.
 */
int
core_obj_setup(ocf_cache_t cache, ocf_core_t *core)
{
    struct ocf_mngt_core_config core_cfg = { .name = "core" };
    struct add_core_callback_states callback_states;
    int ret;

    /** Let the callback state point to this functions return value. */
    callback_states.core = core;
    callback_states.error = &ret;

    /**
     * Set core configuration to default. Default config details can
     * be found in ocf_mngt.h: ocf_mngt_core_config_set_default().
     *
     * Core's device configs are included in this same config structure,
     * while for cache those are separated.
     */
    ocf_mngt_core_config_set_default(&core_cfg);
    core_cfg.volume_type = CORE_VOL_TYPE;
    ret = ocf_uuid_set_str(&core_cfg.uuid, "core");
    if (ret)
        return ret;

    /** Add core to cache. */
    ocf_mngt_cache_add_core(cache, &core_cfg, add_core_callback,
                            &callback_states);
    if (ret)
        return ret;

    debug("SETUP: done");

    return 0;
}

/**
 * Stop the core.
 * Should be called BEFORE `cache_stop`.
 */
int
core_obj_stop(ocf_core_t core)
{
    struct remove_core_callback_states callback_states;
    int ret = 0;

    /** Let the callback state point to this functions return value. */
    callback_states.error = &ret;

    ocf_mngt_cache_remove_core(core, remove_core_callback,
                               &callback_states);
    if (ret)
        return ret;

    debug("STOP: done");

    return 0;
}
