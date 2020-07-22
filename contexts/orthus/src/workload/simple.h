/**
 * Workload file header.
 *
 * OCF users submit io to the core volume, and the engine will
 * interfere and execute desired caching logic in this process.
 */


#ifndef __WORKLOAD_SIMPLE_H__
#define __WORKLOAD_SIMPLE_H__


int perform_workload_simple(ocf_core_t core);


#endif
