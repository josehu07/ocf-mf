/**
 * Workload file header.
 *
 * OCF users submit io to the core volume, and the engine will
 * interfere and execute desired caching logic in this process.
 */


#ifndef _WORKLOAD_H_
#define _WORKLOAD_H_


void perform_workload(ocf_core_t core);


#endif
