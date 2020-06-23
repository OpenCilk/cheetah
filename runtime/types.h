#ifndef _CILK_TYPES_H
#define _CILK_TYPES_H

#include <stdint.h>

typedef uint32_t worker_id;
typedef struct __cilkrts_worker __cilkrts_worker;
typedef struct __cilkrts_stack_frame __cilkrts_stack_frame;
typedef struct global_state global_state;
typedef struct cilkred_map cilkred_map;

#define NO_WORKER 0xffffffffu /* type worker_id */

#endif /* _CILK_TYPES_H */
