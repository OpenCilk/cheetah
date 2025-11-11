#ifndef _READYDEQUE_H
#define _READYDEQUE_H

#include "closure.h"
#include "rts-config.h"
#include "worker_coord.h"
#include <atomic>

// Forward declaration
struct ReadyDeque;

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
    std::atomic<worker_id> mutex_owner
      __attribute__((aligned(CILK_CACHE_LINE)));

    /*********************************************************
     * Management of ReadyDeques
     *********************************************************/

    void assert_ownership(worker_id self) {
        CILK_ASSERT(mutex_owner.load(std::memory_order_relaxed) == self);
        (void)self;
    }

    static void assert_ownership(ReadyDeque *deques, worker_id self,
                                 worker_id pn) {
        deques[pn].assert_ownership(self);
    }

    void lock(worker_id id) {
        while (true) {
            worker_id current_owner =
                mutex_owner.load(std::memory_order_relaxed);
            if ((current_owner == NO_WORKER) &&
                mutex_owner.compare_exchange_weak
                (current_owner, id, std::memory_order_acq_rel,
                 std::memory_order_relaxed))
                return;
            busy_loop_pause();
        }
    }

    static void lock(ReadyDeque *deques, worker_id self, worker_id pn) {
        deques[pn].lock(self);
    }

    static void lock_self(ReadyDeque *deques, worker_id self) {
        deques[self].lock(self);
    }
    static void unlock_self(ReadyDeque *deques, worker_id self) {
        worker_id id = self;
        deques[id].mutex_owner.store(NO_WORKER, std::memory_order_release);
    }

    bool trylock(worker_id id) {
        worker_id current_owner =
            mutex_owner.load(std::memory_order_relaxed);
        if (current_owner == NO_WORKER)
            return mutex_owner.compare_exchange_weak
                (current_owner, id, std::memory_order_acq_rel,
                 std::memory_order_relaxed);
        return false;
    }

    static bool trylock(ReadyDeque *deques, worker_id self, worker_id pn) {
        return deques[pn].trylock(self);
    }

    void unlock(worker_id self) {
        (void)self; // TODO: Remove unused parameter?
        mutex_owner.store(NO_WORKER, std::memory_order_release);
    }

    static void unlock(ReadyDeque *deques, worker_id self, worker_id pn) {
        deques[pn].unlock(self);
    }

    /*
     * functions that add/remove elements from the top/bottom
     * of deques
     *
     * ANGE: the precondition of these functions is that the worker w -> self
     * must have locked worker pn's deque before entering the function
     */

    Closure *xtract_top(worker_id self) {
        /* ANGE: make sure w has the lock on worker pn's deque */
        assert_ownership(self);

        if (Closure *cl = top) {
            top = cl->next_ready;
            /* ANGE: if there is only one entry in the deque ... */
            if (cl == bottom) {
                CILK_ASSERT_NULL(cl->next_ready);
                bottom = nullptr;
            } else {
                CILK_ASSERT(cl->next_ready);
                (cl->next_ready)->prev_ready = nullptr;
            }
            cl->owner_ready_deque = NO_WORKER;
            return cl;
        }
        CILK_ASSERT_NULL(bottom);
        return nullptr;
    }

    static Closure *xtract_top(ReadyDeque *deques, worker_id self,
                               worker_id pn) {
        return deques[pn].xtract_top(self);
    }

    static Closure *xtract_bottom(ReadyDeque *deques, worker_id self,
                                  worker_id pn) {
        return deques[pn].xtract_bottom(self);
    }

    Closure *xtract_bottom(worker_id self) {
        /* ANGE: make sure w has the lock on worker pn's deque */
        assert_ownership(self);

        if (Closure *cl = bottom) {
            bottom = cl->prev_ready;
            if (cl == top) {
                CILK_ASSERT_NULL(cl->prev_ready);
                top = nullptr;
            } else {
                CILK_ASSERT(cl->prev_ready);
                (cl->prev_ready)->next_ready = nullptr;
            }
            cl->owner_ready_deque = NO_WORKER;
            return cl;
        }
        CILK_ASSERT_NULL(top);
        return nullptr;
    }

    Closure *peek_top(worker_id self) {
        /* ANGE: make sure w has the lock on worker pn's deque */
        assert_ownership(self);

        /* ANGE: return the top but does not unlink it from the rest */
        if (Closure *cl = top) {
            // If w is stealing, then it may peek the top of the deque
            // of the worker who is in the midst of exiting a
            // Cilkified region.  In that case, cl will be the root
            // closure, and cl->owner_ready_deque is not necessarily
            // pn.  The steal will subsequently fail do_dekker_on.
            //CILK_ASSERT(cl->owner_ready_deque == pn ||
            //            (self != pn && cl == w->g->root_closure));
            return cl;
        }
        CILK_ASSERT_NULL(bottom);
        return nullptr;
    }

    static Closure *peek_top(ReadyDeque *deques, worker_id self,
                             worker_id pn) {
        return deques[pn].peek_top(self);
    }

    Closure *peek_bottom(worker_id self) {
        /* ANGE: make sure w has the lock on worker pn's deque */
        assert_ownership(self);

        if (Closure *cl = bottom) {
            return cl;
        }
        CILK_ASSERT_NULL(top);
        return nullptr;
    }

    static Closure *
    peek_bottom(ReadyDeque *deques, worker_id self, worker_id pn) {
        return deques[pn].peek_bottom(self);
    }

    /*
     * ANGE: this allow w -> self to append Closure cl onto worker pn's ready
     *       deque (i.e. make cl the new bottom).
     */
    void add_bottom(Closure *cl, worker_id self, worker_id pn) {
        assert_ownership(self);

        CILK_ASSERT(cl->owner_ready_deque == NO_WORKER);

        cl->prev_ready = bottom;
        cl->next_ready = nullptr;
        bottom = cl;
        cl->owner_ready_deque = pn;
        if (top) {
            CILK_ASSERT(cl->prev_ready);
            (cl->prev_ready)->next_ready = cl;
        } else {
            top = cl;
        }
    }

    static void add_bottom(ReadyDeque *deques, Closure *cl,
                           worker_id self, worker_id pn) {
        return deques[pn].add_bottom(cl, self, pn);
    }

} __attribute__((aligned(CILK_CACHE_LINE)));

#endif
