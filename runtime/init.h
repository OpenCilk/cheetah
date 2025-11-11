#ifndef _CILK_INIT_H
#define _CILK_INIT_H

#include "cilk-internal.h"

// For invoke, the global state is implied.
// Exceptions never escape invoke but may escape exit.
CHEETAH_API
void __cilkrts_internal_invoke_cilkified_root(__cilkrts_stack_frame *sf)
  __CILKRTS_NOTHROW;
CHEETAH_API
void __cilkrts_internal_exit_cilkified_root(global_state *g, __cilkrts_stack_frame *sf);

// Used by Cilksan to set nworkers to 1 and force reduction
extern "C"
void __cilkrts_internal_set_nworkers(unsigned int nworkers);

#endif /* _CILK_INIT_H */
