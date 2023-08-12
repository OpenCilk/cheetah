#ifndef _CILK_WORKER_H
#define _CILK_WORKER_H

#include "rts-config.h"

struct __cilkrts_stack_frame;
struct local_state;
struct global_state;
struct local_hyper_table;
struct fiber_header;

enum __cilkrts_worker_state {
    WORKER_IDLE = 10,
    WORKER_SCHED,
    WORKER_STEAL,
    WORKER_RUN
};

struct __cilkrts_worker {
    // Worker id, a small integer
    const worker_id self;

    // 4 byte hole on 64 bit systems

    struct local_hyper_table *hyper_table;

    /* struct __cilkrts_stack_frame *current_stack_frame; */
    /* struct fiber_header *fh; */

    // Global state of the runtime system, opaque to the client.
    struct global_state *const g;

    // Additional per-worker state hidden from the client.
    struct local_state *const l;

    // Cache line boundary on 64 bit systems with 64 byte cache lines

    // Optional state, only maintained if __cilkrts_use_extension == true.
    void *extension;
    void *ext_stack;

    // T, H, and E pointers in the THE protocol.
    // T and E are frequently accessed and should be in a hot cache line.
    // H could be moved elsewhere because it is only touched when stealing.
    _Atomic(struct __cilkrts_stack_frame **) tail;
    _Atomic(struct __cilkrts_stack_frame **) exc __attribute__((aligned(64)));
    _Atomic(struct __cilkrts_stack_frame **) head __attribute__((aligned(CILK_CACHE_LINE)));

    // Limit of the Lazy Task Queue, to detect queue overflow (debug only)
    struct __cilkrts_stack_frame **const ltq_limit;

} __attribute__((aligned(1024))); // This alignment reduces false sharing
                                  // induced by hardware prefetchers on some
                                  // systems, such as Intel CPUs.

#endif /* _CILK_WORKER_H */
