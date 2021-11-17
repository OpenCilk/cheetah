#ifndef _READYDEQUE_H
#define _READYDEQUE_H

#include "closure-type.h"
#include "rts-config.h"

// Forward declaration
typedef struct ReadyDeque ReadyDeque;

// Includes
#include "cilk-internal.h"
#include "mutex.h"

#include "debug.h"
#include "global.h"
#include "local.h"

// Actual declaration
struct ReadyDeque {
    cilk_mutex mutex;
    Closure *top, *bottom;
    worker_id mutex_owner;
} __attribute__((aligned(CILK_CACHE_LINE)));

/*********************************************************
 * Management of ReadyDeques
 *********************************************************/

static inline void deque_assert_ownership(ReadyDeque *deques,
                                          __cilkrts_worker *const w,
                                          worker_id pn) {
    CILK_ASSERT(w, deques[pn].mutex_owner == w->self);
}

static inline void deque_lock_self(ReadyDeque *deques,
                                   __cilkrts_worker *const w) {
    worker_id id = w->self;
    cilk_mutex_lock(&deques[id].mutex);
    deques[id].mutex_owner = id;
}

static inline void deque_unlock_self(ReadyDeque *deques,
                                     __cilkrts_worker *const w) {
    worker_id id = w->self;
    deques[id].mutex_owner = NO_WORKER;
    cilk_mutex_unlock(&deques[id].mutex);
}

static inline int deque_trylock(ReadyDeque *deques, __cilkrts_worker *const w,
                                worker_id pn) {
    int ret = cilk_mutex_try(&deques[pn].mutex);
    if (ret) {
        deques[pn].mutex_owner = w->self;
    }
    return ret;
}

static inline void deque_lock(ReadyDeque *deques, __cilkrts_worker *const w,
                              worker_id pn) {
    cilk_mutex_lock(&deques[pn].mutex);
    deques[pn].mutex_owner = w->self;
}

static inline void deque_unlock(ReadyDeque *deques, __cilkrts_worker *const w,
                                worker_id pn) {
    deques[pn].mutex_owner = NO_WORKER;
    cilk_mutex_unlock(&deques[pn].mutex);
}

/*
 * functions that add/remove elements from the top/bottom
 * of deques
 *
 * ANGE: the precondition of these functions is that the worker w -> self
 * must have locked worker pn's deque before entering the function
 */
static inline Closure *
deque_xtract_top(ReadyDeque *deques, __cilkrts_worker *const w, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, w, pn);

    cl = deques[pn].top;
    if (cl) {
        CILK_ASSERT(w, cl->owner_ready_deque == pn);
        deques[pn].top = cl->next_ready;
        /* ANGE: if there is only one entry in the deque ... */
        if (cl == deques[pn].bottom) {
            CILK_ASSERT(w, cl->next_ready == (Closure *)NULL);
            deques[pn].bottom = (Closure *)NULL;
        } else {
            CILK_ASSERT(w, cl->next_ready);
            (cl->next_ready)->prev_ready = (Closure *)NULL;
        }
        WHEN_CILK_DEBUG(cl->owner_ready_deque = NO_WORKER);
    } else {
        CILK_ASSERT(w, deques[pn].bottom == (Closure *)NULL);
    }

    return cl;
}

static inline Closure *deque_peek_top(ReadyDeque *deques,
                                      __cilkrts_worker *const w, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, w, pn);

    /* ANGE: return the top but does not unlink it from the rest */
    cl = deques[pn].top;
    if (cl) {
        // If w is stealing, then it may peek the top of the deque of the worker
        // who is in the midst of exiting a Cilkified region.  In that case, cl
        // will be the root closure, and cl->owner_ready_deque is not
        // necessarily pn.  The steal will subsequently fail do_dekker_on.
        CILK_ASSERT(w, cl->owner_ready_deque == pn ||
                           (w->self != pn && cl == w->g->root_closure));
    } else {
        CILK_ASSERT(w, deques[pn].bottom == (Closure *)NULL);
    }

    return cl;
}

static inline Closure *deque_xtract_bottom(ReadyDeque *deques,
                                           __cilkrts_worker *const w,
                                           worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, w, pn);

    cl = deques[pn].bottom;
    if (cl) {
        CILK_ASSERT(w, cl->owner_ready_deque == pn);
        deques[pn].bottom = cl->prev_ready;
        if (cl == deques[pn].top) {
            CILK_ASSERT(w, cl->prev_ready == (Closure *)NULL);
            deques[pn].top = (Closure *)NULL;
        } else {
            CILK_ASSERT(w, cl->prev_ready);
            (cl->prev_ready)->next_ready = (Closure *)NULL;
        }

        WHEN_CILK_DEBUG(cl->owner_ready_deque = NO_WORKER);
    } else {
        CILK_ASSERT(w, deques[pn].top == (Closure *)NULL);
    }

    return cl;
}

static inline Closure *
deque_peek_bottom(ReadyDeque *deques, __cilkrts_worker *const w, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, w, pn);

    cl = deques[pn].bottom;
    if (cl) {
        CILK_ASSERT(w, cl->owner_ready_deque == pn);
    } else {
        CILK_ASSERT(w, deques[pn].top == (Closure *)NULL);
    }

    return cl;
}

static inline void deque_assert_is_bottom(ReadyDeque *deques,
                                          __cilkrts_worker *const w,
                                          Closure *t) {

    /* ANGE: still need to make sure the worker self has the lock */
    deque_assert_ownership(deques, w, w->self);
    CILK_ASSERT(w, t == deque_peek_bottom(deques, w, w->self));
}

/*
 * ANGE: this allow w -> self to append Closure cl onto worker pn's ready
 *       deque (i.e. make cl the new bottom).
 */
static inline void deque_add_bottom(ReadyDeque *deques,
                                    __cilkrts_worker *const w, Closure *cl,
                                    worker_id pn) {

    deque_assert_ownership(deques, w, pn);
    CILK_ASSERT(w, cl->owner_ready_deque == NO_WORKER);

    cl->prev_ready = deques[pn].bottom;
    cl->next_ready = (Closure *)NULL;
    deques[pn].bottom = cl;
    WHEN_CILK_DEBUG(cl->owner_ready_deque = pn);

    if (deques[pn].top) {
        CILK_ASSERT(w, cl->prev_ready);
        (cl->prev_ready)->next_ready = cl;
    } else {
        deques[pn].top = cl;
    }
}

#endif
