/**
 * Multi-factor cache mode implementation.
 *
 * Writes always follow Write-Around (for now). Reads swtich between
 * cache & core according to `load_admit`. Reads populate lines into
 * cache only if `data_admit` is on (i.e., in workload probing stage).
 *
 * Monitor logic is implemented in `mf_monitor.c`. Must ensure that
 * the monitor has been initialized and started through
 * `ocf_mngt_mf_monitor_init()`.
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#ifndef ENGINE_MF_H_
#define ENGINE_MF_H_


#include <stdbool.h>
#include "../ocf_request.h"


bool monitor_query_data_admit();
double monitor_query_load_admit();


int ocf_read_mf(struct ocf_request *req);
int ocf_write_mf(struct ocf_request *req);

/*========== [Orthus FLAG END] ==========*/


#endif /* ENGINE_MF_H_ */
