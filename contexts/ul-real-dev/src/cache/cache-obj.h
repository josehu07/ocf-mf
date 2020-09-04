/**
 * Cache object header.
 *
 * After volume types have been registered, a logical object should then
 * be created, and a volume of some registered type be attached to the
 * object.
 */


#ifndef __CACHE_OBJ_H__
#define __CACHE_OBJ_H__


#include <ocf/ocf.h>
#include "ocf_env.h"

#include "common.h"


/**
 * Cache object private data.
 */
struct cache_obj_priv {
    ocf_queue_t mngt_queue;     /** Management queue. */
    ocf_queue_t io_queue;       /** I/O queue. */
};

typedef struct cache_obj_priv cache_obj_priv_t;


/** Device log for throughput measurement. */
void cache_log_push_entry(int pkg, double begin_time_ms, double end_time_ms,
                          uint32_t bytes);
double cache_log_query_throughput(double begin_time_ms, double end_time_ms);


int cache_obj_setup(ocf_ctx_t ctx, ocf_cache_t *cache,
                    enum bench_cache_mode cache_mode);
int cache_obj_stop(ocf_cache_t cache);

env_atomic cache_read_counter;
env_atomic cache_write_counter;

/** query as well as reset the counters of cache */
int cache_query_read_throughput();
int cache_query_write_throughput(); 

#endif