/**
 * Workload file header.
 *
 * OCF users submit io to the core volume, and the engine will
 * interfere and execute desired caching logic in this process.
 */


#ifndef __WORKLOAD_H__
#define __WORKLOAD_H__


int perform_workload(ocf_core_t core);


#endif
