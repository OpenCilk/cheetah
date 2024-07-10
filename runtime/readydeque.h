#ifndef _READYDEQUE_H
#define _READYDEQUE_H

#include <stdatomic.h>
#include "closure-type.h"
#include "rts-config.h"
#include "worker_coord.h"

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
    Closure *bottom;
    Closure *top __attribute__((aligned(CILK_CACHE_LINE)));
    _Atomic(worker_id) mutex_owner __attribute__((aligned(CILK_CACHE_LINE)));
} __attribute__((aligned(CILK_CACHE_LINE)));

/*********************************************************
 * Management of ReadyDeques
 *********************************************************/

static inline void deque_assert_ownership(ReadyDeque *deques,
                                          worker_id self, worker_id pn) {
    CILK_ASSERT(atomic_load_explicit(&deques[pn].mutex_owner,
                                        memory_order_relaxed) == self);
    (void)deques;
    (void)self;
    (void)pn;
}

static inline void deque_lock_self(ReadyDeque *deques, worker_id self) {
    worker_id id = self;
    while (true) {
        worker_id current_owner =
            atomic_load_explicit(&deques[id].mutex_owner, memory_order_relaxed);
        if ((current_owner == NO_WORKER) &&
            atomic_compare_exchange_weak_explicit(
                &deques[id].mutex_owner, &current_owner, id,
                memory_order_acq_rel, memory_order_relaxed))
            return;
        busy_loop_pause();
    }
}

static inline void deque_unlock_self(ReadyDeque *deques, worker_id self) {
    worker_id id = self;
    atomic_store_explicit(&deques[id].mutex_owner, NO_WORKER,
                          memory_order_release);
}

static inline int deque_trylock(ReadyDeque *deques, worker_id self,
                                worker_id pn) {
    worker_id current_owner =
        atomic_load_explicit(&deques[pn].mutex_owner, memory_order_relaxed);
    if ((current_owner == NO_WORKER) &&
        atomic_compare_exchange_weak_explicit(
            &deques[pn].mutex_owner, &current_owner, self, memory_order_acq_rel,
            memory_order_relaxed))
        return 1;

    return 0;
}

static inline void deque_lock(ReadyDeque *deques, worker_id self,
                              worker_id pn) {
    while (true) {
        worker_id current_owner =
            atomic_load_explicit(&deques[pn].mutex_owner, memory_order_relaxed);
        if ((current_owner == NO_WORKER) &&
            atomic_compare_exchange_weak_explicit(
                &deques[pn].mutex_owner, &current_owner, self,
                memory_order_acq_rel, memory_order_relaxed))
            return;
        busy_loop_pause();
    }
}

static inline void deque_unlock(ReadyDeque *deques, worker_id self,
                                worker_id pn) {
    (void)self; // TODO: Remove unused parameter?
    atomic_store_explicit(&deques[pn].mutex_owner, NO_WORKER,
                          memory_order_release);
}

/*
 * functions that add/remove elements from the top/bottom
 * of deques
 *
 * ANGE: the precondition of these functions is that the worker w -> self
 * must have locked worker pn's deque before entering the function
 */
static inline Closure *deque_xtract_top(ReadyDeque *deques, worker_id self,
                                        worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, self, pn);

    cl = deques[pn].top;
    if (cl) {
        CILK_ASSERT(cl->owner_ready_deque == pn);
        deques[pn].top = cl->next_ready;
        /* ANGE: if there is only one entry in the deque ... */
        if (cl == deques[pn].bottom) {
            CILK_ASSERT(cl->next_ready == (Closure *)NULL);
            deques[pn].bottom = (Closure *)NULL;
        } else {
            CILK_ASSERT(cl->next_ready);
            (cl->next_ready)->prev_ready = (Closure *)NULL;
        }
        WHEN_CILK_DEBUG(cl->owner_ready_deque = NO_WORKER);
    } else {
        CILK_ASSERT(deques[pn].bottom == (Closure *)NULL);
    }

    return cl;
}

static inline Closure *deque_peek_top(ReadyDeque *deques,
                                      __cilkrts_worker *const w, worker_id self,
                                      worker_id pn) {

    (void)w;  // unused if assertions disabled

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, self, pn);

    /* ANGE: return the top but does not unlink it from the rest */
    cl = deques[pn].top;
    if (cl) {
        // If w is stealing, then it may peek the top of the deque of the worker
        // who is in the midst of exiting a Cilkified region.  In that case, cl
        // will be the root closure, and cl->owner_ready_deque is not
        // necessarily pn.  The steal will subsequently fail do_dekker_on.
        CILK_ASSERT(cl->owner_ready_deque == pn ||
                           (self != pn && cl == w->g->root_closure));
    } else {
        CILK_ASSERT(deques[pn].bottom == (Closure *)NULL);
    }

    return cl;
}

static inline Closure *deque_xtract_bottom(ReadyDeque *deques, worker_id self,
                                           worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, self, pn);

    cl = deques[pn].bottom;
    if (cl) {
        CILK_ASSERT(cl->owner_ready_deque == pn);
        deques[pn].bottom = cl->prev_ready;
        if (cl == deques[pn].top) {
            CILK_ASSERT(cl->prev_ready == (Closure *)NULL);
            deques[pn].top = (Closure *)NULL;
        } else {
            CILK_ASSERT(cl->prev_ready);
            (cl->prev_ready)->next_ready = (Closure *)NULL;
        }

        WHEN_CILK_DEBUG(cl->owner_ready_deque = NO_WORKER);
    } else {
        CILK_ASSERT(deques[pn].top == (Closure *)NULL);
    }

    return cl;
}

static inline Closure *
deque_peek_bottom(ReadyDeque *deques, worker_id self, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(deques, self, pn);

    cl = deques[pn].bottom;
    if (cl) {
        CILK_ASSERT(cl->owner_ready_deque == pn);
    } else {
        CILK_ASSERT(deques[pn].top == (Closure *)NULL);
    }

    return cl;
}

/*
 * ANGE: this allow w -> self to append Closure cl onto worker pn's ready
 *       deque (i.e. make cl the new bottom).
 */
static inline void deque_add_bottom(ReadyDeque *deques, Closure *cl,
                                    worker_id self, worker_id pn) {

    deque_assert_ownership(deques, self, pn);
    CILK_ASSERT(cl->owner_ready_deque == NO_WORKER);

    cl->prev_ready = deques[pn].bottom;
    cl->next_ready = (Closure *)NULL;
    deques[pn].bottom = cl;
    WHEN_CILK_DEBUG(cl->owner_ready_deque = pn);

    if (deques[pn].top) {
        CILK_ASSERT(cl->prev_ready);
        (cl->prev_ready)->next_ready = cl;
    } else {
        deques[pn].top = cl;
    }
}

#endif
