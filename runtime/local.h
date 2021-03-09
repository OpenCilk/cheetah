#ifndef _CILK_LOCAL_H
#define _CILK_LOCAL_H

#include <stdbool.h>

struct local_state {
    struct __cilkrts_stack_frame **shadow_stack;

    unsigned short state; /* __cilkrts_worker_state */
    bool lock_wait;
    bool provably_good_steal;
    unsigned int rand_next;
    // Local copy of the index-to-worker map.
    worker_id *index_to_worker;

    jmpbuf rts_ctx;
    struct cilk_fiber_pool fiber_pool;
    struct cilk_im_desc im_desc;
    struct cilk_fiber *fiber_to_free;
    struct sched_stats stats;
};

#endif /* _CILK_LOCAL_H */
