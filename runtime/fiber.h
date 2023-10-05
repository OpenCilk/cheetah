#ifndef _FIBER_H
#define _FIBER_H

#include "cilk-internal.h"
#include "debug.h"
#include "fiber-header.h"
#include "frame.h"
#include "mutex.h"
#include "rts-config.h"
#include "types.h"

#include <stdint.h>

//===============================================================
// Struct defs used by fibers, fiber pools
//===============================================================

// Statistics on active fibers that were allocated from this pool,
struct fiber_pool_stats {
    int in_use;     // number of fibers allocated - freed from / into the pool
    int max_in_use; // high watermark for in_use
    unsigned max_free; // high watermark for number of free fibers in the pool
};

struct cilk_fiber_pool {
    worker_id mutex_owner;
    int shared;
    size_t stack_size;              // Size of stacks for fibers in this pool.
    struct cilk_fiber_pool *parent; // Parent pool.
                                    // If this pool is empty, get from parent
    // Describes inactive fibers stored in the pool.
    struct cilk_fiber **fibers; // Array of max_size fiber pointers
    unsigned int capacity;      // Limit on number of fibers in pool
    unsigned int size;          // Number of fibers currently in the pool
    struct fiber_pool_stats stats;

    cilk_mutex lock __attribute__((aligned(CILK_CACHE_LINE)));
};

//===============================================================
// Supported functions
//===============================================================

static inline __attribute__((always_inline)) void
sysdep_save_fp_ctrl_state(__cilkrts_stack_frame *sf) {
#ifdef CHEETAH_SAVE_MXCSR
#if 1
    __asm__("stmxcsr %0" : "=m"(MXCSR(sf)));
#else
    /* Disabled because LLVM's implementation is bad. */
    sf->mxcsr = __builtin_ia32_stmxcsr(); /* aka _mm_setcsr */
#endif
#endif
}

/*
 * Restore the floating point state that is stored in a stack frame at each
 * spawn.  This should be called each time a frame is resumed.  OpenCilk
 * only saves MXCSR.  The 80387 status word is obsolete.
 */
static inline __attribute__((always_inline)) void
sysdep_restore_fp_state(__cilkrts_stack_frame *sf) {
    /* TODO: Find a way to do this only when using floating point. */
#ifdef CHEETAH_SAVE_MXCSR
#if 1
    __asm__ volatile("ldmxcsr %0" : : "m"(MXCSR(sf)));
#else
    /* Disabled because LLVM's implementation is bad. */
    __builtin_ia32_ldmxcsr(sf->mxcsr); /* aka _mm_getcsr */
#endif
#endif

#ifdef __AVX__
    /* VZEROUPPER improves performance when mixing SSE and AVX code.
       VZEROALL would work as well here because vector registers are
       dead but takes about 10 cycles longer. */
    __builtin_ia32_vzeroupper();
#endif
}

static inline char *sysdep_get_fiber_start(struct cilk_fiber *fiber) {
    return fiber->alloc_low;
}

static inline char *sysdep_get_fiber_end(struct cilk_fiber *fiber) {
    return (char *)(fiber + 1);
}

static inline char *sysdep_get_stack_start(struct cilk_fiber *fiber) {
    /* The OpenCilk compiler should ensure that sufficient space is
       allocated for outgoing arguments of any function, so we don't need any
       particular alignment here.  We use a positive alignment here for the
       subsequent debugging step that checks the stack is accessible. */

    return (char *)fiber;
}

static inline char *sysdep_reset_stack_for_resume(struct cilk_fiber *fiber,
                                                  __cilkrts_stack_frame *sf) {
    CILK_ASSERT_G(fiber);
    char *sp = sysdep_get_stack_start(fiber);
    /* Debugging: make sure stack is accessible. */
    ((volatile char *)sp)[-1];
    SP(sf) = sp;

    return sp;
}

static inline __attribute__((noreturn))
void sysdep_longjmp_to_sf(__cilkrts_stack_frame *sf) {
    cilkrts_alert(FIBER, get_worker_from_stack(sf),
                  "longjmp to sf, BP/SP/PC: %p/%p/%p", FP(sf), SP(sf), PC(sf));

#if defined CHEETAH_SAVE_MXCSR
    // Restore the floating point state that was set in this frame at the
    // last spawn.
    sysdep_restore_fp_state(sf);
#endif
    __builtin_longjmp(sf->ctx, 1);
}

static inline void init_fiber_header(struct cilk_fiber *fh) {
    fh->worker = INVALID_WORKER;
    fh->current_stack_frame = NULL;
    fh->fake_stack_save = NULL;
}

static inline void deinit_fiber_header(struct cilk_fiber *fh) {
    fh->worker = INVALID_WORKER;
    fh->current_stack_frame = NULL;
    fh->fake_stack_save = NULL;
}

CHEETAH_INTERNAL void cilk_fiber_pool_global_init(global_state *g);
CHEETAH_INTERNAL void cilk_fiber_pool_global_terminate(global_state *g);
CHEETAH_INTERNAL void cilk_fiber_pool_global_destroy(global_state *g);
CHEETAH_INTERNAL void cilk_fiber_pool_per_worker_zero_init(__cilkrts_worker *w);
CHEETAH_INTERNAL void cilk_fiber_pool_per_worker_init(__cilkrts_worker *w);
CHEETAH_INTERNAL void cilk_fiber_pool_per_worker_terminate(__cilkrts_worker *w);
CHEETAH_INTERNAL void cilk_fiber_pool_per_worker_destroy(__cilkrts_worker *w);

// allocate / deallocate one fiber from / back to OS
CHEETAH_INTERNAL
struct cilk_fiber *cilk_fiber_allocate(__cilkrts_worker *w, size_t stacksize);
CHEETAH_INTERNAL
void cilk_fiber_deallocate(__cilkrts_worker *w, struct cilk_fiber *fiber);
CHEETAH_INTERNAL
void cilk_fiber_deallocate_global(global_state *, struct cilk_fiber *fiber);
// allocate / deallocate one fiber from / back to per-worker pool
CHEETAH_INTERNAL
struct cilk_fiber *cilk_fiber_allocate_from_pool(__cilkrts_worker *w);
CHEETAH_INTERNAL
void cilk_fiber_deallocate_to_pool(__cilkrts_worker *w,
                                   struct cilk_fiber *fiber);

CHEETAH_INTERNAL int in_fiber(struct cilk_fiber *, void *);

#if CILK_ENABLE_ASAN_HOOKS
void sanitizer_start_switch_fiber(struct cilk_fiber *fiber);
void sanitizer_finish_switch_fiber();
CHEETAH_INTERNAL void sanitizer_poison_fiber(struct cilk_fiber *fiber);
CHEETAH_INTERNAL void sanitizer_unpoison_fiber(struct cilk_fiber *fiber);
#else
static inline void sanitizer_start_switch_fiber(struct cilk_fiber *fiber) {}
static inline void sanitizer_finish_switch_fiber() {}
static inline void sanitizer_poison_fiber(struct cilk_fiber *fiber) {}
static inline void sanitizer_unpoison_fiber(struct cilk_fiber *fiber) {}
#endif // CILK_ENABLE_ASAN_HOOKS
#endif
