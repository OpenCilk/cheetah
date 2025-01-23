#ifndef _WORKER_SLEEP_H
#define _WORKER_SLEEP_H

#include <stdatomic.h>
#include <stdint.h>
#include <limits.h>
#include <time.h>

#include "cilk-internal.h"
#include "efficiency.h"
#include "global.h"
#include "rts-config.h"
#include "sched_stats.h"
#include "worker_coord.h"

#if defined(__APPLE__) && defined(__aarch64__)
#define APPLE_ARM64
#endif

#ifdef APPLE_ARM64
#include <mach/mach_time.h>
#endif // APPLE_ARM64

// Nanoseconds that a sentinel worker should sleep if it reaches the disengage
// threshold but does not disengage.
/* #define NAP_NSEC 12500 */
#define NAP_NSEC 25000
/* #define NAP_NSEC 50000 */
/* #define SLEEP_NSEC 4 * NAP_NSEC */
#define SLEEP_NSEC NAP_NSEC

// Ratio of active workers over sentinels that the system aims to maintain.
#define AS_RATIO 2

// Threshold for number of consective failed steal attempts to declare a
// thief as sentinel.  Must be a power of 2.
#define SENTINEL_THRESHOLD 128

// Number of attempted steals the thief should do each time it copies the
// worker state.  ATTEMPTS must divide SENTINEL_THRESHOLD.
#define ATTEMPTS 4

// Amount of history that must be efficient/inefficient to reengage/disengage
// workers.
#define HISTORY_THRESHOLD (3 * HISTORY_LENGTH / 4)
/* #define HISTORY_THRESHOLD (1 * HISTORY_LENGTH / 2) */

// Threshold for number of consecutive failed steal attempts to try disengaging
// this worker.  Must be a multiple of SENTINEL_THRESHOLD and a power of 2.
#define DISENGAGE_THRESHOLD HISTORY_THRESHOLD * SENTINEL_THRESHOLD

static inline __attribute__((always_inline)) uint64_t gettime_fast(void) {
    // __builtin_readcyclecounter triggers "illegal instruction" errors on ARM64
    // chips, unless user-level access to the cycle counter has been enabled in
    // the kernel.  Since we cannot rely on that, we use other means to measure
    // the time.
#ifdef APPLE_ARM64
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
#elif defined(__aarch64__)
    struct timespec res;
#ifdef __FreeBSD__
    clock_gettime(CLOCK_MONOTONIC_PRECISE, &res);
#else
    clock_gettime(CLOCK_MONOTONIC_RAW, &res);
#endif
    return (res.tv_sec * 1e9) + (res.tv_nsec);
#else
    return __builtin_readcyclecounter();
#endif
}

typedef struct worker_counts {
    int32_t active;
    int32_t sentinels;
    int32_t disengaged;
} worker_counts;

// Update the index-to-worker map to swap self with the worker at the target
// index.
static void swap_worker_with_target(global_state *g, worker_id self,
                                    worker_id target_index) {
    worker_id *worker_to_index = g->worker_to_index;
    worker_id *index_to_worker = g->index_to_worker;

    worker_id self_index = worker_to_index[self];
    worker_id target_worker = index_to_worker[target_index];

    // Update the index-to-worker map.
    index_to_worker[self_index] = target_worker;
    index_to_worker[target_index] = self;

    // Update the worker-to-index map.
    worker_to_index[target_worker] = self_index;
    worker_to_index[self] = target_index;
}

// These functions return the old value

__attribute__((always_inline)) static inline uint64_t
add_to_sentinels(global_state *const rts, int32_t val) {
    // val is sign extended to 64 bits
    return atomic_fetch_add_explicit(&rts->disengaged_sentinel, val,
                                     memory_order_release);
}

__attribute__((always_inline)) static inline uint64_t
add_to_disengaged(global_state *const rts, int32_t val) {
    return atomic_fetch_add_explicit(&rts->disengaged_sentinel,
                                     DISENGAGED_SENTINEL(val, 0),
                                     memory_order_acquire);
}

#if ENABLE_THIEF_SLEEP
// Called by a thief thread.  Causes the thief thread to try to sleep, that is,
// to wait for a signal to resume work-stealing.
static bool try_to_disengage_thief(global_state *g, worker_id self,
                                   uint64_t disengaged_sentinel) {
    // Try to grab the lock on the index structure.
    if (!cilk_mutex_try(&g->index_lock)) {
        return false;
    }

    // Increment the number of disengaged thieves and decrement number of
    // sentinels.
    uint32_t disengaged = GET_DISENGAGED(disengaged_sentinel);
    uint32_t sentinel = GET_SENTINEL(disengaged_sentinel);
    uint64_t new_disengaged_sentinel =
        DISENGAGED_SENTINEL(disengaged + 1, sentinel - 1);

    unsigned int nworkers = g->nworkers;
    worker_id *worker_to_index = g->worker_to_index;

    // Try to update the number of disengaged workers.  This step synchronizes
    // with parallel calls to reengage thieves, calls to reengage thieves, and
    // updates to the number of sentinel workers.
    // First atomically update the number of disengaged workers.
    // The compare and exchange fails if the sentinel or disenaged
    // count has changed.
    if (atomic_compare_exchange_strong_explicit(
            &g->disengaged_sentinel, &disengaged_sentinel,
            new_disengaged_sentinel, memory_order_release,
            memory_order_acquire)) {
        // Update the index-to-worker map.
        worker_id last_index = nworkers - (new_disengaged_sentinel >> 32);
        if (worker_to_index[self] < last_index) {
            swap_worker_with_target(g, self, last_index);
        }
        // Release the lock on the index structure
        cilk_mutex_unlock(&g->index_lock);

        // Disengage this thread.
        thief_disengage(g);

        // The thread is now reengaged.  Grab the lock on the index structure.
        cilk_mutex_lock(&g->index_lock);

        // Decrement the number of disengaged workers.
        uint64_t disengaged_sentinel =
            atomic_fetch_add(&g->disengaged_sentinel,
                             DISENGAGED_SENTINEL(-1, 1));

        last_index = nworkers - GET_DISENGAGED(disengaged_sentinel);
        if (worker_to_index[self] > last_index) {
            swap_worker_with_target(g, self, last_index);
        }

        // Release the lock on the index structure.
        cilk_mutex_unlock(&g->index_lock);
        return true;
    } else {
        // Release the lock on the index structure.
        cilk_mutex_unlock(&g->index_lock);
        return false;
    }
}
#endif // ENABLE_THIEF_SLEEP

// Helper function to parse the given value of disengaged_sentinel to determine
// the number of active, sentinel, and disengaged workers.
__attribute__((const, always_inline)) static inline worker_counts
get_worker_counts(uint64_t disengaged_sentinel, unsigned int nworkers) {
    uint32_t disengaged = GET_DISENGAGED(disengaged_sentinel);
    uint32_t sentinel = GET_SENTINEL(disengaged_sentinel);
    CILK_ASSERT(disengaged < nworkers);
    CILK_ASSERT(sentinel <= nworkers);
    CILK_ASSERT(sentinel + disengaged <= nworkers);
    int32_t active =
        (int32_t)nworkers - (int32_t)disengaged - (int32_t)sentinel;

    worker_counts counts = {
        .active = active, .sentinels = sentinel, .disengaged = disengaged};
    return counts;
}

// Check if the given worker counts are inefficient, i.e., if active <
// sentinels.
__attribute__((const, always_inline)) static inline history_sample_t
is_inefficient(worker_counts counts) {
    return counts.sentinels > 1 && counts.active >= 1 &&
           counts.active * AS_RATIO < counts.sentinels * 1;
}

// Check if the given worker counts are efficient, i.e., if active >= 2 *
// sentinels.
__attribute__((const, always_inline)) static inline history_sample_t
is_efficient(worker_counts counts) {
    return (counts.active * 1 >= counts.sentinels * AS_RATIO) ||
           (counts.sentinels <= 1);
}

// Convert the elapsed time spent working into a fail count.
__attribute__((const, always_inline)) static inline unsigned int
get_scaled_elapsed(unsigned int elapsed) {
#ifdef __aarch64__
    return ((elapsed * (2 * SENTINEL_THRESHOLD) / (1 * 65536)) / ATTEMPTS) *
           ATTEMPTS;
#else
    return ((elapsed * (1 * SENTINEL_THRESHOLD) / (1 * 65536)) / ATTEMPTS) *
           ATTEMPTS;
#endif // APPLE_ARM64
}

// If steal attempts found work, update histories as appropriate and possibly
// reengage workers.
__attribute__((always_inline)) static inline unsigned int
maybe_reengage_workers(global_state *const rts, worker_id self,
                       unsigned int nworkers, __cilkrts_worker *const w,
                       unsigned int fails,
                       unsigned int *const sample_threshold,
                       history_sample_t *const inefficient_history,
                       history_sample_t *const efficient_history,
                       unsigned int *const sentinel_count_history,
                       unsigned int *const sentinel_count_history_tail,
                       unsigned int *const recent_sentinel_count) {
#if !ENABLE_THIEF_SLEEP
    return 0;
#endif
    (void)w; // unused if scheduling stats not enabled

    if (fails >= SENTINEL_THRESHOLD) {
        // This thief is no longer a sentinel.  Decrement the number of
        // sentinels.
        uint64_t disengaged_sentinel = add_to_sentinels(rts, -1);
        // Get the current worker counts, with this sentinel now active.
        worker_counts counts =
            get_worker_counts(disengaged_sentinel - 1, nworkers);
        CILK_ASSERT(counts.active >= 1);

        history_sample_t my_efficient_history = *efficient_history;
        history_sample_t my_inefficient_history = *inefficient_history;
        unsigned int my_sentinel_count = *recent_sentinel_count;
        if (fails >= *sample_threshold) {
            // Update the inefficient history.
            history_sample_t curr_ineff = is_inefficient(counts);
            my_inefficient_history = (my_inefficient_history >> 1) |
                                     (curr_ineff << (HISTORY_LENGTH - 1));

            // Update the efficient history.
            history_sample_t curr_eff = is_efficient(counts);
            my_efficient_history = (my_efficient_history >> 1) |
                                   (curr_eff << (HISTORY_LENGTH - 1));

            // Update the sentinel count.
            unsigned int current_sentinel_count = counts.sentinels + 1;
            unsigned int tail = *sentinel_count_history_tail;
            my_sentinel_count = my_sentinel_count -
                                sentinel_count_history[tail] +
                                current_sentinel_count;
            *recent_sentinel_count = my_sentinel_count;
            sentinel_count_history[tail] = current_sentinel_count;
            *sentinel_count_history_tail = (tail + 1) % SENTINEL_COUNT_HISTORY;
        }

        // Request to reengage some thieves, depending on whether there are
        // too many active workers compared to sentinel workers.

        // Compute a number of additional workers to request, based on the
        // efficiency history divided by the average recent sentinel count.
        //
        // Dividing by the average recent sentinel count is intended to
        // handle the case where sentinels request more workers in parallel,
        // based on the same independently collected history.
        int32_t request;
        int32_t eff_steps = __builtin_popcount(my_efficient_history);
        int32_t ineff_steps = __builtin_popcount(my_inefficient_history);
        int32_t eff_diff = eff_steps - ineff_steps;
        if (eff_diff < HISTORY_THRESHOLD) {
            request = 0;
            *efficient_history = my_efficient_history;
            *inefficient_history = my_inefficient_history;
        } else {
            unsigned int avg_sentinels =
                my_sentinel_count / SENTINEL_COUNT_HISTORY;
            request = eff_diff / avg_sentinels;
            int32_t remainder = eff_diff % avg_sentinels;
            if (remainder)
                request += (self % remainder != 0);
            // Charge the request for more workers against the efficiency
            // history by resetting that history.
            *efficient_history = 0;
            *inefficient_history = 0;
        }
        WHEN_SCHED_STATS(w->l->stats.reeng_rqsts += request);

        // Make sure at least 1 worker is requested if we're about to run
        // out of sentinels.
        if (request == 0 && counts.sentinels == 0 &&
            counts.active < (int32_t)nworkers) {
            int32_t current_request = atomic_load_explicit(
                &rts->disengaged_thieves_futex, memory_order_relaxed);
            if (current_request < ((counts.active + 3) / 4)) {
                request = ((counts.active + 3) / 4) - current_request;
                WHEN_SCHED_STATS(w->l->stats.onesen_rqsts += request);
            }
        }

        if (request > 0) {
            request_more_thieves(rts, request);
        }

        // Set a cap on the fail count.
        if (fails > SENTINEL_THRESHOLD) {
            fails = SENTINEL_THRESHOLD;
        }

        // Update request threshold so that, in case this worker ends up
        // executing a small task, it still adds samples to its history that
        // are spread out in time.
        *sample_threshold = fails + (SENTINEL_THRESHOLD / 1);
    }

    return fails;
}

#if ENABLE_THIEF_SLEEP
// Attempt to disengage this thief thread.  The __cilkrts_worker parameter is only
// used for debugging.
static bool maybe_disengage_thief(global_state *g, worker_id self,
                                  unsigned int nworkers) {
    // Check the number of active and sentinel workers, and disengage this
    // worker if there are too many sentinel workers.
    while (true) {
        // Check if this sentinel thread should sleep.
        uint64_t disengaged_sentinel =
            atomic_load_explicit(&g->disengaged_sentinel, memory_order_acquire);

        worker_counts counts = get_worker_counts(disengaged_sentinel, nworkers);

        // Make sure that we don't inadvertently disengage the last sentinel.
        if (is_inefficient(counts)) {
            // Too many sentinels.  Try to disengage this worker.  If it fails,
            // repeat the loop.
            if (try_to_disengage_thief(g, self, disengaged_sentinel)) {
                // The thief was successfully disengaged. It has since been
                // reengaged.
                return true;
            }
        } else {
            break;
        }
        busy_loop_pause();
    }
    return false;
}
#endif // ENABLE_THIEF_SLEEP

// If steal attempts did not find work, update histories as appropriate and
// possibly disengage this worker.
__attribute__((always_inline)) static inline unsigned int
handle_failed_steal_attempts(global_state *const rts, worker_id self,
                             unsigned int nworkers, const unsigned int NAP_THRESHOLD,
                             __cilkrts_worker *const w,
                             unsigned int fails,
                             unsigned int *const sample_threshold,
                             history_sample_t *const inefficient_history,
                             history_sample_t *const efficient_history,
                             unsigned int *const sentinel_count_history,
                             unsigned int *const sentinel_count_history_tail,
                             unsigned int *const recent_sentinel_count) {
    (void)w; // only used when timing is enabled

    const bool is_boss = (0 == self);
    // Threshold for number of failed steal attempts to put this thief to sleep
    // for an extended amount of time.  Must be at least SENTINEL_THRESHOLD and
    // a power of 2.
    const unsigned int SLEEP_THRESHOLD = NAP_THRESHOLD;
    const unsigned int MAX_FAILS =
        2 * ((SLEEP_THRESHOLD > DISENGAGE_THRESHOLD) ? SLEEP_THRESHOLD
                                                     : DISENGAGE_THRESHOLD);

    CILK_START_TIMING(w, INTERVAL_SLEEP);
    fails += ATTEMPTS;

    // Every SENTINEL_THRESHOLD consecutive failed steal attempts, update the
    // set of sentinel workers, and maybe disengage this worker if there are too
    // many sentinel workers.
    if (fails % SENTINEL_THRESHOLD == 0) {
        if (fails > MAX_FAILS) {
            // Prevent the fail count from exceeding this maximum, so we don't
            // have to worry about the fail count overflowing.
            fails = MAX_FAILS;
            const struct timespec sleeptime = {.tv_sec = 0, .tv_nsec = SLEEP_NSEC};
            nanosleep(&sleeptime, NULL);
        } else {
#if ENABLE_THIEF_SLEEP
            if (SENTINEL_THRESHOLD == fails) {
                add_to_sentinels(rts, 1);
            }

            // Check the current worker counts.
            uint64_t disengaged_sentinel = atomic_load_explicit(
                &rts->disengaged_sentinel, memory_order_acquire);
            worker_counts counts =
                get_worker_counts(disengaged_sentinel, nworkers);

            // Update the sentinel count.
            unsigned int current_sentinel_count = counts.sentinels;
            unsigned int tail = *sentinel_count_history_tail;
            *recent_sentinel_count = *recent_sentinel_count -
                                     sentinel_count_history[tail] +
                                     current_sentinel_count;
            sentinel_count_history[tail] = current_sentinel_count;
            *sentinel_count_history_tail = (tail + 1) % SENTINEL_COUNT_HISTORY;

            // Update the efficient history.
            history_sample_t curr_eff = is_efficient(counts);
            history_sample_t my_efficient_history = *efficient_history;
            my_efficient_history = (my_efficient_history >> 1) |
                                   (curr_eff << (HISTORY_LENGTH - 1));
            int32_t eff_steps = __builtin_popcount(my_efficient_history);
            *efficient_history = my_efficient_history;

            // Update the inefficient history.
            history_sample_t curr_ineff = is_inefficient(counts);
            history_sample_t my_inefficient_history = *inefficient_history;
            my_inefficient_history = (my_inefficient_history >> 1) |
                                     (curr_ineff << (HISTORY_LENGTH - 1));
            int32_t ineff_steps =
                __builtin_popcount(my_inefficient_history);
            *inefficient_history = my_inefficient_history;

#endif
            if (is_boss) {
                if (fails % NAP_THRESHOLD == 0) {
                    // The boss thread should never disengage.  Sleep instead.
                    const struct timespec sleeptime = {
                        .tv_sec = 0,
                        .tv_nsec =
                            (fails > SLEEP_THRESHOLD) ? SLEEP_NSEC : NAP_NSEC};
                    nanosleep(&sleeptime, NULL);
                }
            } else {
#if ENABLE_THIEF_SLEEP

                if (ENABLE_THIEF_SLEEP && curr_ineff &&
                    (ineff_steps - eff_steps) > HISTORY_THRESHOLD) {
                    uint64_t start, end;
                    start = gettime_fast();
                    if (maybe_disengage_thief(rts, self, nworkers)) {
                        // The semaphore for reserving workers may have been
                        // non-zero due to past successful steals, rather than a
                        // recent successful steal.  Decrement fails so we try
                        // to disengage this again sooner, in case there is
                        // still nothing to steal.
                        end = gettime_fast();
                        unsigned int scaled_elapsed =
                            get_scaled_elapsed(end - start);

                        // Update histories
                        if (scaled_elapsed > SENTINEL_THRESHOLD) {
                            uint32_t samples =
                                scaled_elapsed / SENTINEL_THRESHOLD;
                            if (samples >= HISTORY_LENGTH) {
                                *efficient_history = 0;
                                *inefficient_history = 0;

                                // Update the sentinel count.
                                uint64_t disengaged_sentinel =
                                    atomic_load_explicit(
                                        &rts->disengaged_sentinel,
                                        memory_order_relaxed);
                                uint32_t current_sentinel_count =
                                    GET_SENTINEL(disengaged_sentinel);
                                for (int i = 0; i < SENTINEL_COUNT_HISTORY; ++i)
                                    sentinel_count_history[i] =
                                        current_sentinel_count;
                                *recent_sentinel_count =
                                    current_sentinel_count *
                                    SENTINEL_COUNT_HISTORY;

                            } else {
                                *efficient_history >>= samples;
                                *inefficient_history >>= samples;
                            }
                        }

                        // Update fail count
                        if (scaled_elapsed < SENTINEL_THRESHOLD) {
                            fails -= scaled_elapsed;
                        } else {
                            fails = DISENGAGE_THRESHOLD - SENTINEL_THRESHOLD;
                        }
                        *sample_threshold = SENTINEL_THRESHOLD;
                    } else if (fails % NAP_THRESHOLD == 0) {
                        // We have enough active workers to keep this worker
                        // engaged, but this worker was still unable to steal
                        // work.  Put this thief to sleep for a while using the
                        // conventional way. In testing, a nanosleep(0) takes
                        // approximately 50 us.
                        const struct timespec sleeptime = {
                            .tv_sec = 0,
                            .tv_nsec = (fails > SLEEP_THRESHOLD) ? SLEEP_NSEC
                                                                 : NAP_NSEC};
                        nanosleep(&sleeptime, NULL);
                    }
#else
                if (false) {
#endif
                } else if (fails % NAP_THRESHOLD == 0) {
                    // We have enough active workers to keep this worker
                    // engaged, but this worker was still unable to steal work.
                    // Put this thief to sleep for a while using the
                    // conventional way. In testing, a nanosleep(0) takes
                    // approximately 50 us.
                    const struct timespec sleeptime = {
                        .tv_sec = 0,
                        .tv_nsec =
                            (fails > SLEEP_THRESHOLD) ? SLEEP_NSEC : NAP_NSEC};
                    nanosleep(&sleeptime, NULL);
                }
            }
        }
    }
    CILK_STOP_TIMING(w, INTERVAL_SLEEP);
    return fails;
}

__attribute__((always_inline))
static unsigned int go_to_sleep_maybe(global_state *const rts, worker_id self,
                                      unsigned int nworkers,
                                      const unsigned int NAP_THRESHOLD,
                                      __cilkrts_worker *const w,
                                      Closure *const t, unsigned int fails,
                                      unsigned int *const sample_threshold,
                                      history_sample_t *const inefficient_history,
                                      history_sample_t *const efficient_history,
                                      unsigned int *const sentinel_count_history,
                                      unsigned int *const sentinel_count_history_tail,
                                      unsigned int *const recent_sentinel_count) {
    if (t) {
        return maybe_reengage_workers(
            rts, self, nworkers, w, fails, sample_threshold,
            inefficient_history, efficient_history, sentinel_count_history,
            sentinel_count_history_tail, recent_sentinel_count);
    } else {
        return handle_failed_steal_attempts(
            rts, self, nworkers, NAP_THRESHOLD, w, fails, sample_threshold,
            inefficient_history, efficient_history, sentinel_count_history,
            sentinel_count_history_tail, recent_sentinel_count);
    }
}

#if ENABLE_THIEF_SLEEP
__attribute__((always_inline)) static unsigned int
decrease_fails_by_work(global_state *const rts,
                       unsigned int fails, uint64_t elapsed,
                       unsigned int *const sample_threshold) {
    uint64_t scaled_elapsed = get_scaled_elapsed(elapsed);

    // Decrease the number of fails based on the work done.
    if (scaled_elapsed > (uint64_t)fails) {
        fails = 0;
    } else {
        fails -= scaled_elapsed;
    }

    // The fail count must be a multiple of ATTEMPTS for the sleep logic to
    // work.
    CILK_ASSERT(fails % ATTEMPTS == 0);

    if (scaled_elapsed > (uint64_t)(*sample_threshold) - SENTINEL_THRESHOLD)
        *sample_threshold = SENTINEL_THRESHOLD;
    else
        *sample_threshold -= scaled_elapsed;

    // If this worker is still sentinel, update sentinel-worker count.
    if (fails >= SENTINEL_THRESHOLD)
        add_to_sentinels(rts, 1);
    return fails;
}
#endif // ENABLE_THIEF_SLEEP

__attribute__((always_inline)) static unsigned int
init_fails(uint32_t wake_val, global_state *rts) {
    // It's possible that a disengaged worker is woken up by a call to
    // request_more_thieves, in which case it should be a sentinel.  But there
    // isn't a direct way to tell how whether the worker should be active or a
    // sentinel when it's woken up.  Since the maximum value of the futex when
    // sentinels are engaging and disengaging during Cilk execution is
    // nworkers/2, we simply assume that if the value of the futex is less than
    // that value, then it should be a sentinel.
    //
    // As a result, when workers are woken up to start executing any new Cilk
    // function, half of them will be active, and half sentinels.
    if (wake_val <= (rts->nworkers / 2)) {
        atomic_fetch_add_explicit(&rts->disengaged_sentinel, 1,
                                  memory_order_release);
        return SENTINEL_THRESHOLD;
    }
    return 0;
}

#if ENABLE_THIEF_SLEEP
__attribute__((always_inline)) static unsigned int
reset_fails(global_state *rts, unsigned int fails) {
    if (fails >= SENTINEL_THRESHOLD) {
        // If this worker was sentinel, decrement the number of sentinel
        // workers, effectively making this worker active.
        add_to_sentinels(rts, -1);
    }
    return 0;
}
#endif // ENABLE_THIEF_SLEEP

__attribute__((always_inline)) static inline void
disengage_worker(global_state *g, unsigned int nworkers, worker_id self) {
    cilk_mutex_lock(&g->index_lock);
    uint64_t disengaged_sentinel = add_to_disengaged(g, 1);
    // Update the index-to-worker map.  We derive last_index from the new value
    // of disengaged_sentinel, because the index is now invalid.
    worker_id last_index = nworkers - ((disengaged_sentinel >> 32) + 1);
    if (g->worker_to_index[self] < last_index) {
        swap_worker_with_target(g, self, last_index);
    }
    // Release the lock on the index structure
    cilk_mutex_unlock(&g->index_lock);
}

__attribute__((always_inline)) static inline void
reengage_worker(global_state *g, unsigned int nworkers, worker_id self) {
    cilk_mutex_lock(&g->index_lock);
    uint64_t disengaged_sentinel = add_to_disengaged(g, -1);
    // Update the index-to-worker map.  We derive last_index from the old value
    // of disengaged_sentinel, because the index is now valid.
    worker_id last_index = nworkers - (disengaged_sentinel >> 32);
    if (g->worker_to_index[self] > last_index) {
        swap_worker_with_target(g, self, last_index);
    }
    // Release the lock on the index structure
    cilk_mutex_unlock(&g->index_lock);
}

#endif /* _WORKER_SLEEP_H */
