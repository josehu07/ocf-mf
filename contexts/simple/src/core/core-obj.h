/**
 * Cache object header.
 *
 * After volume types have been registered, a logical object should then
 * be created, and a volume of some registered type be attached to the
 * object.
 */


#ifndef __CORE_OBJ_H__
#define __CORE_OBJ_H__


#include <ocf/ocf.h>
#include "ocf_env.h"


int core_obj_setup(ocf_cache_t cache, ocf_core_t *core);
int core_obj_stop(ocf_core_t core);


#endif
