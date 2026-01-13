#ifndef _FIBER_H
#define _FIBER_H

#include "cilk-internal.h"
#include "debug.h"
#include "fiber-header.h"
#include "frame.h"
#include "mutex.h"
#include "rts-config.h"
#include "worker.h"

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
    size_t stack_size;       // Size of stacks for fibers in this pool.
    cilk_fiber_pool *parent; // Parent pool.
                             // If this pool is empty, get from parent
    // Describes inactive fibers stored in the pool.
    cilk_fiber **fibers;   // Array of max_size fiber pointers
    unsigned int capacity; // Limit on number of fibers in pool
    unsigned int size;     // Number of fibers currently in the pool
    fiber_pool_stats stats;

    alignas(CILK_CACHE_LINE) cilk_mutex lock;
};

//===============================================================
// Supported functions
//===============================================================

static inline __attribute__((always_inline, nothrow)) void
sysdep_save_fp_ctrl_state([[maybe_unused]] __cilkrts_stack_frame *sf) {
#ifdef CHEETAH_SAVE_MXCSR
#if 1
    __asm__("stmxcsr %0" : "=m"(MXCSR(sf)));
#else
    /* Disabled because LLVM's implementation is bad. */
    sf->mxcsr = __builtin_ia32_stmxcsr(); /* aka _mm_setcsr */
#endif
#else
    // sf intentionally unused
#endif
}

/*
 * Restore the floating point state that is stored in a stack frame at each
 * spawn.  This should be called each time a frame is resumed.  OpenCilk
 * only saves MXCSR.  The 80387 status word is obsolete.
 */
static inline __attribute__((always_inline, nothrow)) void
sysdep_restore_fp_state([[maybe_unused]] __cilkrts_stack_frame *sf) {
    /* TODO: Find a way to do this only when using floating point. */
#ifdef CHEETAH_SAVE_MXCSR
#if 1
    __asm__ volatile("ldmxcsr %0" : : "m"(MXCSR(sf)));
#else
    /* Disabled because LLVM's implementation is bad. */
    __builtin_ia32_ldmxcsr(sf->mxcsr); /* aka _mm_getcsr */
#endif
#else
    // sf intentionally unused
#endif

#ifdef __AVX__
    /* VZEROUPPER improves performance when mixing SSE and AVX code.
       VZEROALL would work as well here because vector registers are
       dead but takes about 10 cycles longer. */
    __builtin_ia32_vzeroupper();
#endif
}

static inline char *sysdep_reset_stack_for_resume(cilk_fiber *fiber,
                                                  __cilkrts_stack_frame *sf) {
    CILK_ASSERT(fiber);
    char *sp = fiber->get_stack_start();
    /* Debugging: make sure stack is accessible. */
    ((volatile char *)sp)[-1];
    SP(sf) = sp;

    return sp;
}

static inline __attribute__((noreturn)) void
sysdep_longjmp_to_sf(__cilkrts_stack_frame *sf) {
    cilkrts_alert(FIBER, "longjmp to sf, BP/SP/PC: %p/%p/%p", FP(sf), SP(sf),
                  PC(sf));

#if defined CHEETAH_SAVE_MXCSR
    // Restore the floating point state that was set in this frame at the
    // last spawn.
    sysdep_restore_fp_state(sf);
#endif
    __builtin_longjmp(sf->ctx, 1);
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
cilk_fiber *cilk_fiber_allocate(size_t stacksize);
CHEETAH_INTERNAL
void cilk_fiber_deallocate(cilk_fiber *fiber);
CHEETAH_INTERNAL
void cilk_fiber_deallocate_global(global_state *, cilk_fiber *fiber);
// allocate / deallocate one fiber from / back to per-worker pool
CHEETAH_INTERNAL
cilk_fiber *cilk_fiber_allocate_from_pool(__cilkrts_worker *w);
CHEETAH_INTERNAL
void cilk_fiber_deallocate_to_pool(__cilkrts_worker *w, cilk_fiber *fiber);

#if CILK_ENABLE_ASAN_HOOKS
void sanitizer_start_switch_fiber(cilk_fiber *fiber) __CILKRTS_NOTHROW;
void sanitizer_finish_switch_fiber(void) __CILKRTS_NOTHROW;
CHEETAH_INTERNAL void sanitizer_poison_fiber(cilk_fiber *fiber);
CHEETAH_INTERNAL void sanitizer_unpoison_fiber(cilk_fiber *fiber);
#else
static inline void
sanitizer_start_switch_fiber([[maybe_unused]] cilk_fiber *fiber) {}
static inline void sanitizer_finish_switch_fiber() {}
static inline void sanitizer_poison_fiber([[maybe_unused]] cilk_fiber *fiber) {}
static inline void
sanitizer_unpoison_fiber([[maybe_unused]] cilk_fiber *fiber) {}
#endif // CILK_ENABLE_ASAN_HOOKS
#endif
