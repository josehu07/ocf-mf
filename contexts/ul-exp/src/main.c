/**
 * Simulation context of using the OCF library.
 * 
 * Upwards: simulating a simple paged FS application context `simfs`.
 * 
 * Downwards: using FlashSim to simulate two SSD drives.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <ocf/ocf.h>


/** Options. */
const bool CTX_PRINT_DEBUG_MSG = false;
const bool OCF_LOGGER_INFO_MSG = false;

const bool DEVICE_LOG_ENABLE  = false;
const bool MONITOR_LOG_ENABLE = false;


/** Global parameters. */
const char *cache_sock_name = "cache-sock";
const char *core_sock_name  = "core-sock";

double cpu_freq_mhz = -1.0;

int cache_parallelism = 0;
int core_parallelism  = 0;

uint64_t cache_capacity_bytes = 0;
uint64_t core_capacity_bytes  = 0;

bool flashsim_enable_data;
unsigned long flashsim_page_size = 4096;

FILE *fdevice  = NULL;
FILE *fmonitor = NULL;


#include "simfs/simfs-ctx.h"
#include "cache/cache-vol.h"
#include "cache/cache-obj.h"
#include "core/core-vol.h"
#include "core/core-obj.h"
#include "workload/fuzzy-test.h"
#include "workload/tp-hacking.h"
#include "common.h"


static void
error(const char *msg, int error)
{
    fprintf(stderr, "ERROR: %s, code = %d\n", msg, error);
    exit(1);
}


/**
 * Controlling global time in ms. Assumes Intel CPU.
 */
static uint64_t boot_cpu_cycle;

static inline uint64_t
rdtsc()
{
    uint32_t lo, hi;
    __asm__ __volatile__ (
      "xorl %%eax, %%eax\n"
      "cpuid\n"
      "rdtsc\n"
      : "=a" (lo), "=d" (hi)
      :
      : "%ebx", "%ecx");
    return (uint64_t) hi << 32 | lo;
}

double
get_cur_time_ms()
{
    uint64_t cur_cpu_cycle = rdtsc();
    return (cur_cpu_cycle - boot_cpu_cycle) / (cpu_freq_mhz * 1000);
}


/**
 * Display collected statistics.
 */
static void
_print_statistics(struct ocf_stats_usage *stats_usage,
                 struct ocf_stats_requests *stats_reqs,
                 struct ocf_stats_blocks *stats_blocks,
                 struct ocf_stats_errors *stats_errors)
{
    printf("\nStatistics:\n\n"
           "   usage | cache |  occupied   %8lu pages    %3lu.%02lu %%\n"
           "         |       |      free   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     clean   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     dirty   %8lu pages    %3lu.%02lu %%\n"
           "\n"
           "  blocks | cache |   read ->   %8lu pages    %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     total   %8lu pages    %3lu.%02lu %%\n"
           "         |  core |   read ->   %8lu pages    %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu pages    %3lu.%02lu %%\n"
           "         |       |     total   %8lu pages    %3lu.%02lu %%\n"
           "         |     total           %8lu pages    %3lu.%02lu %%\n"
           "\n"
           "    reqs |  read |     hit $   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | part miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | full miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |     total   %8lu reqs     %3lu.%02lu %%\n"
           "         | write |     hit $   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | part miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       | full miss   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |     total   %8lu reqs     %3lu.%02lu %%\n"
           "         |  pass |   read ->   %8lu reqs     %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu reqs     %3lu.%02lu %%\n"
           "         |     total           %8lu reqs     %3lu.%02lu %%\n"
           "\n"
           "  errors | cache |   read ->   %8lu errors   %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu errors   %3lu.%02lu %%\n"
           "         |  core |   read ->   %8lu errors   %3lu.%02lu %%\n"
           "         |       |  write <-   %8lu errors   %3lu.%02lu %%\n"
           "         |     total           %8lu errors   %3lu.%02lu %%\n",
           stats_usage->occupancy.value,
           stats_usage->occupancy.fraction / 100,
           stats_usage->occupancy.fraction % 100,
           stats_usage->free.value,
           stats_usage->free.fraction / 100,
           stats_usage->free.fraction % 100,
           stats_usage->clean.value,
           stats_usage->clean.fraction / 100,
           stats_usage->clean.fraction % 100,
           stats_usage->dirty.value,
           stats_usage->dirty.fraction / 100,
           stats_usage->dirty.fraction % 100,
           stats_blocks->cache_volume_rd.value,
           stats_blocks->cache_volume_rd.fraction / 100,
           stats_blocks->cache_volume_rd.fraction % 100,
           stats_blocks->cache_volume_wr.value,
           stats_blocks->cache_volume_wr.fraction / 100,
           stats_blocks->cache_volume_wr.fraction % 100,
           stats_blocks->cache_volume_total.value,
           stats_blocks->cache_volume_total.fraction / 100,
           stats_blocks->cache_volume_total.fraction % 100,
           stats_blocks->core_volume_rd.value,
           stats_blocks->core_volume_rd.fraction / 100,
           stats_blocks->core_volume_rd.fraction % 100,
           stats_blocks->core_volume_wr.value,
           stats_blocks->core_volume_wr.fraction / 100,
           stats_blocks->core_volume_wr.fraction % 100,
           stats_blocks->core_volume_total.value,
           stats_blocks->core_volume_total.fraction / 100,
           stats_blocks->core_volume_total.fraction % 100,
           stats_blocks->volume_total.value,
           stats_blocks->volume_total.fraction / 100,
           stats_blocks->volume_total.fraction % 100,
           stats_reqs->rd_hits.value,
           stats_reqs->rd_hits.fraction / 100,
           stats_reqs->rd_hits.fraction % 100,
           stats_reqs->rd_partial_misses.value,
           stats_reqs->rd_partial_misses.fraction / 100,
           stats_reqs->rd_partial_misses.fraction % 100,
           stats_reqs->rd_full_misses.value,
           stats_reqs->rd_full_misses.fraction / 100,
           stats_reqs->rd_full_misses.fraction % 100,
           stats_reqs->rd_total.value,
           stats_reqs->rd_total.fraction / 100,
           stats_reqs->rd_total.fraction % 100,
           stats_reqs->wr_hits.value,
           stats_reqs->wr_hits.fraction / 100,
           stats_reqs->wr_hits.fraction % 100,
           stats_reqs->wr_partial_misses.value,
           stats_reqs->wr_partial_misses.fraction / 100,
           stats_reqs->wr_partial_misses.fraction % 100,
           stats_reqs->wr_full_misses.value,
           stats_reqs->wr_full_misses.fraction / 100,
           stats_reqs->wr_full_misses.fraction % 100,
           stats_reqs->wr_total.value,
           stats_reqs->wr_total.fraction / 100,
           stats_reqs->wr_total.fraction % 100,
           stats_reqs->rd_pt.value,
           stats_reqs->rd_pt.fraction / 100,
           stats_reqs->rd_pt.fraction % 100,
           stats_reqs->wr_pt.value,
           stats_reqs->wr_pt.fraction / 100,
           stats_reqs->wr_pt.fraction % 100,
           stats_reqs->total.value,
           stats_reqs->total.fraction / 100,
           stats_reqs->total.fraction % 100,
           stats_errors->cache_volume_rd.value,
           stats_errors->cache_volume_rd.fraction / 100,
           stats_errors->cache_volume_rd.fraction % 100,
           stats_errors->cache_volume_wr.value,
           stats_errors->cache_volume_wr.fraction / 100,
           stats_errors->cache_volume_wr.fraction % 100,
           stats_errors->core_volume_rd.value,
           stats_errors->core_volume_rd.fraction / 100,
           stats_errors->core_volume_rd.fraction % 100,
           stats_errors->core_volume_wr.value,
           stats_errors->core_volume_wr.fraction / 100,
           stats_errors->core_volume_wr.fraction % 100,
           stats_errors->total.value,
           stats_errors->total.fraction / 100,
           stats_errors->total.fraction % 100);
}


/**
 * Read the `/proc/cpuinfo` virtual file for CPU frequency.
 */
static void
_read_cpu_frequency()
{
    char *line = NULL;
    size_t len = 0;
    ssize_t rlen = 0;

    FILE *fcpu = fopen("/proc/cpuinfo", "r");
    if (fcpu == NULL)
        error("Cannot open `/proc/cpuinfo`", 1);

    while ((rlen = getline(&line, &len, fcpu)) != -1) {
        if (rlen > 7 && (! strncmp(line, "cpu MHz", 7))) {
            sscanf(line, "cpu MHz         : %lf\n", &cpu_freq_mhz);
            break;
        }
    }

    if (cpu_freq_mhz <= 0.0)
        error("Invalid CPU frequency MHz number", 1);
    printf("  CPU frequency: %.3lf MHz\n", cpu_freq_mhz);
}

/**
 * Read cache and core device config files.
 */
static void
_read_cache_device_config()
{
    char *line = NULL;
    size_t len = 0;
    ssize_t rlen = 0;

    FILE *fcache = fopen("cache-ssd.conf", "r");
    if (fcache == NULL)
        error("Cannot open `cache-ssd.conf`", 2);

    uint64_t package_size = 0, die_size = 0, plane_size = 0,
             block_size = 0, page_size = 0;

    while ((rlen = getline(&line, &len, fcache)) != -1) {
        if (rlen > 8 && (! strncmp(line, "SSD_SIZE", 8)))
            sscanf(line, "SSD_SIZE %d\n", &cache_parallelism);
        else if (rlen > 12 && (! strncmp(line, "PACKAGE_SIZE", 12)))
            sscanf(line, "PACKAGE_SIZE %lu\n", &package_size);
        else if (rlen > 8 && (! strncmp(line, "DIE_SIZE", 8)))
            sscanf(line, "DIE_SIZE %lu\n", &die_size);
        else if (rlen > 10 && (! strncmp(line, "PLANE_SIZE", 10)))
            sscanf(line, "PLANE_SIZE %lu\n", &plane_size);
        else if (rlen > 10 && (! strncmp(line, "BLOCK_SIZE", 10)))
            sscanf(line, "BLOCK_SIZE %lu\n", &block_size);
        else if (rlen > 9 && (! strncmp(line, "PAGE_SIZE", 9)))
            sscanf(line, "PAGE_SIZE %lu\n", &page_size);
    }

    if (cache_parallelism <= 0)
        error("Invalid cache SSD number of packages", 2);
    printf("  Cache parallelism: %d\n", cache_parallelism);

    cache_capacity_bytes = cache_parallelism * package_size * die_size
                           * plane_size * block_size * page_size;
    cache_capacity_bytes = (uint64_t) (cache_capacity_bytes
                                       * 0.125);    /** Only use 1/8. */
    if (cache_capacity_bytes <= 0)
        error("Invalid cache SSD capacity", 2);
    printf("  Cache 1/8 capacity: %ld bytes\n", cache_capacity_bytes);
}

static void
_read_core_device_config()
{
    char *line = NULL;
    size_t len = 0;
    ssize_t rlen = 0;

    FILE *fcore = fopen("core-ssd.conf", "r");
    if (fcore == NULL)
        error("Cannot open `core-ssd.conf`", 3);

    uint64_t package_size = 0, die_size = 0, plane_size = 0,
             block_size = 0, page_size = 0;

    while ((rlen = getline(&line, &len, fcore)) != -1) {
        if (rlen > 8 && (! strncmp(line, "SSD_SIZE", 8)))
            sscanf(line, "SSD_SIZE %d\n", &core_parallelism);
        else if (rlen > 12 && (! strncmp(line, "PACKAGE_SIZE", 12)))
            sscanf(line, "PACKAGE_SIZE %lu\n", &package_size);
        else if (rlen > 8 && (! strncmp(line, "DIE_SIZE", 8)))
            sscanf(line, "DIE_SIZE %lu\n", &die_size);
        else if (rlen > 10 && (! strncmp(line, "PLANE_SIZE", 10)))
            sscanf(line, "PLANE_SIZE %lu\n", &plane_size);
        else if (rlen > 10 && (! strncmp(line, "BLOCK_SIZE", 10)))
            sscanf(line, "BLOCK_SIZE %lu\n", &block_size);
        else if (rlen > 9 && (! strncmp(line, "PAGE_SIZE", 9)))
            sscanf(line, "PAGE_SIZE %lu\n", &page_size);
        else if (rlen > 16 && (! strncmp(line, "PAGE_ENABLE_DATA", 16))) {
            int tmp;
            sscanf(line, "PAGE_ENABLE_DATA %d\n", &tmp);
            flashsim_enable_data = (tmp == 1);
        }
    }

    if (page_size <= 0)
        error("Invalid FlashSim page size", 3);
    flashsim_page_size = page_size;

    if (core_parallelism <= 0)
        error("Invalid core SSD number of packages", 3);
    printf("  Core parallelism: %d\n", core_parallelism);

    core_capacity_bytes = core_parallelism * package_size * die_size
                          * plane_size * block_size * page_size;
    core_capacity_bytes = (uint64_t) (core_capacity_bytes
                                      * 0.125);     /** Only use 1/8. */
    if (core_capacity_bytes <= 0)
        error("Invalid cache SSD capacity", 3);
    printf("  Core 1/8 capacity: %ld bytes\n", core_capacity_bytes);

    printf("  FlashSim page size: %ld bytes\n", flashsim_page_size);
    printf("  FlashSim enable data: %s\n", flashsim_enable_data ? "true"
                                                                : "false");
}


/**
 * Main entrance for a round of testing.
 */
static inline void
prompt_usage_exit()
{
    fprintf(stderr, "Usage:\n"
                    "  1) ./bench <pt|wa|wb|mfwa|mfwb> <intensity>\n"
                    "  2) ./bench <pt|wa|wb|mfwa|mfwb> fuzzy\n");
    exit(1);
}

int
main(int argc, char *argv[])
{
    ocf_ctx_t ctx;
    ocf_cache_t cache;
    ocf_core_t core;
    struct ocf_stats_usage stats_usage;
    struct ocf_stats_requests stats_reqs;
    struct ocf_stats_blocks stats_blocks;
    struct ocf_stats_errors stats_errors;

    bool fuzzy_testing = false;
    int intensity = -1, ret;
    enum bench_cache_mode cache_mode;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("\nExperiment setup parameters:\n\n");

    /** FlashSim sockets. */
    cache_sock_name = "cache-sock";
    core_sock_name  = "core-sock";

    /** Get cache mode and intensity for this round of experiment. */
    if (argc != 3)
        prompt_usage_exit();

    if (! strncmp(argv[1], "pt", 2))
        cache_mode = BENCH_CACHE_MODE_PT;
    else if (! strncmp(argv[1], "wa", 2))
        cache_mode = BENCH_CACHE_MODE_WA;
    else if (! strncmp(argv[1], "wb", 2))
        cache_mode = BENCH_CACHE_MODE_WB;
    else if (! strncmp(argv[1], "mfwa", 4))
        cache_mode = BENCH_CACHE_MODE_MFWA;
    else if (! strncmp(argv[1], "mfwb", 4))
        cache_mode = BENCH_CACHE_MODE_MFWB;
    else
        prompt_usage_exit();
    printf("  Using cache mode: %s\n", argv[1]);

    if (! strncmp(argv[2], "fuzzy", 5)) {
        fuzzy_testing = true;
        printf("  Doing fuzzy testing...\n");
    } else {
        fuzzy_testing = false;
        intensity = (int) strtol(argv[2], NULL, 10);
        printf("  Intensity: %d 4KiB-Reqs/s\n", intensity);
    }

    /** Get CPU frequency for timing purpose. */
    _read_cpu_frequency();

    /** Read device config files. */
    _read_cache_device_config();
    _read_core_device_config();

    ENV_BUG_ON(flashsim_page_size != PAGE_SIZE);

    /** Random seed. */
    srand(time(NULL));

    /** Logging locations. */
    fdevice  = fopen("logs/log-device.txt" , "w+");
    fmonitor = fopen("logs/log-monitor.txt", "w+");

    /** Record RDTSC at boot time. */
    boot_cpu_cycle = rdtsc();

    /** 1. Initialize OCF context. */
    ret = simfs_ctx_init(&ctx);
    if (ret)
        error("Unable to initialize app context", ret);

    /** 2. Register two volume types. */
    ret = cache_vol_register(ctx);
    if (ret)
        error("Unable to register cache volume type", ret);

    ret = core_vol_register(ctx);
    if (ret)
        error("Unable to register core volume type", ret);

    /** 3. Setup cache object. */
    ret = cache_obj_setup(ctx, &cache, cache_mode);
    if (ret)
        error("Unable to initialize cache", ret);

    /** 4. Setup core object. */
    ret = core_obj_setup(cache, &core);
    if (ret)
        error("Unable to initialize core", ret);

    /** 5. Init and start the monitor. */
    if (cache_mode == BENCH_CACHE_MODE_MFWA
        || cache_mode == BENCH_CACHE_MODE_MFWB) {
        ret = ocf_mngt_mf_monitor_init(core);
        if (ret)
            error("Unable to start monitor thread", ret);
    }

    /** 6. Perform workload. */
    if (fuzzy_testing)
        ret = perform_workload_fuzzy(core, 30000);
    else
        ret = perform_workload_tp_hack(core, intensity);
    if (ret)
      error("Error when performing workload", ret);

    /** 7. Stop the multi-factor monitor. */
    if (cache_mode == BENCH_CACHE_MODE_MFWA
        || cache_mode == BENCH_CACHE_MODE_MFWB)
        ocf_mngt_mf_monitor_stop();

    /** 8. Collect & show statistics. */
    ocf_stats_collect_core(core, &stats_usage, &stats_reqs,
                           &stats_blocks, &stats_errors);
    _print_statistics(&stats_usage, &stats_reqs,
                      &stats_blocks, &stats_errors);

    /** 9. Stop and detach core from cache. */
    ret = core_obj_stop(core);
    if (ret)
        error("Unable to stop core", ret);

    /** 10. Stop the cache. */
    ret = cache_obj_stop(cache);
    if (ret)
        error("Unable to stop cache", ret);

    /** 11. Unregister volume types. */
    core_vol_unregister(ctx);
    cache_vol_unregister(ctx);

    /** 12. Cleanup this context. */
    simfs_ctx_cleanup(ctx);

    fclose(fdevice);
    fclose(fmonitor);

    return 0;
}
