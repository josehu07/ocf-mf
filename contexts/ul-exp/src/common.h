/**
 * Common headers across context code.
 */


#ifndef __COMMON_H__
#define __COMMON_H__


#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>


extern FILE *fdevice;
extern FILE *fmonitor;


/**
 * Debug printing.
 */
extern bool OCF_LOGGER_INFO_MSG;
extern bool CTX_PRINT_DEBUG_MSG;

#define __FILEBASE__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 \
                                             : __FILE__)

#define DEBUG(fmt, args...) if (CTX_PRINT_DEBUG_MSG)        \
                                printf("[%s:%d] " fmt "\n", \
                                       __FILEBASE__, __LINE__, ##args)


/**
 * Get the global time in ms unit.
 */
double get_cur_time_ms();


#endif
