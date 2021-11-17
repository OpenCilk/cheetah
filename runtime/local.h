#ifndef _CILK_LOCAL_H
#define _CILK_LOCAL_H

#include <stdbool.h>

#include "internal-malloc-impl.h" /* for cilk_im_desc */

struct hyper_table_cache;

struct local_state {
    struct __cilkrts_stack_frame **shadow_stack;
    struct hyper_table_cache *hyper_table;

    unsigned short state; /* __cilkrts_worker_state */
    bool provably_good_steal;
    unsigned int rand_next;

    jmpbuf rts_ctx;
    struct cilk_fiber_pool fiber_pool;
    struct cilk_im_desc im_desc;
    struct cilk_fiber *fiber_to_free;
    struct cilk_fiber *ext_fiber_to_free;
    struct sched_stats stats;
};

#endif /* _CILK_LOCAL_H */
