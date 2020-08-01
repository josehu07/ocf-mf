/**
 * Common headers across context code.
 */


#ifndef __COMMON_H__
#define __COMMON_H__


#include <string.h>
#include <stdbool.h>


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
 * Controlling the global time in ms.
 */
double get_cur_time_ms();
void time_elapse_ms(double delta_ms);


#endif
