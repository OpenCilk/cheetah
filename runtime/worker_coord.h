#ifndef _WORKER_COORD_H
#define _WORKER_COORD_H

// Routines for coordinating workers, specifically, putting workers to sleep and
// waking workers when execution enters and leaves cilkified regions.

#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <err.h>

#include "global.h"
#include "mutex.h"

//=========================================================
// Common internal interface for managing execution of workers.
//=========================================================

__attribute__((always_inline)) static inline void busy_loop_pause() {
#ifdef __SSE__
    __builtin_ia32_pause();
#endif
#ifdef __aarch64__
    __builtin_arm_yield();
#endif
}

__attribute__((always_inline)) static inline void busy_pause(void) {
    for (int i = 0; i < BUSY_PAUSE; ++i)
        busy_loop_pause();
}

// Routines to update global flags to prevent workers from re-entering the
// work-stealing loop.  Note that we don't wait for the workers to exit the
// work-stealing loop, since its more efficient to allow that to happen
// eventually.

// Routines to control the cilkified state.

static inline void set_cilkified(global_state *g) {
    // Set g->cilkified = 1, indicating that the execution is now cilkified.
    atomic_store_explicit(&g->cilkified, 1, memory_order_release);
}

// Mark the computation as no longer cilkified and signal the thread that
// originally cilkified the execution.
static inline void signal_uncilkified(global_state *g) {
#if USE_FUTEX
    cond_post(&g->cilkified, 0);
#else
    cond_post(&g->cilkified, 0, &g->cilkified_cond_var, &g->cilkified_lock);
#endif
}

// Wait on g->cilkified to be set to 0, indicating the end of the Cilkified
// region.
static inline void wait_while_cilkified(global_state *g) {
    unsigned int fail = 0;
    do {
        if (!atomic_load_explicit(&g->cilkified, memory_order_acquire)) {
            return;
        }
        busy_pause();
    } while (fail++ < BUSY_LOOP_SPIN);
#if USE_FUTEX
    cond_wait(&g->cilkified, 1); // Wait as long as cilkified == 1.
#else
    cond_wait(&g->cilkified, 1, &g->cilkified_cond_var, &g->cilkified_lock);
#endif
}

//=========================================================
// Operations to disengage and reengage workers within the work-stealing loop.
//=========================================================

// Reset the shared variable for disengaging thief threads.
static inline void reset_disengaged_var(global_state *g) {
#if !USE_FUTEX
    pthread_mutex_lock(&g->disengaged_lock);
#endif
    atomic_store_explicit(&g->disengaged_thieves, 0, memory_order_release);
#if !USE_FUTEX
    pthread_mutex_unlock(&g->disengaged_lock);
#endif
}

// Request to reengage `count` thief threads.
static inline void request_more_thieves(global_state *g, uint32_t count) {
    CILK_ASSERT(count > 0);

    // Don't allow this routine increment the futex beyond half the number of
    // workers on the system.  This bounds how many successful steals can
    // possibly keep thieves engaged unnecessarily in the future, when there may
    // not be as much parallelism.
    int32_t max_requests = (int32_t)(g->nworkers / 2);
#if USE_FUTEX
    // This step synchronizes with concurrent calls to request_more_thieves and
    // concurrent calls to try_to_disengage_thief.
    while (true) {
        futex_val_t disengaged_thieves = atomic_load_explicit(
            &g->disengaged_thieves, memory_order_acquire);

        int32_t max_to_wake = max_requests - disengaged_thieves;
        if (max_to_wake <= 0)
            return;
        uint64_t to_wake = max_to_wake < (int32_t)count ? max_to_wake : count;

        if (atomic_compare_exchange_strong_explicit(
                &g->disengaged_thieves, &disengaged_thieves,
                disengaged_thieves + to_wake, memory_order_release,
                memory_order_relaxed)) {
            // We successfully updated the futex.  Wake the thief threads
            // waiting on this futex.
            cond_wake_some(&g->disengaged_thieves, to_wake);
            return;
        }
    }
#else
    pthread_mutex_lock(&g->disengaged_lock);
    uint32_t disengaged_thieves = atomic_load_explicit(
        &g->disengaged_thieves, memory_order_acquire);

    int32_t max_to_wake = max_requests - disengaged_thieves;
    if (max_to_wake <= 0) {
        pthread_mutex_unlock(&g->disengaged_lock);
        return;
    }
    uint32_t to_wake = max_to_wake < (int32_t)count ? max_to_wake : count;
    cond_wake_some_locked(&g->disengaged_thieves,
                          disengaged_thieves + to_wake,
                          &g->cilkified_cond_var, to_wake);
    pthread_mutex_unlock(&g->disengaged_lock);
#endif
}

#if USE_FUTEX
static inline uint32_t thief_disengage_futex(futex_t *cilkified,
                                             futex_t *futexp) {
    // This step synchronizes with calls to request_more_thieves.
    while (true) {
        // Decrement the futex when woken up.  The loop and compare-exchange are
        // designed to handle cases where multiple threads waiting on the futex
        // were woken up and where there may be spurious wakeups.
        futex_val_t val;
        while ((val = atomic_load_explicit(futexp, memory_order_relaxed)) > 0) {
            if (atomic_compare_exchange_weak_explicit(futexp, &val, val - 1,
                                                      memory_order_release,
                                                      memory_order_relaxed)) {
                return val;
            }
            busy_loop_pause();
        }

        // The futex was 0 when the loop above terminated.
        // Possibly it is 0 because Cilk is over.
        // XXX Is there still a race here?
        if (!atomic_load_explicit(cilkified, memory_order_acquire))
            return 0;

        cond_wait(futexp, 0);
    }
}
#else
static inline uint32_t thief_disengage_cond_var(_Atomic uint32_t *count,
                                                pthread_mutex_t *lock,
                                                pthread_cond_t *cond_var) {
    // This step synchronizes with calls to request_more_thieves.
    pthread_mutex_lock(lock);
    while (true) {
        uint32_t val = atomic_load_explicit(count, memory_order_acquire);
        if (val > 0) {
            atomic_store_explicit(count, val - 1, memory_order_release);
            pthread_mutex_unlock(lock);
            return val;
        }
        pthread_cond_wait(cond_var, lock);
    }
}
#endif
static inline uint32_t thief_disengage(global_state *g) {
#if USE_FUTEX
    return thief_disengage_futex(&g->cilkified, &g->disengaged_thieves);
#else
    return thief_disengage_cond_var(&g->disengaged_thieves,
                                    &g->disengaged_lock,
                                    &g->disengaged_cond_var);
#endif
}

// Signal to all disengaged thief threads to resume work-stealing.
static inline void wake_all_disengaged(global_state *g) {
#if USE_FUTEX
    cond_broadcast(&g->disengaged_thieves, FUTEX_MAX);
#else
    cond_broadcast(&g->disengaged_thieves, FUTEX_MAX,
                   &g->disengaged_cond_var, &g->disengaged_lock);
#endif
}

// Reset global state to make thief threads sleep for signal to start
// work-stealing again.
static inline void sleep_thieves(global_state *g) {
    reset_disengaged_var(g);
}

// Called by a thief thread.  Causes the thief thread to wait for a signal to
// start work-stealing.
static inline uint32_t thief_wait(global_state *g) {
    return thief_disengage(g);
}

// Called by a thief thread.  Check if the thief should start waiting for the
// start of a cilkified region.  If a new cilkified region has been started
// already, update the global state to indicate that this worker is engaged in
// work stealing.
static inline bool thief_should_wait(global_state *g) {
    futex_t *futexp = &g->disengaged_thieves;
    futex_val_t val = atomic_load_explicit(futexp, memory_order_relaxed);
#if USE_FUTEX
    while (val > 0) {
        if (atomic_compare_exchange_weak_explicit(futexp, &val, val - 1,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return false;
        busy_loop_pause();
        val = atomic_load_explicit(futexp, memory_order_relaxed);
    }
    return true;
#else
    if (val == 0)
        return true;

    pthread_mutex_t *lock = &g->disengaged_lock;
    pthread_mutex_lock(lock);
    val = atomic_load_explicit(futexp, memory_order_relaxed);
    if (val > 0) {
        atomic_store_explicit(futexp, val - 1, memory_order_release);
        pthread_mutex_unlock(lock);
        return false;
    }
    pthread_mutex_unlock(lock);
    return true;
#endif
}

// Signal the thief threads to start work-stealing (or terminate, if
// g->terminate == 1).
static inline void wake_thieves(global_state *g) {
#if USE_FUTEX
    cond_broadcast(&g->disengaged_thieves, g->nworkers - 1);
#else
    cond_broadcast(&g->disengaged_thieves, g->nworkers - 1,
                   &g->disengaged_cond_var, &g->disengaged_lock);
#endif
}

#endif /* _WORKER_COORD_H */
