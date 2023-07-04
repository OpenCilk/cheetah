#ifndef _CLOSURE_TYPE_H
#define _CLOSURE_TYPE_H

#include "cilk-internal.h"
#include "fiber.h"
#include "local-hypertable.h"
#include "mutex.h"

// Forward declaration
typedef struct Closure Closure;

enum ClosureStatus {
    /* Closure.status == 0 is invalid */
    CLOSURE_RUNNING = 42,
    CLOSURE_SUSPENDED,
    CLOSURE_RETURNING,
    CLOSURE_READY,
    CLOSURE_PRE_INVALID, /* before first real use */
    CLOSURE_POST_INVALID /* after destruction */
};

/*
 * the list of children is not distributed among
 * the children themselves, in order to avoid extra protocols
 * and locking.
 */
struct Closure {
    __cilkrts_stack_frame *frame; /* rest of the closure */

    struct cilk_fiber *fiber;
    struct cilk_fiber *fiber_child;

    struct cilk_fiber *ext_fiber;
    struct cilk_fiber *ext_fiber_child;

    worker_id owner_ready_deque; /* debug only */

    enum ClosureStatus status : 8; /* doubles as magic number */
    bool has_cilk_callee;
    bool simulated_stolen;
    bool exception_pending;
    unsigned int join_counter; /* number of outstanding spawned children */
    char *orig_rsp; /* the rsp one should use when sync successfully */

    Closure *callee;

    Closure *call_parent;  /* the "parent" closure that called */
    Closure *spawn_parent; /* the "parent" closure that spawned */

    Closure *left_sib;  // left *spawned* sibling in the closure tree
    Closure *right_sib; // right *spawned* sibling in the closur tree
    // right most *spawned* child in the closure tree
    Closure *right_most_child;

    /*
     * stuff related to ready deque.
     *
     * ANGE: for top of the ReadyDeque, prev_ready = NULL
     *       for bottom of the ReadyDeque, next_ready = NULL
     *       next_ready pointing downward, prev_ready pointing upward
     *
     *       top
     *  next | ^
     *       | | prev
     *       v |
     *       ...
     *  next | ^
     *       | | prev
     *       v |
     *      bottom
     */
    Closure *next_ready;
    Closure *prev_ready;

    hyper_table *right_ht;
    hyper_table *child_ht;
    hyper_table *user_ht;

    _Atomic(worker_id) mutex_owner __attribute__((aligned(CILK_CACHE_LINE)));

} __attribute__((aligned(CILK_CACHE_LINE)));

#endif
