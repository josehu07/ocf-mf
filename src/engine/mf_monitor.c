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
#include <linux/random.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/time.h>
#include "ocf/ocf.h"
#include "cache_engine.h"
#include "engine_debug.h"
#include "../ocf_stats_priv.h"
#include "../utils/utils_prio_heap.h"
#include "../ocf_core_priv.h"
#include "mf_monitor.h"
#define MAX_LOG_SIZE 48000

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

static env_rwlock latency_vec_lock;

static int log_tail = -1;

static int is_full = 0;

static uint64_t *latency_vec = NULL;


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
    /*struct file *filp = NULL;
    mm_segment_t oldfs;
    int err = 0;

    oldfs = get_fs();
    set_fs(KERNEL_DS);
    filp = filp_open(path, flags, rights);
    set_fs(oldfs);

    if (IS_ERR(filp)) {
        err = PTR_ERR(filp);
        return NULL;
    }*/
    
    struct file *filp = NULL;
    int err = 0;

    filp = filp_open(path, flags, rights);

    if (IS_ERR(filp)) {
        err = PTR_ERR(filp);
        printk(KERN_ALERT "Kan: fail to opened a file");
        return NULL;
    }
            
    
    printk(KERN_ALERT "Kan: opened a file");

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
    /*mm_segment_t oldfs;
    ssize_t ret;

    oldfs = get_fs();
    set_fs(KERNEL_DS);

    ret = vfs_read(file, data, size, &offset);

    set_fs(oldfs);
    */
    
    ssize_t ret;
    ret = kernel_read(file, data, size, &offset);
    return ret;
}


/**
 * Kernel timing utility.
 */
static struct timespec64 boot_tv;

static int64_t
_get_cur_time_ms(void)
{
    struct timespec64 cur_tv;
    int64_t cur_time_ms;

    ktime_get_real_ts64(&cur_tv);

    cur_time_ms = (cur_tv.tv_sec - boot_tv.tv_sec) * 1000
                  + (cur_tv.tv_nsec - boot_tv.tv_nsec) / 1000000;

    return cur_time_ms;
}


/** For block device throughput measurement. */
//static bool measure_latency = false;
static const char *CACHE_STAT_FILENAME = "/sys/block/nvme1n1/stat";
static const char *CORE_STAT_FILENAME  = "/sys/block/nvme0n1/stat";
static const char *CAS_STAT_FILENAME = "/sys/block/cas1-1/stat";

static struct file *cache_stat;
static struct file *core_stat;
static struct file *cas_stat;

static char cache_stat_buf[1024];
static char core_stat_buf[1024];
static char cas_stat_buf[1024];


/*========== Multi-factor algorithm logic BEGIN ==========*/

/** Do not attempt tuning when miss ratio is higher than X. */
static const int MISS_RATIO_TUNING_BOUND = 3000;    // 30%.

/** Consider cache is stable if miss ratio within OLD_RATIO +- X. */
static const int WAIT_STABLE_THRESHOLD = 10;         // 0.10%.

/** Sleep X microseconds when detecting cache stability. */
static const int WAIT_STABLE_SLEEP_INTERVAL_US = 100000; //0.1s

/** Consider workload change when miss ratio > BASE_RATIO + X. */
static const int WORKLOAD_CHANGE_THRESHOLD = 5000;  // 50%.

/** `load_admit` tuning step size. */
static const int LOAD_ADMIT_TUNING_STEP = 100;      // 1%.

/** Measure throughput for a `load_admit` value for X microseconds. */
static const int MEASURE_THROUGHPUT_INTERVAL_US = 50000;

/** How many chances given to not quit on `load_admit` 100%. */
static const int NOT_QUIT_ON_100_CHANCES = 2;

static const int SAMPLE_NUMBER_BOUND = 5;

/**
 * Query the stat component for read (partial + full) miss ratio info.
 */
static inline int
_get_miss_ratio(ocf_core_t core)
{
    struct ocf_counters_req *curr;
    uint64_t misses = 0, total = 0;
    uint32_t i;
    int miss_ratio = 10000;

    for (i = 0; i != OCF_IO_CLASS_MAX; ++i) {
        curr = &core->counters->part_counters[i].read_reqs;

        misses += env_atomic64_read(&curr->partial_miss);
        misses += env_atomic64_read(&curr->full_miss);

        total += env_atomic64_read(&curr->total);


	// set to zero
	env_atomic64_set(&curr->total, 0); 
	env_atomic64_set(&curr->partial_miss, 0); 
	env_atomic64_set(&curr->full_miss, 0); 
    }

    if (total < 0)
        return -1;

    if (total == 0)
        return 10000;
	    
	    
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
    int64_t latency = 0;
    
    int64_t cas_read_sectors_old = 0, cas_write_sectors_old = 0,
            cas_read_sectors_new = 0, cas_write_sectors_new = 0,
            cas_read_requests_old = 0, cas_read_ticks_old = 0,
	    cas_read_requests_new = 0, cas_read_ticks_new = 0;

    
    char *cache_stat_tmp, *core_stat_tmp;
    char *cache_counter, *core_counter;

    char *cas_stat_tmp, *cas_counter;
    int count;
    
    /** Record old counters. */
    ENV_BUG_ON(file_read(cache_stat, 0, cache_stat_buf, 1024) <= 0);
    ENV_BUG_ON(file_read(core_stat,  0, core_stat_buf,  1024) <= 0);
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);

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

    
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);

    old_time_ms = _get_cur_time_ms();

    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_old);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_old);
	else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_old);
	else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_old);
    }
    
    //printk("=== MONITOR: Measured throughput: old cas time: %lld, old cas read sectors: %lld, old cas read ticks: %lld, old cas read requests: %lld, old cas write sectors: %lld, old cache write sectors: %lld", old_time_ms, cas_read_sectors_old, cas_read_ticks_old, cas_read_requests_old, cas_write_sectors_old, cache_write_sectors_old);

    /** Set `load_admit` and sleep for some time. */
    monitor_set_load_admit(load_admit);
    usleep_range(MEASURE_THROUGHPUT_INTERVAL_US,
                 MEASURE_THROUGHPUT_INTERVAL_US + 1);

    /** Get new counters and calculate the throughputs. */
    ///*
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
    //*/
    
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);
    new_time_ms = _get_cur_time_ms();
    
    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_new);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_new);
	else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_new);
	else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_new);
    }
    
    //printk("MONITOR: Measured throughput: new cas time: %lld, new cas read sectors: %lld, new cas read ticks: %lld, new cas read requests: %lld, new cas write sectors: %lld, new cache write sectors: %lld", new_time_ms, cas_read_sectors_new, cas_read_ticks_new, cas_read_requests_new, cas_write_sectors_new, cache_write_sectors_new);

    if (new_time_ms > old_time_ms) {
        /*throughput += (int64_t) (500
                      * ((cache_read_sectors_new - cache_read_sectors_old)
                      + (cache_write_sectors_new - cache_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        throughput += (int64_t) (500
                      * ((core_read_sectors_new - core_read_sectors_old)
                      + (core_write_sectors_new - core_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        */
        throughput += (int64_t) (500
                      * (cas_read_sectors_new - cas_read_sectors_old))
                      / (new_time_ms  - old_time_ms);
        if (cas_read_requests_new != cas_read_requests_old) {
	    latency += (int64_t) (1000 
		       * (cas_read_ticks_new - cas_read_ticks_old)) 
		       / (cas_read_requests_new - cas_read_requests_old);
	}
    }
    printk("MONITOR: Measured throughput: %lld, latency %lld us, for load admit %d", throughput, latency, load_admit);
    return throughput;
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
static void
monitor_measure_throughput_latency(int load_admit, int64_t* tp_ptr, int64_t* la_ptr)
{
    int64_t cache_read_sectors_old = 0, cache_write_sectors_old = 0,
            cache_read_sectors_new = 0, cache_write_sectors_new = 0,
            core_read_sectors_old  = 0, core_write_sectors_old  = 0,
            core_read_sectors_new  = 0, core_write_sectors_new  = 0;
    
    int64_t old_time_ms = 0, new_time_ms = 0;
    int64_t throughput = 0;
    int64_t latency = 0;
    
    int64_t cas_read_sectors_old = 0, cas_write_sectors_old = 0,
            cas_read_sectors_new = 0, cas_write_sectors_new = 0,
            cas_read_requests_old = 0, cas_read_ticks_old = 0,
	    cas_read_requests_new = 0, cas_read_ticks_new = 0;

    
    char *cache_stat_tmp, *core_stat_tmp;
    char *cache_counter, *core_counter;

    char *cas_stat_tmp, *cas_counter;
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

    
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);

    old_time_ms = _get_cur_time_ms();

    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_old);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_old);
	else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_old);
	else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_old);
    }
    
    //printk("=== MONITOR: Measured throughput: old cas time: %lld, old cas read sectors: %lld, old cas read ticks: %lld, old cas read requests: %lld, old cas write sectors: %lld, old cache write sectors: %lld", old_time_ms, cas_read_sectors_old, cas_read_ticks_old, cas_read_requests_old, cas_write_sectors_old, cache_write_sectors_old);

    /** Set `load_admit` and sleep for some time. */
    monitor_set_load_admit(load_admit);
    usleep_range(MEASURE_THROUGHPUT_INTERVAL_US,
                 MEASURE_THROUGHPUT_INTERVAL_US + 1);

    /** Get new counters and calculate the throughputs. */
    ///*
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
    //*/
    
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);
    new_time_ms = _get_cur_time_ms();
    
    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_new);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_new);
	else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_new);
	else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_new);
    }
    
    //printk("MONITOR: Measured throughput: new cas time: %lld, new cas read sectors: %lld, new cas read ticks: %lld, new cas read requests: %lld, new cas write sectors: %lld, new cache write sectors: %lld", new_time_ms, cas_read_sectors_new, cas_read_ticks_new, cas_read_requests_new, cas_write_sectors_new, cache_write_sectors_new);

    if (new_time_ms > old_time_ms) {
        /*throughput += (int64_t) (500
                      * ((cache_read_sectors_new - cache_read_sectors_old)
                      + (cache_write_sectors_new - cache_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        throughput += (int64_t) (500
                      * ((core_read_sectors_new - core_read_sectors_old)
                      + (core_write_sectors_new - core_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        */
        throughput += (int64_t) (500
                      * (cas_read_sectors_new - cas_read_sectors_old))
                      / (new_time_ms  - old_time_ms);
        if (cas_read_requests_new != cas_read_requests_old) {
	    latency += (int64_t) (1000 
		       * (cas_read_ticks_new - cas_read_ticks_old)) 
		       / (cas_read_requests_new - cas_read_requests_old);
	}
    }
    printk("MONITOR: Measured throughput: %lld, latency %lld us, for load admit %d", throughput, latency, load_admit);
    *tp_ptr = throughput;
    *la_ptr = latency;
    return throughput;
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
static void
monitor_measure_throughput_latency(int load_admit, int64_t* tp_ptr, int64_t *cache_tp_ptr, int64_t *core_tp_ptr, int64_t* la_ptr)
{
    int64_t cache_read_sectors_old = 0, cache_write_sectors_old = 0,
            cache_read_sectors_new = 0, cache_write_sectors_new = 0,
            core_read_sectors_old  = 0, core_write_sectors_old  = 0,
            core_read_sectors_new  = 0, core_write_sectors_new  = 0;
    
    int64_t old_time_ms = 0, new_time_ms = 0;
    int64_t throughput = 0, cache_tp = 0, core_tp = 0;
    int64_t latency = 0;
    
    int64_t cas_read_sectors_old = 0, cas_write_sectors_old = 0,
            cas_read_sectors_new = 0, cas_write_sectors_new = 0,
            cas_read_requests_old = 0, cas_read_ticks_old = 0,
	        cas_read_requests_new = 0, cas_read_ticks_new = 0;

    
    char *cache_stat_tmp, *core_stat_tmp;
    char *cache_counter, *core_counter;

    char *cas_stat_tmp, *cas_counter;
    int count;
    
    /** Record old counters. */
    ENV_BUG_ON(file_read(cache_stat, 0, cache_stat_buf, 1024) <= 0);
    ENV_BUG_ON(file_read(core_stat,  0, core_stat_buf,  1024) <= 0);
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);

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

    


    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_old);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_old);
	    else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_old);
	    else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_old);
    }
    
    //printk("=== MONITOR: Measured throughput: old cas time: %lld, old cas read sectors: %lld, old cas read ticks: %lld, old cas read requests: %lld, old cas write sectors: %lld, old cache write sectors: %lld", old_time_ms, cas_read_sectors_old, cas_read_ticks_old, cas_read_requests_old, cas_write_sectors_old, cache_write_sectors_old);

    /** Set `load_admit` and sleep for some time. */
    monitor_set_load_admit(load_admit);
    usleep_range(MEASURE_THROUGHPUT_INTERVAL_US,
                 MEASURE_THROUGHPUT_INTERVAL_US + 1);

    /** Get new counters and calculate the throughputs. */
    ///*
    ENV_BUG_ON(file_read(cache_stat, 0, cache_stat_buf, 1024) <= 0);
    ENV_BUG_ON(file_read(core_stat,  0, core_stat_buf,  1024) <= 0);
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);

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
    //*/
    
    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_new);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_new);
	else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_new);
	else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_new);
    }
    
    //printk("MONITOR: Measured throughput: new cas time: %lld, new cas read sectors: %lld, new cas read ticks: %lld, new cas read requests: %lld, new cas write sectors: %lld, new cache write sectors: %lld", new_time_ms, cas_read_sectors_new, cas_read_ticks_new, cas_read_requests_new, cas_write_sectors_new, cache_write_sectors_new);

    if (new_time_ms > old_time_ms) {
        cache_tp+= (int64_t) (500
                      * ((cache_read_sectors_new - cache_read_sectors_old)
                      + (cache_write_sectors_new - cache_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        core_tp += (int64_t) (500
                      * ((core_read_sectors_new - core_read_sectors_old)
                      + (core_write_sectors_new - core_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        
        throughput += (int64_t) (500
                      * (cas_read_sectors_new - cas_read_sectors_old))
                      / (new_time_ms  - old_time_ms);
        if (cas_read_requests_new != cas_read_requests_old) {
	        latency += (int64_t) (1000 * (cas_read_ticks_new - cas_read_ticks_old)) 
		       / (cas_read_requests_new - cas_read_requests_old);
	    }
    }
    printk("MONITOR: Measured agg throughput: %lld, agg latency %lld us, cache throughput: %lld, core throughput: %lld, for load admit %d", throughput, latency, cache_tp, core_tp, load_admit);
    *tp_ptr = throughput;
    *la_ptr = latency;
    *cache_tp_ptr = cache_tp;
    *core_tp_ptr = core_tp;
    return;
}


// int __gt(void *lhs, void* rhs) {
//     return *((uint64_t *)lhs) < *((uint64_t *)rhs);
// }

static uint64_t _get_tail_latency(int percentage) {
    int size = 0, i = 0, start = 0;
    struct ptr_heap max_heap;
    size_t heap_size = 0;
    uint64_t avg_tail_latency = 0, tmp = 0, avg_latency = 0;
    if (percentage > 100 || percentage < 0)
        return ULONG_MAX;

    env_rwlock_read_lock(&latency_vec_lock);
    start = log_tail;

    if (is_full) {
        env_rwlock_read_unlock(&latency_vec_lock);
        size = MAX_LOG_SIZE;
    } else {
        env_rwlock_read_unlock(&latency_vec_lock);
        size = start + 1;
    }

    heap_size = size * (100 - percentage) / 100;
    // printk(KERN_DEBUG "[_get_tail_latency] heap size:%ld, total logs: %d\n", heap_size, size);
    if (heap_size == 0) return ULONG_MAX;
    if (heap_init(&max_heap, heap_size * sizeof(uint64_t), GFP_KERNEL) < 0) {
        printk(KERN_DEBUG "[_get_tail_latency] Run out of memeory\n");
        return ULONG_MAX;
    }

    for ( ;i < size; i ++) {
        // env_rwlock_read_lock(&latency_vec_lock);
        tmp = latency_vec[start];
        // env_rwlock_read_unlock(&latency_vec_lock);
        avg_latency += tmp;
        heap_insert(&max_heap, tmp);
        if (start <= 0) start = MAX_LOG_SIZE;
        else start = (start - 1) % MAX_LOG_SIZE;
    }
    i = 0;

    for (; i < max_heap.size; i++) {
        avg_tail_latency += max_heap.ptrs[i];
    }
    avg_tail_latency = avg_tail_latency / max_heap.size;
    // printk(KERN_DEBUG "[_get_tail_latency] Average tail latency:%lld, Average latency:%lld, Number of requests:%d\n", avg_tail_latency, avg_latency / size, size);
    heap_free(&max_heap);
    env_rwlock_write_lock(&latency_vec_lock);
    log_tail = -1;
    is_full = 0;
    env_rwlock_write_unlock(&latency_vec_lock);
    return avg_tail_latency;
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
monitor_measure_avg_latency(int load_admit)
{
    int64_t cache_read_sectors_old = 0, cache_write_sectors_old = 0,
            cache_read_sectors_new = 0, cache_write_sectors_new = 0,
            core_read_sectors_old  = 0, core_write_sectors_old  = 0,
            core_read_sectors_new  = 0, core_write_sectors_new  = 0;
    
    int64_t old_time_ms = 0, new_time_ms = 0;
    int64_t throughput = 0;
    int64_t latency = 0;
    
    int64_t cas_read_sectors_old = 0, cas_write_sectors_old = 0,
            cas_read_sectors_new = 0, cas_write_sectors_new = 0,
            cas_read_requests_old = 0, cas_read_ticks_old = 0,
	    cas_read_requests_new = 0, cas_read_ticks_new = 0;

    
    char *cache_stat_tmp, *core_stat_tmp;
    char *cache_counter, *core_counter;

    char *cas_stat_tmp, *cas_counter;
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

    
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);

    old_time_ms = _get_cur_time_ms();

    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_old);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_old);
	else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_old);
	else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_old);
    }
    
    //printk("=== MONITOR: Measured throughput: old cas time: %lld, old cas read sectors: %lld, old cas read ticks: %lld, old cas read requests: %lld, old cas write sectors: %lld, old cache write sectors: %lld", old_time_ms, cas_read_sectors_old, cas_read_ticks_old, cas_read_requests_old, cas_write_sectors_old, cache_write_sectors_old);

    /** Set `load_admit` and sleep for some time. */
    monitor_set_load_admit(load_admit);
    usleep_range(MEASURE_THROUGHPUT_INTERVAL_US,
                 MEASURE_THROUGHPUT_INTERVAL_US + 1);

    /** Get new counters and calculate the throughputs. */
    ///*
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
    //*/
    
    ENV_BUG_ON(file_read(cas_stat,  0, cas_stat_buf,  1024) <= 0);
    new_time_ms = _get_cur_time_ms();
    
    cas_stat_tmp = cas_stat_buf;
    while (*cas_stat_tmp == ' ')
        cas_stat_tmp++;
    for (count = 0; count < 7; ++count) {
        cas_counter = cas_stat_tmp;

        while (*cas_stat_tmp != ' ' && *cas_stat_tmp != '\0')
            cas_stat_tmp++;
        if (*cas_stat_tmp == '\0')
            break;
        *(cas_stat_tmp++) = '\0';
        while (*cas_stat_tmp == ' ')
            cas_stat_tmp++;

        if (count == 2)
            kstrtoll(cas_counter, 10, &cas_read_sectors_new);
        else if (count == 6)
            kstrtoll(cas_counter, 10, &cas_write_sectors_new);
	else if (count == 0)
            kstrtoll(cas_counter, 10, &cas_read_requests_new);
	else if (count == 3)
            kstrtoll(cas_counter, 10, &cas_read_ticks_new);
    }
    
    //printk("MONITOR: Measured throughput: new cas time: %lld, new cas read sectors: %lld, new cas read ticks: %lld, new cas read requests: %lld, new cas write sectors: %lld, new cache write sectors: %lld", new_time_ms, cas_read_sectors_new, cas_read_ticks_new, cas_read_requests_new, cas_write_sectors_new, cache_write_sectors_new);

    if (new_time_ms > old_time_ms) {
        /*throughput += (int64_t) (500
                      * ((cache_read_sectors_new - cache_read_sectors_old)
                      + (cache_write_sectors_new - cache_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        throughput += (int64_t) (500
                      * ((core_read_sectors_new - core_read_sectors_old)
                      + (core_write_sectors_new - core_write_sectors_old)))
                      / (new_time_ms  - old_time_ms);
        */
        throughput += (int64_t) (500
                      * (cas_read_sectors_new - cas_read_sectors_old))
                      / (new_time_ms  - old_time_ms);
        if (cas_read_requests_new != cas_read_requests_old) {
	        latency += (int64_t) (1000 
		        * (cas_read_ticks_new - cas_read_ticks_old)) 
		        / (cas_read_requests_new - cas_read_requests_old);
	    }
    }
    printk("MONITOR: Measured throughput: %lld, latency %lld us, for load admit %d", throughput, latency, load_admit);
    return latency;
}



/**
 * Wait until cache hit rate is stable. Returns the final miss ratio.
 */
static int
monitor_wait_stable(ocf_core_t core)
{
    int last_miss_ratio = 10000;
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
            printk(KERN_DEBUG "MONITOR: (wait) miss ratio = %-5d -> %-5d\n",
                   last_miss_ratio, miss_ratio);
        }
    }

    return miss_ratio;
}

uint64_t monitor_measure_tail_latency(int load_admit) {
    monitor_set_load_admit(load_admit);
    usleep_range(MEASURE_THROUGHPUT_INTERVAL_US,
                 MEASURE_THROUGHPUT_INTERVAL_US + 1);
    return _get_tail_latency(99); 
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

            if (miss_ratio > base_miss_ratio + WORKLOAD_CHANGE_THRESHOLD
                || miss_ratio < base_miss_ratio - WORKLOAD_CHANGE_THRESHOLD) {
                if (MONITOR_VERBOSE_LOG) {
                    printk(KERN_ALERT "MONITOR: (tune) miss ratio changed too"
                                      " far(%d -> %d, %d), quit\n", base_miss_ratio, miss_ratio, WORKLOAD_CHANGE_THRESHOLD);
                }
                return;
            }
	
	    if (MONITOR_VERBOSE_LOG) {
                printk(KERN_ALERT "MONITOR: (tune), la2 = %-5d, tp1 = %lld, tp2 = %lld, tp3 = %lld \n",
                       la2, tp1, tp2, tp3);
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
                    printk(KERN_DEBUG "MONITOR: (tune) load_admit stays 100%%, "
                                      "quit\n");
                }
                return;
            }
        }
    }
}


/**
 * Repeatedly tune `load_admit` ratio until a workload change is
 * considered happened.
 */
static void
monitor_tune_load_admit_tail_latency(int base_miss_ratio, ocf_core_t core)
{
    int la1, la2, la3;
    uint64_t tla1, tla2, tla3;
    int chances = NOT_QUIT_ON_100_CHANCES;
    uint64_t iteration = 0;

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
        tla2 = monitor_measure_tail_latency(la2);

        /** Get higher ratio throughput. */
        la3 = la2 + LOAD_ADMIT_TUNING_STEP;
        tla3 = la3 > 10000 ? ULONG_MAX : monitor_measure_tail_latency(la3);

        /** Get lower ratio throughput. */
        la1 = la2 - LOAD_ADMIT_TUNING_STEP;
        tla1 = la1 < 0     ? ULONG_MAX : monitor_measure_tail_latency(la1);

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
                    printk(KERN_DEBUG "MONITOR: (tune) miss ratio changed too"
                                      " far, quit\n");
                }
                return;
            }

            if (kthread_should_stop())
                return;

            /**
             * Middle ratio yields best throughput, goto intensity check.
             */
            if (tla2 <= tla1 && tla2 <= tla3) {
                monitor_set_load_admit(la2);
                break;
            }

            /**
             * Higher ratio yields best throughput, then shift to higher
             * `load_admit` value.
             */
            if (tla3 <= tla1 && tla3 <= tla2) {
                if (la3 >= 10000) {
                    monitor_set_load_admit(10000);
                    break;
                } else {
                    la1 = la2; tla1 = tla2;
                    la2 = la3; tla2 = tla3;
                    la3 = la3 + LOAD_ADMIT_TUNING_STEP;
                    tla3 = la3 > 10000 ? ULONG_MAX : monitor_measure_tail_latency(la3);
                    continue;
                }
            }

            /**
             * Lower ratio yields best throughput, then shift to lower
             * `load_admit` value.
             */
            if (tla1 <= tla2 && tla1 <= tla3) {
                if (la1 <= 0) {
                    monitor_set_load_admit(0);
                    break;
                } else {
                    la3 = la2; tla3 = tla2;
                    la2 = la1; tla2 = tla1;
                    la1 = la1 - LOAD_ADMIT_TUNING_STEP;
                    tla1 = la1 < 0     ? ULONG_MAX : monitor_measure_tail_latency(la1);
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
                    printk(KERN_DEBUG "MONITOR: (tune) load_admit stays 100%%, "
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
monitor_func(void *args_ptr)
{

    ocf_core_t core = ((struct args*)args_ptr) -> core;
    ocf_tuning_mode_t tuning_mode = ((struct args*)args_ptr) -> tuning_mode;
    kfree(args_ptr);

    while (1) {
        int base_miss_ratio;

        if (kthread_should_stop()) {
            env_rwlock_destroy(&data_admit_lock);
            env_rwlock_destroy(&load_admit_lock);

            file_close(cache_stat);
            file_close(core_stat);
            file_close(cas_stat);

            break;
        }

        /** Start a new workload with classic caching. */
        if (MONITOR_VERBOSE_LOG)
            printk(KERN_ALERT "MONITOR: (fall) start classic caching\n");
	        monitor_set_data_admit(true);
            //monitor_set_data_admit(false);
            monitor_set_load_admit(10000);
        //monitor_set_load_admit(5000);
	//continue;

        /** Wait until cache is stable. */
        base_miss_ratio = monitor_wait_stable(core);
        if (MONITOR_VERBOSE_LOG)
            printk(KERN_ALERT "MONITOR: (wait) cache is stable\n");

        if (kthread_should_stop()) {
            env_rwlock_destroy(&data_admit_lock);
            env_rwlock_destroy(&load_admit_lock);

            file_close(cache_stat);
            file_close(core_stat);
            file_close(cas_stat);

            break;
        }

        /** Turn off `data_admit` and start `load_admit` tuning. */
        monitor_set_data_admit(false);
        if (MONITOR_VERBOSE_LOG) {
            printk(KERN_DEBUG "MONITOR: (tune) turn off data_admit & start "
                              "tuning\n");
        }
        tune_func[(int)tuning_mode](base_miss_ratio, core);
    }

    return 0;
}

/*========== SIB LOGIC START ==========*/
static int64_t bandwidth_mean = 0;
static int64_t bandwidth_var = 0;
static int64_t minimum_la = LONG_MAX;
static int64_t num_elements = 0;


static 
int64_t get_z_score(int64_t bandwidth) {
    if (bandwidth_var == 0) 
        return bandwidth > bandwidth_mean * 85 / 100 ? 0 : -5;
    return (bandwidth - bandwidth_mean) / (int64_t) int_sqrt(bandwidth_var / (num_elements - 1));
}

static
void update_stat(int64_t tp, int64_t la) {
    num_elements ++;
    if (num_elements >= 2) 
        bandwidth_var = (num_elements - 2) * bandwidth_var / (num_elements - 1) + (tp - bandwidth_mean) * (tp - bandwidth_mean) / (num_elements);
     
    bandwidth_mean = bandwidth_mean + (tp - bandwidth_mean) / num_elements;
    // printk(KERN_DEBUG"bandwidth_mean update to %lld, and bandwidth_var update to %lld, tp = %lld", bandwidth_mean, bandwidth_var, tp);
    if (la > 0) minimum_la = la < minimum_la ? la : minimum_la;
}


/**
 * Monitor thread logic.
 */
static int
sib_monitor_without_window_func(void *args_ptr)
{
    ocf_core_t core = ((struct args*)args_ptr) -> core; 
    int64_t last_tp = -1, last_cache_tp = -1, last_core_tp = -1;
    int64_t tp = -1, lat = -1, cache_tp = -1, core_tp = -1;
    int64_t load_admit;
    int8_t tendency_flag = 0;

    printk(KERN_DEBUG"bandwidth_mean = %lld, bandwidth_var = %lld", bandwidth_mean, bandwidth_var);
    while (1) {
        monitor_set_data_admit(true);
        monitor_wait_stable(core);
        // Phase 1: Exploration: starting from load admit equals to 10000 and periodly tries to decrease the load admit
        printk(KERN_DEBUG "Currently at phase 1\n");
        monitor_set_load_admit(10000);
        while (1) {
            load_admit = monitor_query_load_admit();
            if (kthread_should_stop()) {
                env_rwlock_destroy(&data_admit_lock);
                env_rwlock_destroy(&load_admit_lock);

                file_close(cache_stat);
                file_close(core_stat);
                file_close(cas_stat);
                return 0;
            }

            // Increase the bypass level and see what will happen


            if (load_admit - LOAD_ADMIT_TUNING_STEP >= 5000)
                monitor_measure_throughput_latency(load_admit - LOAD_ADMIT_TUNING_STEP, &tp, &cache_tp, &core_tp, &lat);
            else 
                monitor_measure_throughput_latency(5000, &tp, &cache_tp, &core_tp, &lat);

            // Collect the data if the bandwidth increases.
            if (last_tp != -1 && tp > last_tp) {
                printk(KERN_DEBUG "[***]Last window: aggregate tp = %lld, cache tp = %lld, core tp = %lld\n", last_tp, last_cache_tp, last_core_tp); 
                printk(KERN_DEBUG "[******]Current window: aggregate tp = %lld, cache tp = %lld, core tp = %lld\n", tp, cache_tp, core_tp); 
                update_stat(cache_tp, lat);
            } else {
                monitor_set_load_admit(load_admit);
            }
            last_tp = tp;
            last_cache_tp = cache_tp;
            last_core_tp = core_tp;

            if (num_elements > SAMPLE_NUMBER_BOUND) 
                break;
        }

        // Phase 2: using the maximum bandwidth to determine whether we need to increase the bypass level

        printk(KERN_DEBUG "Currently at phase 2\n");
        while (1) {
            if (kthread_should_stop()) {
                env_rwlock_destroy(&data_admit_lock);
                env_rwlock_destroy(&load_admit_lock);

                file_close(cache_stat);
                file_close(core_stat);
                file_close(cas_stat);
                return 0;
            }
            load_admit = monitor_query_load_admit();
            monitor_measure_throughput_latency(load_admit, &tp, &cache_tp, &core_tp, &lat);
            last_tp = tp;
            last_cache_tp = cache_tp;
            last_core_tp = core_tp;
            printk(KERN_DEBUG "[...]latency = %lld, minimum_latency = %lld, bandwidth_mean = %lld, bandwidth_var = %lld, std = %ld, bandwidth = %lld, z-score = %lld\n", lat, minimum_la, bandwidth_mean, bandwidth_var, int_sqrt(bandwidth_var / (num_elements - 1)),cache_tp, get_z_score(cache_tp)); 
            if (get_z_score(cache_tp) > - 4) {
                if (lat > minimum_la) {
                    if (tendency_flag == 3) {
                        tendency_flag = 0;
                        // Increase the bypass level
                        printk(KERN_DEBUG "------> Increasing bypass level from %lld to %lld.\n", load_admit, load_admit - LOAD_ADMIT_TUNING_STEP); 
                        if (load_admit - LOAD_ADMIT_TUNING_STEP >= 5000)
                            monitor_measure_throughput_latency(load_admit - LOAD_ADMIT_TUNING_STEP, &tp, &cache_tp, &core_tp, &lat);
                        else
                            monitor_measure_throughput_latency(5000, &tp, &cache_tp, &core_tp, &lat);

                        if (tp > last_tp)
                            update_stat(cache_tp, lat);
                        
                    } else {
                        tendency_flag ++;
                    }
                } else {
                    tendency_flag = 0;
                }
            } else {
                if (tendency_flag == -3) {
                    // Break out of second phase since the device is under-saturaded. 
                    printk(KERN_DEBUG "------> The device is under-saturated. Go back to phase 1\n"); 
                    break;
                } else {
                    tendency_flag --;
                }
            }
        }

        // Reset the parameter to start from the phase 1
        bandwidth_mean = 0;
        bandwidth_var = 0;
        minimum_la = LONG_MAX;
        num_elements = 0;

        last_tp = -1;
        last_cache_tp = -1;
        last_core_tp = -1;
        tendency_flag = 0;
    }
    return 0;
}


int (*monitor_func_ptr[2])(void *) = 
{
    monitor_func,
    sib_monitor_without_window_func,
};
/*========== SIB LOGIC END ==========*/

/*========== Multi-factor algorithm logic END ==========*/

static struct task_struct *monitor_thread_st = NULL;

/**
 * Setup multi-factor switches and sart the monitor thread.
 */
int
ocf_mngt_mf_monitor_start(ocf_core_t core, ocf_tuning_mode_t tuning_mode)
{
    struct args *arg; 
    if (monitor_thread_st != NULL)  // Already started.
        return 0;
    
    printk(KERN_ALERT "MONITOR(Kan): to start a monitor thread\n");

    global_data_admit = true;
    global_load_admit = 10000;

    env_rwlock_init(&data_admit_lock);
    env_rwlock_init(&load_admit_lock);

    ktime_get_real_ts64(&boot_tv);

    /** Open block device stat files. */
    
    ///*
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
    //*/
    cas_stat = file_open(CAS_STAT_FILENAME, O_RDONLY, 0);
    if (cas_stat < 0) {
        printk(KERN_ALERT "MONITOR: Cannot open cas stat");
        OCF_DEBUG_LOG(ocf_core_get_cache(core), "Unable to open core stat");
        return MF_MONITOR_START_ERR_CORE_STAT;
    }


    cas_stat = file_open(CAS_STAT_FILENAME, O_RDONLY, 0);
    if (cas_stat < 0) {
        OCF_DEBUG_LOG(ocf_core_get_cache(core), "Unable to open cas stat");
        return MF_MONITOR_START_ERR_CORE_STAT;
    }

    arg = (struct args*)kmalloc(sizeof(struct args), GFP_KERNEL);
    arg -> core = core;
    arg -> tuning_mode = tuning_mode;
    /** Create the monitor thread. */
    monitor_thread_st = kthread_run(sib_monitor_without_window_func, (void *) arg,
                                    "mf_monitor_thread");
    if (monitor_thread_st == NULL)
        return MF_MONITOR_START_ERR_THREAD_RUN;

    printk(KERN_DEBUG "MONITOR: Thread %d started running with tuning mode:%s\n",
           monitor_thread_st->pid, tuning_mode_to_str[(int) tuning_mode]);
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
        printk(KERN_DEBUG "MONITOR: Thread %d stop signaled\n",
               monitor_thread_st->pid);
        monitor_thread_st = NULL;
    }
}



/*========== [Orthus FLAG END] ==========*/
