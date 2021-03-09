#ifndef _CLOSURE_TYPE_H
#define _CLOSURE_TYPE_H

#include "cilk-internal.h"
#include "cilkred_map.h"
#include "fiber.h"
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
 * All the data needed to properly handle a thrown exception.
 */
struct closure_exception {
    char *exn;
};

/*
 * the list of children is not distributed among
 * the children themselves, in order to avoid extra protocols
 * and locking.
 */
struct Closure {
    cilk_mutex mutex; /* mutual exclusion lock */

    __cilkrts_stack_frame *frame; /* rest of the closure */

    struct cilk_fiber *fiber;
    struct cilk_fiber *fiber_child;

    worker_id owner_ready_deque; /* debug only */
    worker_id mutex_owner;       /* debug only */

    enum ClosureStatus status : 8; /* doubles as magic number */
    bool has_cilk_callee;
    bool lock_wait;
    bool simulated_stolen;
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

    /* Stores of non-null values to right_rmap and child_rmap must
       have release ordering to make sure values pointed to by the
       map are visible.  Loads must have acquire ordering. */
    /* Accumulated reducer maps from right siblings */
    _Atomic(cilkred_map *) volatile right_rmap;
    /* Accumulated reducer maps from children */
    _Atomic(cilkred_map *) volatile child_rmap;
    /* Reducer map for this closure when suspended at sync */
    cilkred_map *user_rmap;

    // Exceptions (roughly follows the reducer protocol)

    /* Canonical frame address (CFA) of the call-stack frame from which an
       exception was rethrown.  Used to ensure that the rethrown exception
       appears to be rethrown from the correct frame and to avoid repeated calls
       to __cilkrts_leave_frame during stack unwinding. */
    char *reraise_cfa;
    /* Stack pointer for the parent fiber.  used to restore the stack pointer
       properly after entering a landingpad. */
    char *parent_rsp;
    /* Pointer to a fiber whose destruction was delayed for
       exception-handling. */
    struct cilk_fiber *saved_throwing_fiber;

    // exception propagated from our right siblings
    struct closure_exception right_exn;
    // exception propagated from our children
    struct closure_exception child_exn;
    // exception thrown from this closure
    struct closure_exception user_exn;

} __attribute__((aligned(CILK_CACHE_LINE)));

#endif
