#ifndef _READYDEQUE_H
#define _READYDEQUE_H

#include "rts-config.h"

// Forward declaration
typedef struct ReadyDeque ReadyDeque;

// Includes
#include "cilk-internal.h"
#include "closure.h"
#include "debug.h"
#include "mutex.h"

// Actual declaration
struct ReadyDeque {
    cilk_mutex mutex;
    Closure *top, *bottom;
    worker_id mutex_owner;
} __attribute__((aligned(CILK_CACHE_LINE)));

// assert that pn's deque be locked by ourselves
static inline void deque_assert_ownership(__cilkrts_worker *const w,
                                          worker_id pn) {
    CILK_ASSERT(w, w->g->deques[pn].mutex_owner == w->self);
}

static inline void deque_lock_self(__cilkrts_worker *const w) {
    struct local_state *l = w->l;
    worker_id id = w->self;
    global_state *g = w->g;
    l->lock_wait = true;
    cilk_mutex_lock(&g->deques[id].mutex);
    l->lock_wait = false;
    g->deques[id].mutex_owner = id;
}

static inline void deque_unlock_self(__cilkrts_worker *const w) {
    worker_id id = w->self;
    global_state *g = w->g;
    g->deques[id].mutex_owner = NOBODY;
    cilk_mutex_unlock(&g->deques[id].mutex);
}

static inline int deque_trylock(__cilkrts_worker *const w, worker_id pn) {
    global_state *g = w->g;
    int ret = cilk_mutex_try(&g->deques[pn].mutex);
    if (ret) {
        g->deques[pn].mutex_owner = w->self;
    }
    return ret;
}

static inline void deque_lock(__cilkrts_worker *const w, worker_id pn) {
    global_state *g = w->g;
    struct local_state *l = w->l;
    l->lock_wait = true;
    cilk_mutex_lock(&g->deques[pn].mutex);
    l->lock_wait = false;
    g->deques[pn].mutex_owner = w->self;
}

static inline void deque_unlock(__cilkrts_worker *const w, worker_id pn) {
    global_state *g = w->g;
    g->deques[pn].mutex_owner = NOBODY;
    cilk_mutex_unlock(&w->g->deques[pn].mutex);
}

/*
 * functions that add/remove elements from the top/bottom of deques
 *
 * ANGE: the precondition of these functions is that the worker w -> self
 * must have locked worker pn's deque before entering the function
 */
CHEETAH_INTERNAL
Closure *deque_xtract_top(__cilkrts_worker *const w, worker_id pn);

CHEETAH_INTERNAL
Closure *deque_peek_top(__cilkrts_worker *const w, worker_id pn);

CHEETAH_INTERNAL
Closure *deque_xtract_bottom(__cilkrts_worker *const w, worker_id pn);

CHEETAH_INTERNAL
Closure *deque_peek_bottom(__cilkrts_worker *const w, worker_id pn);

/*
 * ANGE: this allow w -> self to append Closure cl onto worker pn's ready
 *       deque (i.e. make cl the new bottom).
 */
CHEETAH_INTERNAL void deque_add_bottom(__cilkrts_worker *const w, Closure *cl,
                                       worker_id pn);

CHEETAH_INTERNAL void deque_assert_is_bottom(__cilkrts_worker *const w,
                                             Closure *t);

/* ANGE: remove closure for frame f from bottom of pn's deque and _really_
 *       free them (i.e. not internal-free).  As far as I can tell.
 *       This is called only in invoke_main_slow in invoke-main.c.
 */
CHEETAH_INTERNAL
void Cilk_remove_and_free_closure_and_frame(__cilkrts_worker *const w,
                                            __cilkrts_stack_frame *f,
                                            worker_id pn);
#endif
