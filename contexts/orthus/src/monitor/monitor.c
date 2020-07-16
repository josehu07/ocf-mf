/**
 * The multi-factor caching algorithm monitor.
 *
 * Dynamically monitors and tweaks `data_admit` & `load_admit` switches
 * on the fly.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <ocf/ocf.h>
#include "ocf_env.h"

#include "monitor.h"


/**
 * Debug printing.
 */
static inline void
debug(const char *fmt, ...)
{
    va_list args;

    printf("[MONITOR] ");

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("\n");
}


/** `data_admit` switch, protected by a global rwlock. */
static bool global_data_admit;

/** `load_admit` switch, protected by a global rwlock. */
static double global_load_admit;

/** Reader-writer lock to protect `data_admit`. */
static env_rwlock data_admit_lock;

/** Reader-writer lock to protect `load_admit`. */
static env_rwlock load_admit_lock;


/**
 * Set switch value with writer lock.
 */
static void
monitor_set_data_admit(bool data_admit)
{
    env_rwlock_write_lock(&data_admit_lock);
    global_data_admit = data_admit;
    env_rwlock_write_unlock(&data_admit_lock);
}

static void
monitor_set_load_admit(double load_admit)
{
    env_rwlock_write_lock(&load_admit_lock);
    global_load_admit = load_admit;
    env_rwlock_write_unlock(&load_admit_lock);
}


/**
 * Monitor thread logic.
 * TODO: implement actual monitor logic into this.
 */
static void *
monitor_func(void *args)
{
    while (1) {
        sleep(1);   /** Awake every 1 sec. */

        monitor_set_data_admit(false);
        monitor_set_load_admit(0.0);

        debug("Pulse: data_admit = %d, load_admit = %.3lf",
              false, 0.0);
    }

    return NULL;
}


/**
 * For OCF mf policy to query the switch values.
 */
bool
monitor_query_data_admit()
{
    bool data_admit;

    env_rwlock_read_lock(&data_admit_lock);
    data_admit = global_data_admit;
    env_rwlock_read_unlock(&data_admit_lock);

    return data_admit;
}

double
monitor_query_load_admit()
{
    double load_admit;

    env_rwlock_read_lock(&load_admit_lock);
    load_admit = global_load_admit;
    env_rwlock_read_unlock(&load_admit_lock);

    return load_admit;
}


/**
 * Setup multi-factor switches and sart the monitor thread.
 */
int
monitor_init()
{
    pthread_t monitor_thread_id;
    pthread_attr_t monitor_thread_attr;
    int ret;

    /** Initialize switch values. */
    global_data_admit = true;
    global_load_admit = 1.0;

    /** Initialize rwlocks. */
    env_rwlock_init(&data_admit_lock);
    env_rwlock_init(&load_admit_lock);

    /** Monitor runs as an infinite loop, so set to detached. */
    ret = pthread_attr_init(&monitor_thread_attr);
    if (ret)
        return ret;

    ret = pthread_attr_setdetachstate(&monitor_thread_attr,
                                      PTHREAD_CREATE_DETACHED);
    if (ret)
        return ret;

    /** Create the monitor thread. */
    ret = pthread_create(&monitor_thread_id, &monitor_thread_attr,
                         monitor_func, NULL);
    if (ret)
        return ret;

    debug("START: done");

    return 0;
}
