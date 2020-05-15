#ifndef _FIBER_H
#define _FIBER_H

#include "debug.h"
#include "mutex.h"
#include "rts-config.h"
#include "types.h"

#include <stdint.h>

#define FIBER_STATS CILK_STATS

#if FIBER_STATS
#define WHEN_FIBER_STATS(ex) ex
#else
#define WHEN_FIBER_STATS(ex)
#endif

//===============================================================
// Struct defs used by fibers, fiber pools
//===============================================================

// Statistics on active fibers that were allocated from this pool,
struct fiber_pool_stats {
    int in_use;     // number of fibers allocated - freed from / into the pool
    int max_in_use; // high watermark for in_use
    int max_free;   // high watermark for number of free fibers in the pool
};

struct cilk_fiber_pool {
    cilk_mutex *lock;               // Mutual exclusion for pool operations
    int64_t stack_size;             // Size of stacks for fibers in this pool.
    struct cilk_fiber_pool *parent; // Parent pool.
                                    // If this pool is empty, get from parent
    // Describes inactive fibers stored in the pool.
    struct cilk_fiber **fibers; // Array of max_size fiber pointers
    int capacity;               // Limit on number of fibers in pool
    int size;                   // Number of fibers currently in the pool
    worker_id mutex_owner;
    WHEN_FIBER_STATS(struct fiber_pool_stats stats);
};

struct cilk_fiber {
    char *m_stack;           // lowest usable byte of stack
    char *m_stack_base;      // one byte above highest usable byte of stack
    __cilkrts_worker *owner; // worker using this fiber
};

//===============================================================
// Supported functions
//===============================================================

static inline void cilk_fiber_set_owner(struct cilk_fiber *fiber,
                                        __cilkrts_worker *owner) {
    fiber->owner = owner;
}

CHEETAH_INTERNAL
void sysdep_save_fp_ctrl_state(__cilkrts_stack_frame *sf);
CHEETAH_INTERNAL
char *sysdep_reset_stack_for_resume(struct cilk_fiber *fiber,
                                    __cilkrts_stack_frame *sf);
CHEETAH_INTERNAL_NORETURN
void sysdep_longjmp_to_sf(__cilkrts_stack_frame *sf);
CHEETAH_INTERNAL_NORETURN
void init_fiber_run(__cilkrts_worker *w, struct cilk_fiber *fiber,
                    __cilkrts_stack_frame *sf);

CHEETAH_INTERNAL void cilk_fiber_pool_global_init(global_state *g);
CHEETAH_INTERNAL void cilk_fiber_pool_global_terminate(global_state *g);
CHEETAH_INTERNAL void cilk_fiber_pool_global_destroy(global_state *g);
CHEETAH_INTERNAL void cilk_fiber_pool_per_worker_init(__cilkrts_worker *w);
CHEETAH_INTERNAL void cilk_fiber_pool_per_worker_terminate(__cilkrts_worker *w);
CHEETAH_INTERNAL void cilk_fiber_pool_per_worker_destroy(__cilkrts_worker *w);

// allocate / deallocate one fiber from / back to OS
CHEETAH_INTERNAL
struct cilk_fiber *cilk_fiber_allocate(__cilkrts_worker *w);
CHEETAH_INTERNAL
void cilk_fiber_deallocate(__cilkrts_worker *w, struct cilk_fiber *fiber);
// allocate / deallocate fiber from / back to OS for the invoke-main
CHEETAH_INTERNAL
struct cilk_fiber *cilk_main_fiber_allocate();
CHEETAH_INTERNAL
void cilk_main_fiber_deallocate(struct cilk_fiber *fiber);
// allocate / deallocate one fiber from / back to per-worker pool
CHEETAH_INTERNAL
struct cilk_fiber *cilk_fiber_allocate_from_pool(__cilkrts_worker *w);
CHEETAH_INTERNAL
void cilk_fiber_deallocate_to_pool(__cilkrts_worker *w,
                                   struct cilk_fiber *fiber);

#endif
