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
#include "worker.h"

extern unsigned __cilkrts_nproc;

struct __cilkrts_worker;
struct Closure;

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
    size_t stacksize;            /* can be set via env variable CILK_STACKSIZE */
    unsigned int nproc;          /* can be set via env variable CILK_NWORKERS */
    unsigned int deqdepth;       /* can be set via env variable CILK_DEQDEPTH */
    unsigned int fiber_pool_cap; /* can be set via env variable CILK_FIBER_POOL */
};

struct worker_args {
    worker_id id;
    global_state *g;
};

struct global_state {
    /* globally-visible options (read-only after init) */
    struct rts_options options;

    unsigned int nworkers; /* size of next 4 arrays */
    struct worker_args *worker_args;
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
    bool workers_started;

    // These fields are shared between the boss thread and a couple workers.

    // NOTE: We can probably update the runtime system so that, when it uses
    // cilkified_futex, it does not also use the cilkified field.  But the
    // cilkified field is helpful for debugging, and it seems unlikely that this
    // optimization would improve performance.
    _Atomic uint32_t cilkified_futex __attribute__((aligned(CILK_CACHE_LINE)));
    atomic_bool cilkified;

    pthread_mutex_t cilkified_lock;
    pthread_cond_t cilkified_cond_var;

    // These fields are shared among all workers in the work-stealing loop.

    atomic_bool done __attribute__((aligned(CILK_CACHE_LINE)));
    bool terminate;
    bool root_closure_initialized;

    worker_id *index_to_worker __attribute__((aligned(CILK_CACHE_LINE)));
    worker_id *worker_to_index;
    cilk_mutex index_lock;

    // Count of number of disengaged and sentinel workers.  Upper 32 bits count
    // the disengaged workers.  Lower 32 bits count the sentinel workers.  These
    // two counts are stored in a single word to make it easier to update both
    // counts atomically.
    _Atomic uint64_t disengaged_sentinel __attribute__((aligned(CILK_CACHE_LINE)));
#define GET_DISENGAGED(D) ((D) >> 32)
#define GET_SENTINEL(D) ((D) & 0xffffffff)
#define DISENGAGED_SENTINEL(A, B) (((uint64_t)(A) << 32) | (uint32_t)(B))

    _Atomic uint32_t disengaged_thieves_futex __attribute__((aligned(CILK_CACHE_LINE)));

    pthread_mutex_t disengaged_lock;
    pthread_cond_t disengaged_cond_var;

    cilk_mutex print_lock; // global lock for printing messages

    // This dummy worker structure is used to support lazy initialization of
    // worker structures.  In particular, the global workers array is initially
    // populated with pointers to this dummy worker, so that the main steal loop
    // does not need to check whether it's reading an uninitialized entry in the
    // global workers array.  Instead, this dummy worker will ensure the fast
    // check in Closure_steal always fails.
    struct __cilkrts_worker dummy_worker;

    struct global_sched_stats stats;
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
inline static long env_get_int(char const *var) {
    const char *envstr = getenv(var);
    if (envstr)
        return strtol(envstr, NULL, 0);
    return 0;
}

inline static bool worker_is_valid(const __cilkrts_worker *w,
                                   const global_state *g) {
    return w != &g->dummy_worker;
}

#endif /* _CILK_GLOBAL_H */
