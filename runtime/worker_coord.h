#ifndef _WORKER_COORD_H
#define _WORKER_COORD_H

// Routines for coordinating workers, specifically, putting workers to sleep and
// waking workers when execution enters and leaves cilkified regions.

#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>

#ifdef __linux__
#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "global.h"

#define USER_USE_FUTEX 1
#ifdef __linux__
#define USE_FUTEX USER_USE_FUTEX
#else
#define USE_FUTEX 0
#endif

static inline void maybe_finish_waking_thieves(global_state *const g, uint32_t local_wake, uint32_t nworkers);

#if USE_FUTEX
//=========================================================
// Primitive futex operations.
//=========================================================
#define errExit(msg)                                                           \
    do {                                                                       \
        perror(msg);                                                           \
        exit(EXIT_FAILURE);                                                    \
    } while (false)

static inline uint32_t take_current_wake_value_futex(_Atomic uint32_t *const futexp);

// Convenience wrapper for futex syscall.
static inline long futex(_Atomic uint32_t *uaddr, int futex_op, uint32_t val,
                         const struct timespec *timeout, uint32_t *uaddr2,
                         uint32_t val3) {
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

// Wait for the futex pointed to by `futexp` to become 1.
static inline void fwait(_Atomic uint32_t *futexp) {
    // We don't worry about spurious wakeups here, since we ensure that all
    // calls to fwait are contained in their own loops that effectively check
    // for spurious wakeups.
    long s = futex(futexp, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    if (__builtin_expect(s == -1 && errno != EAGAIN, false))
        errExit("futex-FUTEX_WAIT");
}

// Set the futex pointed to by `futexp` to 1, and wake up 1 thread waiting on
// that futex.
static inline void fpost(_Atomic uint32_t *futexp) {
    atomic_store_explicit(futexp, 1, memory_order_release);
    long s = futex(futexp, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    if (s == -1)
        errExit("futex-FUTEX_WAKE");
}

// Set the futex pointed to by `futexp` to 1, and wake up all threads waiting on
// that futex.
static inline void fbroadcast(_Atomic uint32_t *futexp) {
    atomic_store_explicit(futexp, 1, memory_order_release);
    long s = futex(futexp, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
    if (s == -1)
        errExit("futex-FUTEX_WAKE");
}
#endif

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
#if USE_FUTEX
    atomic_store_explicit(&g->cilkified_futex, 0, memory_order_release);
#endif
}

// Mark the computation as no longer cilkified and signal the thread that
// originally cilkified the execution.
static inline void signal_uncilkified(global_state *g) {
#if USE_FUTEX
    atomic_store_explicit(&g->cilkified, 0, memory_order_release);
    fpost(&g->cilkified_futex);
#else
    pthread_mutex_lock(&(g->cilkified_lock));
    atomic_store_explicit(&g->cilkified, 0, memory_order_release);
    pthread_cond_signal(&g->cilkified_cond_var);
    pthread_mutex_unlock(&(g->cilkified_lock));
#endif
}

// Wait on g->cilkified to be set to 0, indicating the end of the Cilkified
// region.
static inline void wait_while_cilkified(global_state *g) {
    unsigned int fail = 0;
    while (fail++ < BUSY_LOOP_SPIN) {
        if (!atomic_load_explicit(&g->cilkified, memory_order_acquire)) {
            return;
        }
        busy_pause();
    }
#if USE_FUTEX
    while (atomic_load_explicit(&g->cilkified, memory_order_acquire)) {
        fwait(&g->cilkified_futex);
    }
#else
    // TODO: Convert pthread_mutex_lock, pthread_mutex_unlock, and
    // pthread_cond_wait to cilk_* equivalents.
    pthread_mutex_lock(&(g->cilkified_lock));

    // There may be a *very unlikely* scenario where the Cilk computation has
    // already been completed before even starting to wait.  In that case, do
    // not wait and continue directly.  Also handle spurious wakeups with a
    // 'while' instead of an 'if'.
    while (atomic_load_explicit(&g->cilkified, memory_order_acquire)) {
        pthread_cond_wait(&(g->cilkified_cond_var), &(g->cilkified_lock));
    }

    pthread_mutex_unlock(&(g->cilkified_lock));
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
    atomic_store_explicit(&g->disengaged_thieves_futex, 0,
                          memory_order_release);
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
        uint32_t disengaged_thieves_futex = atomic_load_explicit(
            &g->disengaged_thieves_futex, memory_order_acquire);

        int32_t max_to_wake = max_requests - disengaged_thieves_futex;
        if (max_to_wake <= 0)
            return;
        uint64_t to_wake = max_to_wake < (int32_t)count ? max_to_wake : count;

        if (atomic_compare_exchange_strong_explicit(
                &g->disengaged_thieves_futex, &disengaged_thieves_futex,
                disengaged_thieves_futex + to_wake, memory_order_release,
                memory_order_relaxed)) {
            // We successfully updated the futex.  Wake the thief threads
            // waiting on this futex.
            long s = futex(&g->disengaged_thieves_futex, FUTEX_WAKE_PRIVATE,
                           to_wake, NULL, NULL, 0);
            if (s == -1)
                errExit("futex-FUTEX_WAKE");
            return;
        }
    }
#else
    pthread_mutex_lock(&g->disengaged_lock);
    uint32_t disengaged_thieves_futex = atomic_load_explicit(
        &g->disengaged_thieves_futex, memory_order_acquire);

    int32_t max_to_wake = max_requests - disengaged_thieves_futex;
    if (max_to_wake <= 0) {
        pthread_mutex_unlock(&g->disengaged_lock);
        return;
    }
    uint32_t to_wake = max_to_wake < (int32_t)count ? max_to_wake : count;
    atomic_store_explicit(&g->disengaged_thieves_futex,
                          disengaged_thieves_futex + to_wake,
                          memory_order_release);
    while (to_wake-- > 0) {
        pthread_cond_signal(&g->disengaged_cond_var);
    }
    pthread_mutex_unlock(&g->disengaged_lock);
#endif
}

#if USE_FUTEX
static inline uint32_t thief_disengage_futex(_Atomic uint32_t *futexp) {
    uint32_t wake_val = 0;
    // This step synchronizes with calls to request_more_thieves.
    while (true) {
        // Decrement the futex when woken up.  The loop and compare-exchange are
        // designed to handle cases where multiple threads waiting on the futex
        // were woken up and where there may be spurious wakeups.
        wake_val = take_current_wake_value_futex(futexp);
        if (wake_val > 0) {
            break;
        }

        // Wait on the futex.
        long s = futex(futexp, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
        if (__builtin_expect(s == -1 && errno != EAGAIN, false))
            errExit("futex-FUTEX_WAIT");
    }

    return wake_val;
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
static uint32_t thief_disengage(global_state *g) {
    uint32_t wake_val =
#if USE_FUTEX
        thief_disengage_futex(&g->disengaged_thieves_futex);
#else
        thief_disengage_cond_var(&g->disengaged_thieves_futex,
                                 &g->disengaged_lock,
                                 &g->disengaged_cond_var);
#endif

    maybe_finish_waking_thieves(g, wake_val, g->nworkers);

    return wake_val;
}

// Signal to all disengaged thief threads to resume work-stealing.
static inline void wake_all_disengaged(global_state *g) {
#if USE_FUTEX
    atomic_store_explicit(&g->disengaged_thieves_futex, INT_MAX,
                          memory_order_release);
    long s = futex(&g->disengaged_thieves_futex, FUTEX_WAKE_PRIVATE, INT_MAX,
                   NULL, NULL, 0);
    if (s == -1)
        errExit("futex-FUTEX_WAKE");
#else
    pthread_mutex_lock(&g->disengaged_lock);
    atomic_store_explicit(&g->disengaged_thieves_futex, INT_MAX,
                          memory_order_release);
    pthread_cond_broadcast(&g->disengaged_cond_var);
    pthread_mutex_unlock(&g->disengaged_lock);
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

#if USE_FUTEX
static inline uint32_t take_current_wake_value_futex(_Atomic uint32_t *const futexp) {
    uint32_t val = atomic_load_explicit(futexp, memory_order_relaxed);
    while (val > 0) {
        if (atomic_compare_exchange_weak_explicit(futexp, &val, val - 1,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            break;
        busy_loop_pause();
        val = atomic_load_explicit(futexp, memory_order_relaxed);
    }

    return val;
}
#else
static inline uint32_t take_current_wake_value_cv(global_state *const g) {
    _Atomic uint32_t *futexp = &g->disengaged_thieves_futex;
    uint32_t val = atomic_load_explicit(futexp, memory_order_relaxed);
    if (val != 0) {
        pthread_mutex_t *lock = &g->disengaged_lock;
        pthread_mutex_lock(lock);
        val = atomic_load_explicit(futexp, memory_order_relaxed);
        if (val > 0) {
            atomic_store_explicit(futexp, val - 1, memory_order_release);
        }
        pthread_mutex_unlock(lock);
    }

    return val;
}
#endif

static inline uint32_t take_current_wake_value(global_state *const g) {
    uint32_t wake_val =
#if USE_FUTEX
        take_current_wake_value_futex(&g->disengaged_thieves_futex);
#else
        take_current_wake_value_cv(g);
#endif

    maybe_finish_waking_thieves(g, wake_val, g->nworkers);

    return wake_val;
}

// Called by a thief thread.  Check if the thief should start waiting for the
// start of a cilkified region.  If a new cilkified region has been started
// already, update the global state to indicate that this worker is engaged in
// work stealing.
static inline bool thief_should_wait(const uint32_t wake_value) {
    return wake_value == 0u;
}

// Signal the thief threads to start work-stealing (or terminate, if
// g->terminate == 1).
static inline void initiate_waking_thieves(global_state *const g) {
#if USE_FUTEX
    atomic_store_explicit(&g->disengaged_thieves_futex, g->nworkers - 1,
                          memory_order_relaxed);

    atomic_thread_fence(memory_order_seq_cst);

    uint64_t disengaged_sentinel = atomic_load_explicit(
        &g->disengaged_sentinel, memory_order_relaxed);
    uint32_t disengaged = GET_DISENGAGED(disengaged_sentinel);

    // For now, be conservative and wake a worker if any went to sleep.
    if (disengaged == g->nworkers -1u) {
        long s = futex(&g->disengaged_thieves_futex, FUTEX_WAKE_PRIVATE, 1,
                        NULL, NULL, 0);

        if (s == -1) {
            errExit("futex-FUTEX_WAKE");
        }
    }

#else
    pthread_mutex_lock(&g->disengaged_lock);
    atomic_store_explicit(&g->disengaged_thieves_futex, g->nworkers - 1,
                          memory_order_relaxed);

    atomic_thread_fence(memory_order_seq_cst);

    uint64_t disengaged_sentinel = atomic_load_explicit(
        &g->disengaged_sentinel, memory_order_relaxed);
    uint32_t disengaged = GET_DISENGAGED(disengaged_sentinel);

    if (disengaged == g->nworkers - 1u) {
        pthread_cond_signal(&g->disengaged_cond_var);
    }

    pthread_mutex_unlock(&g->disengaged_lock);
#endif
}

static inline void finish_waking_thieves(global_state *const g) {
#if USE_FUTEX
    long s = futex(&g->disengaged_thieves_futex, FUTEX_WAKE_PRIVATE, INT_MAX,
                   NULL, NULL, 0);
    if (s == -1)
        errExit("futex-FUTEX_WAKE");
#else
    pthread_mutex_lock(&g->disengaged_lock);
    pthread_cond_broadcast(&g->disengaged_cond_var);
    pthread_mutex_unlock(&g->disengaged_lock);
#endif
}

static inline void maybe_finish_waking_thieves(global_state *const g, uint32_t local_wake, uint32_t nworkers) {
    if (local_wake == (nworkers - 1u)) {
        finish_waking_thieves(g);
    }
}

#endif /* _WORKER_COORD_H */
