#ifndef _CILK_WORKER_H
#define _CILK_WORKER_H

#include <atomic>
#include <cstdint>

struct __cilkrts_stack_frame;
struct local_state;
struct global_state;
struct hyper_table;

typedef uint32_t worker_id;
#define WORKER_ID_FMT PRIu32
#define NO_WORKER worker_id(0xffffffffu)

struct __cilkrts_worker {
    // Worker id, a small integer
    worker_id self;

    // 4 byte hole on 64 bit systems

    // Current hyperobject table
    struct hyper_table *hyper_table;

    // Global state of the runtime system, opaque to the client.
    struct global_state *g;

    // Additional per-worker state hidden from the client.
    struct local_state *l;

    // Cache line boundary on 64 bit systems with 64 byte cache lines

    // Optional state, only maintained if __cilkrts_use_extension == true.
    void *extension;
    void *ext_stack;

    // T, H, and E pointers in the THE protocol.
    // T and E are frequently accessed and should be in a hot cache line.
    // H could be moved elsewhere because it is only touched when stealing.
    std::atomic<struct __cilkrts_stack_frame **> tail;
    std::atomic<struct __cilkrts_stack_frame **> exc
      __attribute__((aligned(CILK_CACHE_LINE)));
    std::atomic<struct __cilkrts_stack_frame **> head
      __attribute__((aligned(CILK_CACHE_LINE)));

    // Limit of the Lazy Task Queue, to detect queue overflow (debug only)
    struct __cilkrts_stack_frame **ltq_limit;

} __attribute__((aligned(1024))); // This alignment reduces false sharing
                                  // induced by hardware prefetchers on some
                                  // systems, such as Intel CPUs.

#endif /* _CILK_WORKER_H */
