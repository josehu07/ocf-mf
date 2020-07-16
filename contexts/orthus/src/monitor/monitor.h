/**
 * The multi-factor caching algorithm monitor.
 *
 * Dynamically monitors and tweaks `data_admit` & `load_admit` switches
 * on the fly.
 */


#ifndef __MONITOR_H__
#define __MONITOR_H__


#include <ocf/ocf.h>
#include "ocf_env.h"


bool monitor_query_data_admit();
double monitor_query_load_admit();

int monitor_init();


#endif
