#ifndef _BUSYCLOSURE_H
#define _BUSYCLOSURE_H

#include "closure.h"
#include "debug.h"
#include "global.h"
#include "rts-config.h"
#include "worker_coord.h"
#include <atomic>

struct alignas(CILK_CACHE_LINE) BusyClosure {
    Closure *closure;
    alignas(CILK_CACHE_LINE) std::atomic<worker_id> mutex_owner;

    /*********************************************************
     * Management of BusyClosure
     *********************************************************/

    void assert_ownership([[maybe_unused]] worker_id self) {
        CILK_ASSERT(mutex_owner.load(std::memory_order_relaxed) == self);
    }

    static void assert_ownership(BusyClosure *busy, worker_id self,
                                 worker_id pn) {
        busy[pn].assert_ownership(self);
    }

    void lock(worker_id id) {
        while (true) {
            worker_id current_owner =
                mutex_owner.load(std::memory_order_relaxed);
            if ((current_owner == NO_WORKER) &&
                mutex_owner.compare_exchange_weak(current_owner, id,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
                return;
            busy_loop_pause();
        }
    }

    static void lock(BusyClosure *busy, worker_id self, worker_id pn) {
        busy[pn].lock(self);
    }

    static void lock_self(BusyClosure *busy, worker_id self) {
        busy[self].lock(self);
    }
    static void unlock_self(BusyClosure *busy, worker_id self) {
        worker_id id = self;
        busy[id].mutex_owner.store(NO_WORKER, std::memory_order_release);
    }

    bool trylock(worker_id id) {
        worker_id current_owner = mutex_owner.load(std::memory_order_relaxed);
        if (current_owner == NO_WORKER)
            return mutex_owner.compare_exchange_weak(current_owner, id,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed);
        return false;
    }

    static bool trylock(BusyClosure *busy, worker_id self, worker_id pn) {
        return busy[pn].trylock(self);
    }

    void unlock([[maybe_unused]] worker_id self) {
        mutex_owner.store(NO_WORKER, std::memory_order_release);
    }

    static void unlock(BusyClosure *busy, worker_id self, worker_id pn) {
        busy[pn].unlock(self);
    }

    /*
     * Add/remove the busy closure
     *
     * The precondition of these functions is that the worker self
     * must have locked worker pn's busy closure before entering the function.
     */

    static Closure *xtract(BusyClosure *busy, worker_id self, worker_id pn) {
        return busy[pn].xtract(self);
    }

    Closure *xtract(worker_id self) {
        // make sure w has the lock on worker pn's busy closure
        assert_ownership(self);

        if (Closure *cl = closure) {
            closure = nullptr;
            cl->owner = NO_WORKER;
            return cl;
        }
        return nullptr;
    }

    Closure *peek(worker_id self) {
        // make sure w has the lock on worker pn's busy closure
        assert_ownership(self);
        // returns the closure but does not unlink
        return closure;
    }

    static Closure *peek(BusyClosure *busy, worker_id self, worker_id pn) {
        return busy[pn].peek(self);
    }

    /*
     * This allows self to make Closure cl worker pn's busy closure.
     */
    void set(Closure *cl, worker_id self, worker_id pn) {
        assert_ownership(self);

        CILK_ASSERT(cl->owner == NO_WORKER);
        CILK_ASSERT_NULL(closure);
        closure = cl;
        cl->owner = pn;
    }

    static void set(BusyClosure *busy, Closure *cl, worker_id self,
                    worker_id pn) {
        return busy[pn].set(cl, self, pn);
    }
};

#endif
