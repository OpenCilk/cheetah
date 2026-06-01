#ifndef _CILK_GLOBAL_H
#define _CILK_GLOBAL_H

#include "debug.h"
#include "efficiency.h"
#include "fiber.h"
#include "internal-malloc-impl.h"
#include "jmpbuf.h"
#include "mutex.h"
#include "rts-config.h"
#include "sched_stats.h"
#include "worker.h"
#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <thread>

extern unsigned __cilkrts_nproc;

struct __cilkrts_worker;
struct Closure;
struct BusyClosure;

// clang-format off
#define DEFAULT_OPTIONS                                            \
    {                                                              \
        DEFAULT_STACK_SIZE,     /* stack size to use for fiber */  \
        DEFAULT_NPROC,          /* num of workers to create */     \
        DEFAULT_DEQ_DEPTH,      /* num of entries in deque */      \
        DEFAULT_FIBER_POOL_CAP  /* alloc_batch_size */             \
    }
// clang-format on

struct rts_options {
    size_t stacksize;      /* can be set via env variable CILK_STACKSIZE */
    unsigned int nproc;    /* can be set via env variable CILK_NWORKERS */
    unsigned int deqdepth; /* can be set via env variable CILK_DEQDEPTH */
    unsigned int
        fiber_pool_cap; /* can be set via env variable CILK_FIBER_POOL */
};

struct worker_args {
    worker_id id = 0;
    global_state *g = nullptr;
};

struct scheduler_event {
    uint64_t time;
    enum event : unsigned short {
        CILKIFY,
        UNCILKIFY,
        WAIT_CILKIFIED,
        WAIT_DISENGAGED,
        MORE_THIEVES,
        ALL_THIEVES,
    } code;
    // 2 byte hole
    int data1;
    worker_id worker;
};

struct CHEETAH_INTERNAL global_state {
    /* globally-visible options (read-only after init) */
    rts_options options;

    unsigned int nworkers; /* size of next 4 arrays */
    worker_args *worker_args;
    __cilkrts_worker **workers;
    /* dynamically-allocated array of busy closures, one per processor */
    BusyClosure *busy;
    std::thread *threads;
    Closure *root_closure;

    alignas(CILK_CACHE_LINE) cilk_fiber_pool fiber_pool;
    alignas(CILK_CACHE_LINE) global_im_pool im_pool;
    alignas(CILK_CACHE_LINE) cilk_im_desc im_desc;
    cilk_mutex im_lock; // lock for accessing global im_desc

    // These fields are accessed exclusively by the boss thread.

    alignas(CILK_CACHE_LINE) jmpbuf boss_ctx;
    void *orig_rsp;
    bool workers_started;

    // This field is shared between the boss thread and a couple workers.

    alignas(CILK_CACHE_LINE) std::atomic<bool> cilkified;

    // These fields are shared among all workers in the work-stealing loop.

    alignas(CILK_CACHE_LINE) std::atomic<bool> done;
    bool terminate;
    bool root_closure_initialized;

    alignas(CILK_CACHE_LINE) worker_id *index_to_worker;
    worker_id *worker_to_index;
    cilk_mutex index_lock;

    // Count of number of disengaged and sentinel workers.  Upper 32 bits count
    // the disengaged workers.  Lower 32 bits count the sentinel workers.  These
    // two counts are stored in a single word to make it easier to update both
    // counts atomically.
    alignas(CILK_CACHE_LINE) std::atomic<uint64_t> disengaged_sentinel;
#define GET_DISENGAGED(D) ((D) >> 32)
#define GET_SENTINEL(D) ((D) & 0xffffffff)
#define DISENGAGED_SENTINEL(A, B) (((uint64_t)(A) << 32) | (uint32_t)(B))

    alignas(CILK_CACHE_LINE) std::atomic<uint32_t> disengaged_thieves;

    cilk_mutex print_lock; // global lock for printing messages

    // This dummy worker structure is used to support lazy initialization of
    // worker structures.  In particular, the global workers array is initially
    // populated with pointers to this dummy worker, so that the main steal loop
    // does not need to check whether it's reading an uninitialized entry in the
    // global workers array.  Instead, this dummy worker will ensure the fast
    // check in Closure_steal always fails.
    __cilkrts_worker dummy_worker;

    global_sched_stats stats;

    uint64_t start_time;

    std::atomic<size_t> event_index;

    scheduler_event events[1024];

    CHEETAH_INTERNAL void set_cilkified();
    CHEETAH_INTERNAL void signal_uncilkified();
    CHEETAH_INTERNAL void wait_while_cilkified();
    CHEETAH_INTERNAL void request_more_thieves(worker_id self, uint32_t count);
    CHEETAH_INTERNAL uint32_t thief_disengage(worker_id self);
    CHEETAH_INTERNAL uint32_t thief_wait(worker_id self);
    CHEETAH_INTERNAL void wake_thieves();
    CHEETAH_INTERNAL bool thief_should_wait();
    // Reset global state to make thief threads sleep for signal to start
    // work-stealing again.
    CHEETAH_INTERNAL void sleep_thieves();
    CHEETAH_INTERNAL void wake_all_disengaged();

    CHEETAH_INTERNAL
    void reengage_worker(unsigned int nworkers, worker_id self);
    CHEETAH_INTERNAL
    void disengage_worker(unsigned int nworkers, worker_id self);
    CHEETAH_INTERNAL
    void swap_worker_with_target(worker_id self, worker_id target_index);

    // These functions return the old value
    uint64_t add_to_disengaged(int32_t val) {
        return disengaged_sentinel.fetch_add(DISENGAGED_SENTINEL(val, 0),
                                             std::memory_order_acquire);
    }
    uint64_t add_to_sentinels(int32_t val) {
        // val is sign extended to 64 bits
        return disengaged_sentinel.fetch_add(val, std::memory_order_release);
    }

    CHEETAH_INTERNAL void record_event(scheduler_event::event, int, worker_id);

    CHEETAH_INTERNAL static uint64_t gettime_fast(void);

#if ENABLE_THIEF_SLEEP
    bool try_to_disengage_thief(worker_id self, uint64_t disengaged_sentinel);
    bool maybe_disengage_thief(worker_id self, unsigned int nworkers);
    unsigned int decrease_fails_by_work(unsigned int fails, uint64_t elapsed,
                                        unsigned int *const sample_threshold);
    void reset_fails(unsigned int fails);
#endif
    unsigned int
    maybe_reengage_workers(worker_id self, unsigned int nworkers,
                           __cilkrts_worker *const w, unsigned int fails,
                           unsigned int *const sample_threshold,
                           history_sample_t *const inefficient_history,
                           history_sample_t *const efficient_history,
                           unsigned int *const sentinel_count_history,
                           unsigned int *const sentinel_count_history_tail,
                           unsigned int *const recent_sentinel_count);
    unsigned int
    go_to_sleep_maybe(worker_id self, unsigned int nworkers,
                      const unsigned int NAP_THRESHOLD,
                      __cilkrts_worker *const w, Closure *const t,
                      unsigned int fails, unsigned int *const sample_threshold,
                      history_sample_t *const inefficient_history,
                      history_sample_t *const efficient_history,
                      unsigned int *const sentinel_count_history,
                      unsigned int *const sentinel_count_history_tail,
                      unsigned int *const recent_sentinel_count);
    unsigned int handle_failed_steal_attempts(
        worker_id self, unsigned int nworkers, const unsigned int NAP_THRESHOLD,
        __cilkrts_worker *const w, unsigned int fails,
        unsigned int *const sample_threshold,
        history_sample_t *const inefficient_history,
        history_sample_t *const efficient_history,
        unsigned int *const sentinel_count_history,
        unsigned int *const sentinel_count_history_tail,
        unsigned int *const recent_sentinel_count);

    unsigned int init_fails(uint32_t wake_val);
};

CHEETAH_INTERNAL extern global_state *default_cilkrts;
CHEETAH_INTERNAL extern __cilkrts_worker default_worker;

CHEETAH_INTERNAL
__cilkrts_worker *__cilkrts_init_tls_worker(worker_id i, global_state *g);
CHEETAH_INTERNAL void set_nworkers(global_state *g, unsigned int nworkers);
CHEETAH_INTERNAL global_state *global_state_init(int argc, char *argv[]);
CHEETAH_INTERNAL void for_each_worker(global_state *,
                                      void (*)(__cilkrts_worker *, void *),
                                      void *data);
CHEETAH_INTERNAL void for_each_worker_rev(global_state *,
                                          void (*)(__cilkrts_worker *, void *),
                                          void *data);

// util functions used by both init.c and global.c
inline static long env_get_int(char const *var, int unset = 0) {
    const char *envstr = getenv(var);
    if (envstr)
        return strtol(envstr, NULL, 0);
    return unset;
}

inline static bool worker_is_valid(const __cilkrts_worker *w,
                                   const global_state *g) {
    return w != &g->dummy_worker;
}

#endif /* _CILK_GLOBAL_H */
