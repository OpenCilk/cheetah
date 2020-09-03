#ifndef _CLOSURE_H
#define _CLOSURE_H

// Includes
#include "debug.h"

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

CHEETAH_INTERNAL const char *Closure_status_to_str(enum ClosureStatus status);

#if CILK_DEBUG
#define Closure_assert_ownership(w, t) Closure_assert_ownership(w, t)
#define Closure_assert_alienation(w, t) Closure_assert_alienation(w, t)
#define CILK_CLOSURE_MAGIC 0xDEADFACE
#define Closure_checkmagic(w, t) Closure_checkmagic(w, t)
#else
#define Closure_assert_ownership(w, t)
#define Closure_assert_alienation(w, t)
#define Closure_checkmagic(w, t)
#endif

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

    // Exceptions (roughly follows the reducer protocol)

    // exception propagated from our right siblings
    struct closure_exception right_exn;
    // exception propagated from our children
    struct closure_exception child_exn;
    // exception thrown from this closure
    struct closure_exception user_exn;

    char *reraise_cfa;
    char *parent_rsp;
    struct cilk_fiber *saved_throwing_fiber;

    // cilkred_map *children_reducer_map;
    // cilkred_map *right_reducer_map;

    /* Stores of non-null values to right_rmap and child_rmap must
       have release ordering to make sure values pointed to by the
       map are visible.  Loads must have acquire ordering. */
    /* Accumulated reducer maps from right siblings */
    _Atomic(cilkred_map *) volatile right_rmap;
    /* Accumulated reducer maps from children */
    _Atomic(cilkred_map *) volatile child_rmap;
    /* Reducer map for this closure when suspended at sync */
    cilkred_map *user_rmap;

} __attribute__((aligned(CILK_CACHE_LINE)));

#if CILK_DEBUG
CHEETAH_INTERNAL void Closure_assert_ownership(__cilkrts_worker *const w,
                                               Closure *t);
CHEETAH_INTERNAL void Closure_assert_alienation(__cilkrts_worker *const w,
                                                Closure *t);
CHEETAH_INTERNAL void Closure_checkmagic(__cilkrts_worker *const w, Closure *t);
#define Closure_assert_ownership(w, t) Closure_assert_ownership(w, t)
#define Closure_assert_alienation(w, t) Closure_assert_alienation(w, t)
#define Closure_checkmagic(w, t) Closure_checkmagic(w, t)
#else
#define Closure_assert_ownership(w, t)
#define Closure_assert_alienation(w, t)
#define Closure_checkmagic(w, t)
#endif // CILK_DEBUG

// TODO: maybe make this look more like the other closure functions.
CHEETAH_INTERNAL void clear_closure_exception(struct closure_exception *exn);

CHEETAH_INTERNAL
void Closure_change_status(__cilkrts_worker *const w, Closure *t,
                           enum ClosureStatus old, enum ClosureStatus status);
CHEETAH_INTERNAL
void Closure_set_status(__cilkrts_worker *const w, Closure *t,
                        enum ClosureStatus status);

CHEETAH_INTERNAL int Closure_trylock(__cilkrts_worker *const w, Closure *t);
CHEETAH_INTERNAL void Closure_lock(__cilkrts_worker *const w, Closure *t);
CHEETAH_INTERNAL void Closure_unlock(__cilkrts_worker *const w, Closure *t);

CHEETAH_INTERNAL int Closure_at_top_of_stack(__cilkrts_worker *const w);
CHEETAH_INTERNAL int Closure_has_children(Closure *cl);

CHEETAH_INTERNAL Closure *Closure_create(__cilkrts_worker *const w);
CHEETAH_INTERNAL Closure *Closure_create_main();

CHEETAH_INTERNAL void Closure_add_child(__cilkrts_worker *const w,
                                        Closure *parent, Closure *child);
CHEETAH_INTERNAL void Closure_remove_child(__cilkrts_worker *const w,
                                           Closure *parent, Closure *child);
CHEETAH_INTERNAL void Closure_add_temp_callee(__cilkrts_worker *const w,
                                              Closure *caller, Closure *callee);
CHEETAH_INTERNAL void Closure_add_callee(__cilkrts_worker *const w,
                                         Closure *caller, Closure *callee);
CHEETAH_INTERNAL void Closure_remove_callee(__cilkrts_worker *const w,
                                            Closure *caller);

CHEETAH_INTERNAL void Closure_suspend_victim(__cilkrts_worker *thief,
                                             __cilkrts_worker *victim,
                                             Closure *cl);
CHEETAH_INTERNAL void Closure_suspend(__cilkrts_worker *const w, Closure *cl);

CHEETAH_INTERNAL void Closure_make_ready(Closure *cl);
CHEETAH_INTERNAL void Closure_destroy(__cilkrts_worker *const w, Closure *t);
CHEETAH_INTERNAL void Closure_destroy_main(Closure *t);
CHEETAH_INTERNAL void Closure_destroy_global(struct global_state *const g,
                                             Closure *t);
#endif
