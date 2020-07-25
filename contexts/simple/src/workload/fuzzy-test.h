/**
 * Fuzzy test header.
 *
 * Performs random reads & writes and checks correctness. Assumes all
 * IOs are successful (error = 0) for simplicity. Assumes all volumes
 * and context buffers start with all zeros filled.
 */


#ifndef __FUZZY_TEST_H__
#define __FUZZY_TEST_H__


/**
 * A double-linked list for remembering what data a read is expected
 * to get from device. For verification purpose.
 */
struct expected_page {
    struct list_head head;
    struct ocf_io *io;
    char data[PAGE_SIZE];
};


int perform_workload_fuzzy(ocf_core_t core, int num_ios);


#endif
