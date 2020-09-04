/**
 * Supports multi-factor caching algorithm.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ocf/ocf.h>

#include "simfs/simfs-ctx.h"
#include "cache/cache-obj.h"
#include "cache/cache-vol.h"
#include "core/core-obj.h"
#include "core/core-vol.h"
#include "common.h"
#include "tp-hacking.h"

#include "../../../src/engine/engine_mf.h"


// Exposed for device logging.
double base_time_ms = 0.0;


/**
 * Callback functions to be called when operation completes.
 */
// static void
// write_cmpl_callback(struct ocf_io *io, int error)
// {
//     if (error != 0)
//         DEBUG("WR COMPLETE: error = %d", error);

//     ocf_io_put(io);
// }

static void
read_cmpl_callback(struct ocf_io *io, int error)
{
    if (error != 0)
        DEBUG("RD COMPLETE: error = %d", error);

    ocf_io_put(io);
}


/**
 * For result plotting purpose.
 */
static inline double
_get_miss_ratio(ocf_core_t core)
{
    return ocf_core_get_read_miss_ratio(core);
}

static inline double
_get_load_admit()
{
    return monitor_query_load_admit();
}

static inline double
_get_cache_throughput(double begin_time_ms, double end_time_ms)
{
    return cache_log_query_throughput(begin_time_ms, end_time_ms);
}

static inline double
_get_core_throughput(double begin_time_ms, double end_time_ms)
{
    return core_log_query_throughput(begin_time_ms, end_time_ms);
}


/**
 * Define a workload - choosing a page.
 */

int cache_capacity = 0;
int workload_size = 0;

unsigned int workload_g_seed = 2333;
inline int fastrand() {
  workload_g_seed = (214013*workload_g_seed+2531011);
  return (workload_g_seed>>16)&0x7FFF;
}

static int
which_page_workload_fast()
{
    return fastrand() % workload_size ;
}

//static int
//which_page_workload_small()
//{
//    const int cache_capacity = cache_capacity_bytes / PAGE_SIZE;
//    const int workload_size = (int) (0.1 * cache_capacity);

//    return rand() % workload_size;
//}


// static int
// which_page_workload_1st()
// {
//     const int cache_capacity = cache_capacity_bytes / PAGE_SIZE;
//     const int workload_size = (int) (1.25 * cache_capacity);

//     if (rand() % 100 < 95)
//         return rand() % cache_capacity;
//     else
//         return cache_capacity + (rand() % (workload_size - cache_capacity));
// }

// static int
// which_page_workload_2nd()
// {
//     const int core_capacity = core_capacity_bytes / PAGE_SIZE;
//     const int cache_capacity = cache_capacity_bytes / PAGE_SIZE;
//     const int workload_size = (int) (1.25 * cache_capacity);

//     if (rand() % 100 < 95)
//         return core_capacity - rand() % cache_capacity - 1;
//     else
//         return core_capacity - workload_size - 1
//                + (rand() % (workload_size - cache_capacity));
// }


/**
 * Wrapper functions for I/O submission.
 */

static int
submit_io(ocf_core_t core, simfs_data_t *simfs_data, uint64_t addr,
          uint32_t len, int dir, ocf_end_io_t callback_func)
{
    ocf_cache_t cache = ocf_core_get_cache(core);
    cache_obj_priv_t *cache_obj_priv = ocf_cache_get_priv(cache);
    struct ocf_io* io;

    /** Allocate new I/O in queue. */
    io = ocf_core_new_io(core, cache_obj_priv->io_queue, addr,
                         len, dir, 0, 0);
    if (io == NULL)
        return -ENOMEM;

    /** Assign data to I/O. */
    ocf_io_set_data(io, simfs_data, 0);

    /** Assign completion callback function. */
    ocf_io_set_cmpl(io, NULL, NULL, callback_func);

    /** Submit this I/O. */
    ocf_core_submit_io(io);
    
    return 0;
}

static int
submit_10_ios_in_a_row(ocf_core_t core, simfs_data_t *simfs_data,
                       ocf_end_io_t callback_func)
{
    int i, ret;

    for (i = 0; i < 10; ++i) {
	int dir = OCF_READ;
	//int dir = OCF_WRITE;
        uint32_t size = PAGE_SIZE;
	uint64_t addr = which_page_workload_fast() * PAGE_SIZE;

        ret = submit_io(core, simfs_data, addr, size, dir, callback_func);
	if (ret)
            return ret;
    }

    return 0;
}

ocf_core_t core;
enum bench_cache_mode cache_mode;
int intensity;
int num_threads = 1;



void
wait(int loop_count)
{   // this function needs to be finetuned for the specific microprocessor
    int i, j, k;
    int wait_loop0 = 10;
    int wait_loop1 = 500;
    for(i = 0; i < loop_count; i++)
    {
        for(j = 0; j < wait_loop0; j++)
        {
            for(k = 0; k < wait_loop1; k++)
            {   // waste function, volatile makes sure it is not being optimized out by compiler
                int volatile t = 120 * j * i + k;
                t = t + 5;
            }
        }
    }
}


void *
tp_hack_thread(void *args)
{
    printf("Started a tp hacking workload thread!\n");
    int thread_id = *((int *)args); 
    simfs_data_t *data = simfs_data_alloc(1);
    int ret;

    int local_counter = 0;
    double local_last_timestamp = 0.0;
    double local_cur_timestamp;
    do {
        ret = submit_10_ios_in_a_row(core, data, read_cmpl_callback);
        if (ret !=0) {
	    printf("Submit 10 ios with error\n");
	}

	
	wait(0); //1.1 Million 
	//wait(1);   //0.8 Million
	//wait(3);  // 570 K    mfwa perhaps need a bit smaller wait time
	//wait(10);  // 270 K

	
	local_counter += 10;
        if (local_counter % 1000000 == 0) {
	    local_cur_timestamp = get_cur_time_ms();
            printf("Application thread %d Finished %d ios, in last %f ms, throughput: %f iops\n", thread_id, local_counter, local_cur_timestamp - local_last_timestamp,
			(local_counter) / (local_cur_timestamp - local_last_timestamp) * 1000);
	    local_counter = 0;
	    local_last_timestamp = get_cur_time_ms();
        }

    } while (1);
    
    simfs_data_free(data);
    return NULL;
}

int
perform_workload_tp_hack(ocf_core_t core_input, enum bench_cache_mode cache_mode_input,
                         int intensity_input)
{
    cache_capacity = cache_capacity_bytes / PAGE_SIZE;
    workload_size = (int) (0.5 * cache_capacity);
    //workload_size = (int) (1.25 * cache_capacity);
    
    /** Intensity must be a multiple of 10. */
    if (intensity % 10 != 0) {
        fprintf(stderr, "Intensity must be a multiple of 10.\n");
        return -1;
    }
    
    /** Start multiple workload threads */
    core = core_input;
    cache_mode = cache_mode_input;
    intensity = intensity_input;
    intensity /= 10;
    
    pthread_t * thread_pool = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    for (int i = 0; i < num_threads; i++) {
        int *arg = (int *)malloc(sizeof(*arg));
        *arg = i;
        pthread_create(&thread_pool[i], NULL, tp_hack_thread, (void *)arg);
    }
    
    for (int i = 0; i < num_threads; i++) {
        pthread_join(thread_pool[i], NULL);
    }

    /** Wait multiple workload threads to stop*/

    fflush(stdout);

    /** Force stop. */
    cache_vol_force_stop();
    core_vol_force_stop();


    return 0;
}
