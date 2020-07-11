/**
 * Multi-factor cache mode implementation.
 *
 * Writes always follow Write-Around (for now). Reads swtich between
 * cache & core according to `load_admit`. Reads populate lines into
 * cache only if `data_admit` is on (i.e., in workload probing stage).
 */


#ifndef ENGINE_MF_H_
#define ENGINE_MF_H_


int ocf_read_mf(struct ocf_request *req);
int ocf_write_mf(struct ocf_request *req);


#endif /* ENGINE_MF_H_ */
