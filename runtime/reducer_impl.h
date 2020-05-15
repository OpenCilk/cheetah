#ifndef _REDUCER_IMPL_H
#define _REDUCER_IMPL_H

#include "cilk-internal.h"
#include "cilkred_map.h"

CHEETAH_INTERNAL void reducers_init(global_state *g);
CHEETAH_INTERNAL void reducers_deinit(global_state *g);

// used by the scheduler
CHEETAH_INTERNAL
cilkred_map *merge_two_rmaps(__cilkrts_worker *const ws, cilkred_map *left,
                             cilkred_map *right);

#endif // _REDUCER_IMPL_H
