#ifndef _CILK_GLOBAL_H
#define _CILK_GLOBAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <stdatomic.h> /* must follow stdbool.h */

#include "debug.h"
#include "fiber.h"
#include "internal-malloc-impl.h"
#include "jmpbuf.h"
#include "mutex.h"
#include "rts-config.h"
#include "sched_stats.h"
#include "types.h"

extern unsigned cilkg_nproc;

struct __cilkrts_worker;
struct reducer_id_manager;
struct Closure;

// clang-format off
#define DEFAULT_OPTIONS                                            \
    {                                                              \
        DEFAULT_STACK_SIZE,     /* stack size to use for fiber */  \
        DEFAULT_NPROC,          /* num of workers to create */     \
        DEFAULT_REDUCER_LIMIT,  /* num of simultaneous reducers */ \
        DEFAULT_DEQ_DEPTH,      /* num of entries in deque */      \
        DEFAULT_FIBER_POOL_CAP, /* alloc_batch_size */             \
        DEFAULT_FORCE_REDUCE,   /* whether to force self steal and reduce */\
    }
// clang-format on

struct rts_options {
    size_t stacksize;            /* can be set via env variable CILK_STACKSIZE */
    unsigned int nproc;          /* can be set via env variable CILK_NWORKERS */
    unsigned int reducer_cap;
    unsigned int deqdepth;       /* can be set via env variable CILK_DEQDEPTH */
    unsigned int fiber_pool_cap; /* can be set via env variable CILK_FIBER_POOL */
    unsigned int force_reduce;   /* can be set via env variable CILK_FORCE_REDUCE */
};

struct global_state {
    /* globally-visible options (read-only after init) */
    struct rts_options options;

    unsigned int nworkers; /* size of next 3 arrays */
    struct __cilkrts_worker **workers;
    /* dynamically-allocated array of deques, one per processor */
    struct ReadyDeque *deques;
    pthread_t *threads;
    struct Closure *root_closure;

    struct cilk_fiber_pool fiber_pool __attribute__((aligned(CILK_CACHE_LINE)));
    struct global_im_pool im_pool __attribute__((aligned(CILK_CACHE_LINE)));
    struct cilk_im_desc im_desc __attribute__((aligned(CILK_CACHE_LINE)));
    cilk_mutex im_lock; // lock for accessing global im_desc

    // These fields are accessed exclusively by the boss thread.

    jmpbuf boss_ctx __attribute__((aligned(CILK_CACHE_LINE)));
    void *orig_rsp;
    volatile bool workers_started;

    // These fields are shared between the boss thread and a couple workers.

    _Atomic uint32_t start_root_worker __attribute__((aligned(CILK_CACHE_LINE)));
    // NOTE: We can probably update the runtime system so that, when it uses
    // cilkified_futex, it does not also use the cilkified field.  But the
    // cilkified field is helpful for debugging, and it seems unlikely that this
    // optimization would improve performance.
    _Atomic uint32_t cilkified_futex;
    volatile worker_id exiting_worker;
    volatile atomic_bool cilkified;

    pthread_mutex_t start_root_worker_lock;
    pthread_cond_t start_root_worker_cond_var;
    pthread_mutex_t cilkified_lock;
    pthread_cond_t cilkified_cond_var;

    // These fields are shared among all workers in the work-stealing loop.

    volatile atomic_bool done __attribute__((aligned(CILK_CACHE_LINE)));
    volatile bool terminate;
    volatile bool root_closure_initialized;

    worker_id *index_to_worker __attribute__((aligned(CILK_CACHE_LINE)));
    worker_id *worker_to_index;
    cilk_mutex index_lock;

    // Count of number of disengaged and deprived workers.  Upper 32 bits count
    // the disengaged workers.  Lower 32 bits count the deprived workers.  These
    // two counts are stored in a single word to make it easier to update both
    // counts atomically.
    _Atomic uint64_t disengaged_deprived __attribute__((aligned(CILK_CACHE_LINE)));

    _Atomic uint32_t disengaged_thieves_futex __attribute__((aligned(CILK_CACHE_LINE)));

    pthread_mutex_t disengaged_lock;
    pthread_cond_t disengaged_cond_var;

    cilk_mutex print_lock; // global lock for printing messages

    struct reducer_id_manager *id_manager; /* null while Cilk is running */

    struct global_sched_stats stats;
};

extern global_state *default_cilkrts;

CHEETAH_INTERNAL void set_nworkers(global_state *g, unsigned int nworkers);
CHEETAH_INTERNAL void set_force_reduce(global_state *g,
                                       unsigned int force_reduce);
CHEETAH_INTERNAL global_state *global_state_init(int argc, char *argv[]);
CHEETAH_INTERNAL void for_each_worker(global_state *,
                                      void (*)(__cilkrts_worker *, void *),
                                      void *data);
CHEETAH_INTERNAL void for_each_worker_rev(global_state *,
                                          void (*)(__cilkrts_worker *, void *),
                                          void *data);

// util functions used by both init.c and global.c
inline static long env_get_int(char const *var) {
    const char *envstr = getenv(var);
    if (envstr)
        return strtol(envstr, NULL, 0);
    return 0;
}

#endif /* _CILK_GLOBAL_H */
