/**
 * The multi-factor caching algorithm monitor.
 *
 * Dynamically monitors and tweaks `data_admit` & `load_admit` switches
 * on the fly.
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#ifndef MF_MONITOR_H_
#define MF_MONITOR_H_


bool monitor_query_data_admit(void);
int monitor_query_load_admit(void);


#endif /* MF_MONITOR_H_ */

/*========== [Orthus FLAG END] ==========*/
