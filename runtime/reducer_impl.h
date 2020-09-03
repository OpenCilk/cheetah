#ifndef _REDUCER_IMPL_H
#define _REDUCER_IMPL_H

#include "cilk-internal.h"
#include "cilkred_map.h"

// On MacOSX, the runtime needs to explicitly load
// __cilkrts_internal_merge_two_rmaps in order to allow Cilksan to dynamically
// interpose it.
#if defined(__APPLE__) && defined(__MACH__)
#define DL_INTERPOSE 1
#else
#define DL_INTERPOSE 0
#endif

CHEETAH_INTERNAL void reducers_init(global_state *);
CHEETAH_INTERNAL void reducers_import(global_state *, __cilkrts_worker *);
CHEETAH_INTERNAL void reducers_deinit(global_state *);

// used by the scheduler
#if DL_INTERPOSE
CHEETAH_INTERNAL cilkred_map *merge_two_rmaps(__cilkrts_worker *const,
                                              cilkred_map *left,
                                              cilkred_map *right);
#else
#define merge_two_rmaps __cilkrts_internal_merge_two_rmaps
#endif // DL_INTERPOSE

// We give this method global visibility, so that tools, notably Cilksan, can
// dynamically interpose the method.
/* CHEETAH_INTERNAL */
cilkred_map *__cilkrts_internal_merge_two_rmaps(__cilkrts_worker *const,
                                                cilkred_map *left,
                                                cilkred_map *right);

#endif // _REDUCER_IMPL_H
