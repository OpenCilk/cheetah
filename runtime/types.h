#ifndef _CILK_TYPES_H
#define _CILK_TYPES_H

#include <stdint.h>

typedef uint32_t worker_id;
typedef struct __cilkrts_worker __cilkrts_worker;
typedef struct __cilkrts_stack_frame __cilkrts_stack_frame;
typedef struct global_state global_state;

#ifdef REDUCER_MODULE
typedef struct cilkred_map cilkred_map;
#endif

#endif /* _CILK_TYPES_H */
