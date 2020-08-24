/**
 * Supports multi-factor caching algorithm.
 */


#ifndef __TP_HACKING_H__
#define __TP_HACKING_H__


#include <ocf/ocf.h>
#include "ocf_env.h"


int perform_workload_tp_hack(ocf_core_t core, enum bench_cache_mode cache_mode,
                             int intensity);


#endif
