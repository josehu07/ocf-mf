/**
 * Fuzzy test header.
 *
 * Performs random reads & writes and checks correctness. Assumes all
 * IOs are successful (error = 0) for simplicity. Assumes all volumes
 * and context buffers start with all zeros filled.
 */


#ifndef __FUZZY_TEST_H__
#define __FUZZY_TEST_H__


int perform_workload_fuzzy(ocf_core_t core, int num_ios);


#endif
