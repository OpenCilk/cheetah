#ifndef _CILK_WORKER_H
#define _CILK_WORKER_H

#include "rts-config.h"

struct __cilkrts_stack_frame;
struct local_state;
struct global_state;

enum __cilkrts_worker_state {
    WORKER_IDLE = 10,
    WORKER_SCHED,
    WORKER_STEAL,
    WORKER_RUN
};

struct __cilkrts_worker {
    // T, H, and E pointers in the THE protocol.
    // T and E are frequently accessed and should be in a hot cache line.
    // H could be moved elsewhere because it is only touched when stealing.
    _Atomic(struct __cilkrts_stack_frame **) head;
    _Atomic(struct __cilkrts_stack_frame **) tail;
    _Atomic(struct __cilkrts_stack_frame **) exc;

    // Worker id, a small integer
    worker_id self;

    // 4 byte hole on 64 bit systems

    // A slot that points to the currently executing Cilk frame.
    struct __cilkrts_stack_frame *current_stack_frame;

    // Map from reducer names to reducer values
    cilkred_map *reducer_map;

    // Global state of the runtime system, opaque to the client.
    struct global_state *g;

    // Additional per-worker state hidden from the client.
    struct local_state *l;

    // Cache line boundary on 64 bit systems with 64 byte cache lines

    // Optional state, only maintained if __cilkrts_use_extension == true.
    void *extension;
    void *ext_stack;

    // Limit of the Lazy Task Queue, to detect queue overflow (debug only)
    struct __cilkrts_stack_frame **ltq_limit;

} __attribute__((aligned(1024))); // This alignment reduces false sharing
                                  // induced by hardware prefetchers on some
                                  // systems, such as Intel CPUs.

#endif /* _CILK_WORKER_H */
