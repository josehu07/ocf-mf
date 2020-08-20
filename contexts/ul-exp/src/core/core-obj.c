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

#include "core-vol.h"
#include "common.h"
#include "core-obj.h"


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


/*========== Device log implementation BEGIN ==========*/

extern const int core_parallelism;

/**
 * Device circular log for throughput measurement. It records the
 * latest IOs through this device.
 */
struct core_log_entry {
    double start_time_ms;
    double finish_time_ms;
    uint32_t bytes;
};

#define CORE_LOG_SIZE_PER_PACKAGE (120000)
static struct core_log_entry **core_log;

static int *core_log_head;
static int *core_log_tail;

static env_rwlock *core_log_lock;

// Exposed for device logging.
extern double base_time_ms;

/**
 * Push a new IO entry into the log, possibly erase an oldest one if
 * the log is full. Returns the timestamp for this entry.
 */
void
core_log_push_entry(int pkg, double start_time_ms, double finish_time_ms,
                    uint32_t bytes)
{
    int pos;

    env_rwlock_write_lock(&core_log_lock[pkg]);

    pos = (core_log_tail[pkg] + 1) % CORE_LOG_SIZE_PER_PACKAGE;

    core_log[pkg][pos].start_time_ms = start_time_ms;
    core_log[pkg][pos].finish_time_ms = finish_time_ms;
    core_log[pkg][pos].bytes = bytes;

    core_log_tail[pkg] = pos;
    if (core_log_tail[pkg] == core_log_head[pkg]) {
        core_log_head[pkg] = (core_log_head[pkg] + 1)
                             % CORE_LOG_SIZE_PER_PACKAGE;
    }

    if (core_log_head[pkg] < 0)
        core_log_head[pkg] = 0;

    fprintf(fdevice, "core req: %.3lf - %.3lf of %u\n",
            start_time_ms - base_time_ms,
            finish_time_ms - base_time_ms, bytes);

    env_rwlock_write_unlock(&core_log_lock[pkg]);
}

/**
 * Query the log for throughput (KB/s) of given time interval.
 */
double
core_log_query_throughput(double begin_time_ms, double end_time_ms)
{
    double kilobytes = 0.0;
    int pkg;

    for (pkg = 0; pkg < core_parallelism; ++pkg) {
        env_rwlock_read_lock(&core_log_lock[pkg]);

        if (core_log_head[pkg] >= 0) {
            int i = core_log_tail[pkg] + 1;

            do {
                i = i == 0 ? CORE_LOG_SIZE_PER_PACKAGE - 1 : i - 1;

                if (core_log[pkg][i].finish_time_ms <= begin_time_ms)
                    break;

                if (core_log[pkg][i].finish_time_ms <= end_time_ms)
                    kilobytes += (double) core_log[pkg][i].bytes / 1024.0;
            } while (i != core_log_head[pkg]);
        }

        env_rwlock_read_unlock(&core_log_lock[pkg]);
    }

    return (kilobytes * 1000.0) / (end_time_ms - begin_time_ms);
}

/*========== Device log implementation END ==========*/


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
    int ret, i;

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

    /** Setup device log. */
    core_log = malloc(sizeof(struct core_log_entry *) * core_parallelism);
    core_log_head = malloc(sizeof(int) * core_parallelism);
    core_log_tail = malloc(sizeof(int) * core_parallelism);
    core_log_lock = malloc(sizeof(env_rwlock) * core_parallelism);

    for (i = 0; i < core_parallelism; ++i) {
        core_log[i] = malloc(sizeof(struct core_log_entry)
                              * CORE_LOG_SIZE_PER_PACKAGE);
        core_log_head[i] = -1;
        core_log_tail[i] = -1;
        env_rwlock_init(&core_log_lock[i]);
    }

    DEBUG("SETUP: done");

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
    int ret = 0, i;

    /** Let the callback state point to this functions return value. */
    callback_states.error = &ret;

    ocf_mngt_cache_remove_core(core, remove_core_callback,
                               &callback_states);
    if (ret)
        return ret;

    /** Free device log. */
    for (i = 0; i < core_parallelism; ++i) {
        env_rwlock_destroy(&core_log_lock[i]);
        free(core_log[i]);
    }

    free(core_log);
    free(core_log_head);
    free(core_log_tail);
    free(core_log_lock);

    DEBUG("STOP: done");

    return 0;
}
