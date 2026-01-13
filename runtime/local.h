#ifndef _CILK_LOCAL_H
#define _CILK_LOCAL_H

#include "fiber.h"
#include "internal-malloc-impl.h" /* for cilk_im_desc */
#include "jmpbuf.h"
// #include "local-hypertable.h"
#include "local-hyper-pagetable.h"

enum __cilkrts_worker_state : unsigned char {
    WORKER_IDLE = 10,
    WORKER_SCHED,
    WORKER_STEAL,
    WORKER_RUN
};

struct __attribute__((visibility("hidden"))) local_state {
    __cilkrts_stack_frame **shadow_stack;

    __cilkrts_worker_state state;
    bool provably_good_steal;
    bool exiting;
    bool returning;
    unsigned int rand_next;
    uint32_t wake_val;

    jmpbuf rts_ctx;
    hyper_table *lht;
    hyper_table *rht;
    cilk_fiber_pool fiber_pool;
    cilk_im_desc im_desc;
    sched_stats stats;

    void change_state(__cilkrts_worker_state to);
};

#endif /* _CILK_LOCAL_H */
