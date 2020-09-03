/**
 * The multi-factor caching algorithm monitor.
 *
 * Dynamically monitors and tweaks `data_admit` & `load_admit` switches
 * on the fly.
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#include <linux/fs.h>
#include <asm/segment.h>
#include <linux/buffer_head.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include "ocf/ocf.h"
#include "cache_engine.h"
#include "engine_debug.h"
#include "../ocf_stats_priv.h"
#include "../ocf_core_priv.h"
#include "mf_monitor.h"


/** Enable kernel verbose logging? */
static const bool MONITOR_VERBOSE_LOG = true;


/** `data_admit` switch, protected by a global rwlock. */
static bool global_data_admit = true;

/** `load_admit` switch, protected by a global rwlock. */
static int global_load_admit = 10000;       // 100%.

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
monitor_set_load_admit(int load_admit)
{
    env_rwlock_write_lock(&load_admit_lock);
    global_load_admit = load_admit;
    env_rwlock_write_unlock(&load_admit_lock);
}

/**
 * For OCF mf policy to query the switch values.
 */
bool
monitor_query_data_admit(void)
{
    bool data_admit;

    env_rwlock_read_lock(&data_admit_lock);
    data_admit = global_data_admit;
    env_rwlock_read_unlock(&data_admit_lock);

    return data_admit;
}

int
monitor_query_load_admit(void)
{
    int load_admit;

    env_rwlock_read_lock(&load_admit_lock);
    load_admit = global_load_admit;
    env_rwlock_read_unlock(&load_admit_lock);

    return load_admit;
}


/**
 * Kernel module file operations implementation.
 *
 * Credit: https://stackoverflow.com/questions/1184274/read-write-files-within-a-linux-kernel-module.
 */
static struct file *
file_open(const char *path, int flags, int rights)
{
    struct file *filp = NULL;
    mm_segment_t oldfs;
    int err = 0;

    oldfs = get_fs();
    set_fs(get_ds());
    filp = filp_open(path, flags, rights);
    set_fs(oldfs);

    if (IS_ERR(filp)) {
        err = PTR_ERR(filp);
        return NULL;
    }

    return filp;
}

static void
file_close(struct file *file)
{
    filp_close(file, NULL);
}

static ssize_t
file_read(struct file *file, unsigned long long offset, unsigned char *data,
          unsigned int size)
{
    mm_segment_t oldfs;
    ssize_t ret;

    oldfs = get_fs();
    set_fs(get_ds());

    ret = vfs_read(file, data, size, &offset);

    set_fs(oldfs);
    return ret;
}


/**
 * Kernel timing utility.
 */
static struct timeval boot_tv;

static int64_t
_get_cur_time_ms(void)
{
    struct timeval cur_tv;
    int64_t cur_time_ms;

    do_gettimeofday(&cur_tv);

    cur_time_ms = (cur_tv.tv_sec - boot_tv.tv_sec) * 1000
                  + (cur_tv.tv_usec - boot_tv.tv_usec) / 1000;

    return cur_time_ms;
}


/** For block device throughput measurement. */
static const char *CACHE_STAT_FILENAME = "/sys/block/sdc/stat";
static const char *CORE_STAT_FILENAME  = "/sys/block/sdb/stat";

static struct file *cache_stat;
static struct file *core_stat;

static char cache_stat_buf[1024];
static char core_stat_buf[1024];


/*========== Multi-factor algorithm logic BEGIN ==========*/

/** Do not attempt tuning when miss ratio is higher than X. */
static const int MISS_RATIO_TUNING_BOUND = 2000;    // 20%.

/** Consider cache is stable if miss ratio within OLD_RATIO +- X. */
static const int WAIT_STABLE_THRESHOLD = 1;         // 0.01%.

/** Sleep X microseconds when detecting cache stability. */
static const int WAIT_STABLE_SLEEP_INTERVAL_US = 1000000;

/** Consider workload change when miss ratio > BASE_RATIO + X. */
static const int WORKLOAD_CHANGE_THRESHOLD = 2000;  // 20%.

/** `load_admit` tuning step size. */
static const int LOAD_ADMIT_TUNING_STEP = 100;      // 1%.

/** Measure throughput for a `load_admit` value for X microseconds. */
static const int MEASURE_THROUGHPUT_INTERVAL_US = 5000;

/** How many chances given to not quit on `load_admit` 100%. */
static const int NOT_QUIT_ON_100_CHANCES = 1;

/**
 * Query the stat component for read (partial + full) miss ratio info.
 */
static inline int
_get_miss_ratio(ocf_core_t core)
{
    struct ocf_counters_req *curr;
    uint64_t misses = 0, total = 0;
    uint32_t i;
    int miss_ratio = -1;

    for (i = 0; i != OCF_IO_CLASS_MAX; ++i) {
        curr = &core->counters->part_counters[i].read_reqs;

        misses += env_atomic64_read(&curr->partial_miss);
        misses += env_atomic64_read(&curr->full_miss);

        total += env_atomic64_read(&curr->total);
    }

    if (total <= 0)
        return -1;

    miss_ratio = (misses * 10000) / total;

    return miss_ratio;
}

/**
 * Set `load_admit` to a value for a while and measure the throughput
 * in KiB/s.
 *
 * Throughput measured from reading /sys/block/<dev>/stat file.
 *     - 2nd counter: read sectors  in 512 bytes
 *     - 6th counter: write sectors in 512 bytes
 *     - 9th counter: device ticks  in milliseconds
 *
 * Returns throughput as an in64_t in KiB/s.
 */
static int64_t
monitor_measure_throughput(int load_admit)
{
    int64_t cache_read_sectors_old = 0, cache_write_sectors_old = 0,
            cache_read_sectors_new = 0, cache_write_sectors_new = 0,
            core_read_sectors_old  = 0, core_write_sectors_old  = 0,
            core_read_sectors_new  = 0, core_write_sectors_new  = 0;
    int64_t old_time_ms = 0, new_time_ms = 0;
    int64_t throughput = 0;

    char *cache_stat_tmp, *core_stat_tmp;
    char *cache_counter, *core_counter;
    int count;

    /** Record old counters. */
    ENV_BUG_ON(file_read(cache_stat, 0, cache_stat_buf, 1024) <= 0);
    ENV_BUG_ON(file_read(core_stat,  0, core_stat_buf,  1024) <= 0);

    old_time_ms = _get_cur_time_ms();

    cache_stat_tmp = cache_stat_buf;
    while (*cache_stat_tmp == ' ')
        cache_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cache_counter = cache_stat_tmp;

        while (*cache_stat_tmp != ' ' && *cache_stat_tmp != '\0')
            cache_stat_tmp++;
        if (*cache_stat_tmp == '\0')
            break;
        *(cache_stat_tmp++) = '\0';
        while (*cache_stat_tmp == ' ')
            cache_stat_tmp++;

        if (count == 2)
            kstrtoll(cache_counter, 10, &cache_read_sectors_old);
        else if (count == 6)
            kstrtoll(cache_counter, 10, &cache_write_sectors_old);
    }

    core_stat_tmp = core_stat_buf;
    while (*core_stat_tmp == ' ')
        core_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        core_counter = core_stat_tmp;

        while (*core_stat_tmp != ' ' && *core_stat_tmp != '\0')
            core_stat_tmp++;
        if (*core_stat_tmp == '\0')
            break;
        *(core_stat_tmp++) = '\0';
        while (*core_stat_tmp == ' ')
            core_stat_tmp++;

        if (count == 2)
            kstrtoll(core_counter, 10, &core_read_sectors_old);
        else if (count == 6)
            kstrtoll(core_counter, 10, &core_write_sectors_old);
    }

    /** Set `load_admit` and sleep for some time. */
    monitor_set_load_admit(load_admit);
    usleep_range(MEASURE_THROUGHPUT_INTERVAL_US,
                 MEASURE_THROUGHPUT_INTERVAL_US + 1);

    /** Get new counters and calculate the throughputs. */
    ENV_BUG_ON(file_read(cache_stat, 0, cache_stat_buf, 1024) <= 0);
    ENV_BUG_ON(file_read(core_stat,  0, core_stat_buf,  1024) <= 0);

    new_time_ms = _get_cur_time_ms();

    cache_stat_tmp = cache_stat_buf;
    while (*cache_stat_tmp == ' ')
        cache_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cache_counter = cache_stat_tmp;

        while (*cache_stat_tmp != ' ' && *cache_stat_tmp != '\0')
            cache_stat_tmp++;
        if (*cache_stat_tmp == '\0')
            break;
        *(cache_stat_tmp++) = '\0';
        while (*cache_stat_tmp == ' ')
            cache_stat_tmp++;

        if (count == 2)
            kstrtoll(cache_counter, 10, &cache_read_sectors_new);
        else if (count == 6)
            kstrtoll(cache_counter, 10, &cache_write_sectors_new);
    }

    core_stat_tmp = core_stat_buf;
    while (*core_stat_tmp == ' ')
        core_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        core_counter = core_stat_tmp;

        while (*core_stat_tmp != ' ' && *core_stat_tmp != '\0')
            core_stat_tmp++;
        if (*core_stat_tmp == '\0')
            break;
        *(core_stat_tmp++) = '\0';
        while (*core_stat_tmp == ' ')
            core_stat_tmp++;

        if (count == 2)
            kstrtoll(core_counter, 10, &core_read_sectors_new);
        else if (count == 6)
            kstrtoll(core_counter, 10, &core_write_sectors_new);
    }

    if (new_time_ms > old_time_ms) {
        throughput += (int64_t) (500
                      * ((cache_read_sectors_new - cache_read_sectors_old)
                      + (cache_write_sectors_new - cache_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        throughput += (int64_t) (500
                      * ((core_read_sectors_new - core_read_sectors_old)
                      + (core_write_sectors_new - core_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
    }

    return throughput;
}

/**
 * Wait until cache hit rate is stable. Returns the final miss ratio.
 */
static int
monitor_wait_stable(ocf_core_t core)
{
    int last_miss_ratio = -1;
    int miss_ratio = _get_miss_ratio(core);

    while (miss_ratio > MISS_RATIO_TUNING_BOUND
           || miss_ratio < last_miss_ratio - WAIT_STABLE_THRESHOLD
           || miss_ratio > last_miss_ratio + WAIT_STABLE_THRESHOLD) {
        if (kthread_should_stop())
            return -1;

        usleep_range(WAIT_STABLE_SLEEP_INTERVAL_US,
                     WAIT_STABLE_SLEEP_INTERVAL_US + 1);
        
        last_miss_ratio = miss_ratio;
        miss_ratio = _get_miss_ratio(core);

        if (MONITOR_VERBOSE_LOG) {
            printk(KERN_ALERT "MONITOR: (wait) miss ratio = %-5d -> %-5d\n",
                   last_miss_ratio, miss_ratio);
        }
    }

    return miss_ratio;
}

/**
 * Repeatedly tune `load_admit` ratio until a workload change is
 * considered happened.
 */
static void
monitor_tune_load_admit(int base_miss_ratio, ocf_core_t core)
{
    int la1, la2, la3;
    int64_t tp1, tp2, tp3;
    int chances = NOT_QUIT_ON_100_CHANCES;
    int64_t iteration = 0;

    while (1) {
        if (kthread_should_stop())
            return;

        iteration++;

        /** Get middle ratio (current `load_admit`) throughput. */
        la2 = monitor_query_load_admit();
        if (MONITOR_VERBOSE_LOG && iteration % 100 == 1) {
            printk(KERN_ALERT "MONITOR: (tune) iter #%lld: load_admit = %-5d\n",
                   iteration, la2);
        }
        tp2 = monitor_measure_throughput(la2);

        /** Get higher ratio throughput. */
        la3 = la2 + LOAD_ADMIT_TUNING_STEP;
        tp3 = la3 > 10000 ? -1 : monitor_measure_throughput(la3);

        /** Get lower ratio throughput. */
        la1 = la2 - LOAD_ADMIT_TUNING_STEP;
        tp1 = la1 < 0     ? -1 : monitor_measure_throughput(la1);

        monitor_set_load_admit(la2);    /** Recover. */

        /** Slope following loop. */
        while (1) {
            /**
             * Workload change check:
             * If detected workload change, quit and re-optimize.
             */
            int miss_ratio = _get_miss_ratio(core);
            if (miss_ratio > MISS_RATIO_TUNING_BOUND
                || miss_ratio > base_miss_ratio + WORKLOAD_CHANGE_THRESHOLD
                || miss_ratio < base_miss_ratio - WORKLOAD_CHANGE_THRESHOLD) {
                if (MONITOR_VERBOSE_LOG) {
                    printk(KERN_ALERT "MONITOR: (tune) miss ratio changed too"
                                      " far, quit\n");
                }
                return;
            }

            if (kthread_should_stop())
                return;

            /**
             * Middle ratio yields best throughput, goto intensity check.
             */
            if (tp2 >= tp1 && tp2 >= tp3) {
                monitor_set_load_admit(la2);
                break;
            }

            /**
             * Higher ratio yields best throughput, then shift to higher
             * `load_admit` value.
             */
            if (tp3 >= tp1 && tp3 >= tp2) {
                if (la3 >= 10000) {
                    monitor_set_load_admit(10000);
                    break;
                } else {
                    la1 = la2; tp1 = tp2;
                    la2 = la3; tp2 = tp3;
                    la3 = la3 + LOAD_ADMIT_TUNING_STEP;
                    tp3 = la3 > 10000 ? -1 : monitor_measure_throughput(la3);
                    continue;
                }
            }

            /**
             * Lower ratio yields best throughput, then shift to lower
             * `load_admit` value.
             */
            if (tp1 >= tp2 && tp1 >= tp3) {
                if (la1 <= 0) {
                    monitor_set_load_admit(0);
                    break;
                } else {
                    la3 = la2; tp3 = tp2;
                    la2 = la1; tp2 = tp1;
                    la1 = la1 - LOAD_ADMIT_TUNING_STEP;
                    tp1 = la1 < 0     ? -1 : monitor_measure_throughput(la1);
                    continue;
                }
            }
        }

        /**
         * Intensity check:
         * If client's request intensity cannot fill cache bandwidth, then fall
         * back to classic caching.
         */
        if (monitor_query_load_admit() == 10000) {
            if (chances > 0) {      /** Give a second chance. */
                chances--;
                continue;
            } else {
                if (MONITOR_VERBOSE_LOG) {
                    printk(KERN_ALERT "MONITOR: (tune) load_admit stays 100%%, "
                                      "quit\n");
                }
                return;
            }
        }
    }
}

/**
 * Monitor thread logic.
 */
static int
monitor_func(void *core_ptr)
{
    ocf_core_t core = core_ptr;

    while (1) {
        int base_miss_ratio;

        if (kthread_should_stop()) {
            env_rwlock_destroy(&data_admit_lock);
            env_rwlock_destroy(&load_admit_lock);

            file_close(cache_stat);
            file_close(core_stat);

            break;
        }

        /** Start a new workload with classic caching. */
        if (MONITOR_VERBOSE_LOG)
            printk(KERN_ALERT "MONITOR: (fall) start classic caching\n");
        monitor_set_data_admit(true);
        monitor_set_load_admit(10000);

        /** Wait until cache is stable. */
        base_miss_ratio = monitor_wait_stable(core);
        if (MONITOR_VERBOSE_LOG)
            printk(KERN_ALERT "MONITOR: (wait) cache is stable\n");

        if (kthread_should_stop()) {
            env_rwlock_destroy(&data_admit_lock);
            env_rwlock_destroy(&load_admit_lock);

            file_close(cache_stat);
            file_close(core_stat);

            break;
        }

        /** Turn off `data_admit` and start `load_admit` tuning. */
        monitor_set_data_admit(false);
        if (MONITOR_VERBOSE_LOG) {
            printk(KERN_ALERT "MONITOR: (tune) turn off data_admit & start "
                              "tuning\n");
        }
        monitor_tune_load_admit(base_miss_ratio, core);
    }

    return 0;
}

/*========== Multi-factor algorithm logic END ==========*/

static struct task_struct *monitor_thread_st = NULL;

/**
 * Setup multi-factor switches and sart the monitor thread.
 */
int
ocf_mngt_mf_monitor_start(ocf_core_t core)
{
    if (monitor_thread_st != NULL)  // Already started.
        return 0;

    global_data_admit = true;
    global_load_admit = 10000;

    env_rwlock_init(&data_admit_lock);
    env_rwlock_init(&load_admit_lock);

    do_gettimeofday(&boot_tv);

    /** Open block device stat files. */
    cache_stat = file_open(CACHE_STAT_FILENAME, O_RDONLY, 0);
    if (cache_stat < 0) {
        OCF_DEBUG_LOG(ocf_core_get_cache(core), "Unable to open cache stat");
        return MF_MONITOR_START_ERR_CACHE_STAT;
    }

    core_stat = file_open(CORE_STAT_FILENAME, O_RDONLY, 0);
    if (core_stat < 0) {
        OCF_DEBUG_LOG(ocf_core_get_cache(core), "Unable to open core stat");
        return MF_MONITOR_START_ERR_CORE_STAT;
    }

    /** Create the monitor thread. */
    monitor_thread_st = kthread_run(monitor_func, (void *) core,
                                    "mf_monitor_thread");
    if (monitor_thread_st == NULL)
        return MF_MONITOR_START_ERR_THREAD_RUN;

    printk(KERN_ALERT "MONITOR: Thread %d started running\n",
           monitor_thread_st->pid);
    return 0;
}

/**
 * For the context to gracefully stop the monitor thread.
 */
void
ocf_mngt_mf_monitor_stop(void)
{
    if (monitor_thread_st != NULL) {    // Only if started.
        kthread_stop(monitor_thread_st);
        printk(KERN_ALERT "MONITOR: Thread %d stop signaled\n",
               monitor_thread_st->pid);
        monitor_thread_st = NULL;
    }
}

/*========== [Orthus FLAG END] ==========*/
