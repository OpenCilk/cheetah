// =============================================================================
// This file contains the compiler-runtime ABI.  This file is compiled to LLVM
// bitcode, which the compiler then includes and inlines when it compiles a Cilk
// program.
// =============================================================================

#include <stdatomic.h>
#include <stdio.h>
#include <unwind.h>

#include "cilk-internal.h"
#include "cilk2c.h"
#include "debug.h"
#include "fiber.h"
#include "global.h"
#include "init.h"
#include "readydeque.h"
#include "scheduler.h"

#ifdef ENABLE_CILKRTS_PEDIGREE
extern __cilkrts_pedigree cilkrts_root_pedigree_node;
extern uint64_t DPRNG_PRIME;
extern uint64_t* dprng_m_array;
extern uint64_t dprng_m_X;

uint64_t __cilkrts_dprng_swap_halves(uint64_t x);
uint64_t __cilkrts_dprng_mix(uint64_t x);
uint64_t __cilkrts_dprng_mix_mod_p(uint64_t x);
uint64_t __cilkrts_dprng_sum_mod_p(uint64_t a, uint64_t b);
void __cilkrts_init_dprng(void);

uint64_t __cilkrts_get_dprand(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    __cilkrts_bump_worker_rank();
    return __cilkrts_dprng_mix_mod_p(w->current_stack_frame->dprng_dotproduct);
}

#endif

// Begin a Cilkified region.  The routine runs on a Cilkifying thread to
// transfer the execution of this function to the workers in global_state g.
// This routine must be inlined for correctness.
static inline __attribute__((always_inline)) void
cilkify(global_state *g, __cilkrts_stack_frame *sf) {
#ifdef ENABLE_CILKRTS_PEDIGREE
    __cilkrts_init_dprng();
#endif

    // After inlining, the setjmp saves the processor state, including the frame
    // pointer, of the Cilk function.
    if (__builtin_setjmp(sf->ctx) == 0) {
        sysdep_save_fp_ctrl_state(sf);
        __cilkrts_internal_invoke_cilkified_root(g, sf);
    } else {
        sanitizer_finish_switch_fiber();
    }
}

// End a Cilkified region.  This routine runs on one worker in global_state g
// who finished executing the Cilkified region, in order to transfer control
// back to the original thread that began the Cilkified region.  This routine
// must be inlined for correctness.
static inline __attribute__((always_inline)) void
uncilkify(global_state *g, __cilkrts_stack_frame *sf) {
    // The setjmp will save the processor state at the end of the Cilkified
    // region.  The Cilkifying thread will longjmp to this point.
    if (__builtin_setjmp(sf->ctx) == 0) {
        sysdep_save_fp_ctrl_state(sf);
        // Finish this Cilkified region, and transfer control back to the
        // original thread that performed cilkify.
        __cilkrts_internal_exit_cilkified_root(g, sf);
    } else {
        sanitizer_finish_switch_fiber();
    }
}

#ifdef ENABLE_CILKRTS_PEDIGREE
__attribute__((always_inline)) __cilkrts_pedigree __cilkrts_get_pedigree(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (w == NULL) {
        return cilkrts_root_pedigree_node;
    } else {
        __cilkrts_pedigree ret_ped;
        ret_ped.parent = &(w->current_stack_frame->pedigree);
        ret_ped.rank = w->current_stack_frame->rank;
        return ret_ped;
    }
}

__attribute__((always_inline)) void __cilkrts_bump_worker_rank(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (w == NULL) {
        cilkrts_root_pedigree_node.rank++;
    } else {
        w->current_stack_frame->rank++;
    }
    w->current_stack_frame->dprng_dotproduct = __cilkrts_dprng_sum_mod_p(
        w->current_stack_frame->dprng_dotproduct,
        dprng_m_array[w->current_stack_frame->dprng_depth]);
}
#endif

// Enter a new Cilk function, i.e., a function that contains a cilk_spawn.  This
// function must be inlined for correctness.
__attribute__((always_inline)) void
__cilkrts_enter_frame(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    sf->flags = 0;
    if (NULL == w) {
        cilkify(default_cilkrts, sf);
        w = __cilkrts_get_tls_worker();
    }
    cilkrts_alert(CFRAME, w, "__cilkrts_enter_frame %p", (void *)sf);

    sf->magic = frame_magic;
    sf->call_parent = w->current_stack_frame;
    atomic_store_explicit(&sf->worker, w, memory_order_relaxed);
    w->current_stack_frame = sf;
    // WHEN_CILK_DEBUG(sf->magic = CILK_STACKFRAME_MAGIC);

#ifdef ENABLE_CILKRTS_PEDIGREE
    // Pedigree maintenance.
    if (sf->call_parent != NULL && !(sf->flags & CILK_FRAME_LAST)) {
        sf->pedigree.rank = sf->call_parent->rank++;
        sf->pedigree.parent = &(sf->call_parent->pedigree);
        sf->dprng_depth = sf->call_parent->dprng_depth + 1;
        sf->call_parent->dprng_dotproduct = __cilkrts_dprng_sum_mod_p(
            sf->call_parent->dprng_dotproduct,
            dprng_m_array[sf->call_parent->dprng_depth]);
        sf->dprng_dotproduct = sf->call_parent->dprng_dotproduct;
    } else {
        sf->pedigree.rank = 0;
        sf->pedigree.parent = NULL;
        sf->dprng_depth = 0;
        sf->dprng_dotproduct = dprng_m_X;
    }
    sf->rank = 0;
#endif
}

// Enter a spawn helper, i.e., a fucntion containing code that was cilk_spawn'd.
// This function initializes worker and stack_frame structures.  Because this
// routine will always be executed by a Cilk worker, it is optimized compared to
// its counterpart, __cilkrts_enter_frame.
__attribute__((always_inline)) void
__cilkrts_enter_frame_helper(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    cilkrts_alert(CFRAME, w, "__cilkrts_enter_frame_helper %p", (void *)sf);

    sf->flags = 0;
    sf->magic = frame_magic;
    sf->call_parent = w->current_stack_frame;
    atomic_store_explicit(&sf->worker, w, memory_order_relaxed);
    w->current_stack_frame = sf;

#ifdef ENABLE_CILKRTS_PEDIGREE
    // Pedigree maintenance.
    if (sf->call_parent != NULL && !(sf->flags & CILK_FRAME_LAST)) {
        sf->pedigree.rank = sf->call_parent->rank++;
        sf->pedigree.parent = &(sf->call_parent->pedigree);
        sf->dprng_depth = sf->call_parent->dprng_depth + 1;
        sf->call_parent->dprng_dotproduct = __cilkrts_dprng_sum_mod_p(
            sf->call_parent->dprng_dotproduct,
            dprng_m_array[sf->call_parent->dprng_depth]);
        sf->dprng_dotproduct = sf->call_parent->dprng_dotproduct;
    } else {
        sf->pedigree.rank = 0;
        sf->pedigree.parent = NULL;
        sf->dprng_depth = 0;
        sf->dprng_dotproduct = dprng_m_X;
    }
    sf->rank = 0;
#endif
}

__attribute__((always_inline)) int
__cilk_prepare_spawn(__cilkrts_stack_frame *sf) {
    sysdep_save_fp_ctrl_state(sf);
    int res = __builtin_setjmp(sf->ctx);
    if (res != 0) {
        sanitizer_finish_switch_fiber();
    }
    return res;
}

static inline __cilkrts_worker *get_tls_worker(__cilkrts_stack_frame *sf) {
    // In principle, we should be able to get the worker efficiently by calling
    // __cilkrts_get_tls_worker().  But code-generation on many systems assumes
    // that the thread on which a function runs never changes.  As a result, it
    // may cache the address returned by __cilkrts_get_tls_worker() during
    // enter_frame and load the cached value in later, even though the actual
    // result of __cilkrts_get_tls_worker() may change between those two points.
    // To avoid this buggy behavior, we therefore get the worker from sf.
    //
    // TODO: Fix code-generation of TLS lookups on these systems.
    return atomic_load_explicit(&sf->worker, memory_order_relaxed);
}

// Detach the given Cilk stack frame, allowing other Cilk workers to steal the
// parent frame.
__attribute__((always_inline)) void
__cilkrts_detach(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_tls_worker(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_detach %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, w->current_stack_frame == sf);

    struct __cilkrts_stack_frame *parent = sf->call_parent;
    sf->flags |= CILK_FRAME_DETACHED;
    struct __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    CILK_ASSERT(w, (tail + 1) < w->ltq_limit);

    // store parent at *tail, and then increment tail
    *tail++ = parent;
    /* Release ordering ensures the two preceding stores are visible. */
    atomic_store_explicit(&w->tail, tail, memory_order_release);
}

__attribute__((always_inline)) void __cilk_sync(__cilkrts_stack_frame *sf) {
    if (sf->flags & CILK_FRAME_UNSYNCHED) {
        if (__builtin_setjmp(sf->ctx) == 0) {
            sysdep_save_fp_ctrl_state(sf);
            __cilkrts_sync(sf);
        } else {
            sanitizer_finish_switch_fiber();
            if (sf->flags & CILK_FRAME_EXCEPTION_PENDING) {
                __cilkrts_check_exception_raise(sf);
            }
        }
    }
}

__attribute__((always_inline)) void
__cilk_sync_nothrow(__cilkrts_stack_frame *sf) {
    if (sf->flags & CILK_FRAME_UNSYNCHED) {
        if (__builtin_setjmp(sf->ctx) == 0) {
            sysdep_save_fp_ctrl_state(sf);
            __cilkrts_sync(sf);
        } else {
            sanitizer_finish_switch_fiber();
        }
    }
}

__attribute__((always_inline)) void
__cilkrts_leave_frame(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_tls_worker(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_leave_frame %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    w->current_stack_frame = sf->call_parent;
    sf->call_parent = NULL;

    // Check if sf is the final stack frame, and if so, terminate the Cilkified
    // region.
    uint32_t flags = sf->flags;
    if (flags & CILK_FRAME_LAST) {
        uncilkify(w->g, sf);
        flags = sf->flags;
    }

    if (flags == 0) {
        return;
    }

    CILK_ASSERT(w, !(flags & CILK_FRAME_DETACHED));

    // A detached frame would never need to call Cilk_set_return, which performs
    // the return protocol of a full frame back to its parent when the full
    // frame is called (not spawned).  A spawned full frame returning is done
    // via a different protocol, which is triggered in Cilk_exception_handler.
    if (flags & CILK_FRAME_STOLEN) { // if this frame has a full frame
        cilkrts_alert(RETURN, w,
                      "__cilkrts_leave_frame parent is call_parent!");
        // leaving a full frame; need to get the full frame of its call
        // parent back onto the deque
        Cilk_set_return(w);
        CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    }
}

__attribute__((always_inline)) void
__cilkrts_leave_frame_helper(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_tls_worker(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_leave_frame_helper %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    w->current_stack_frame = sf->call_parent;
    sf->call_parent = NULL;

    CILK_ASSERT(w, sf->flags & CILK_FRAME_DETACHED);

    __cilkrts_stack_frame **tail =
            atomic_load_explicit(&w->tail, memory_order_relaxed);
    --tail;
    /* The store of tail must precede the load of exc in global order.  See
       comment in do_dekker_on. */
    atomic_store_explicit(&w->tail, tail, memory_order_seq_cst);
    __cilkrts_stack_frame **exc =
            atomic_load_explicit(&w->exc, memory_order_seq_cst);
    /* Currently no other modifications of flags are atomic so this one isn't
       either.  If the thief wins it may run in parallel with the clear of
       DETACHED.  Does it modify flags too? */
    sf->flags &= ~CILK_FRAME_DETACHED;
    if (__builtin_expect(exc > tail, 0)) {
        Cilk_exception_handler(NULL);
        // If Cilk_exception_handler returns this thread won the race and can
        // return to the parent function.
    }
    // CILK_ASSERT(w, *(w->tail) == w->current_stack_frame);
}

__attribute__((always_inline)) void
__cilk_parent_epilogue(__cilkrts_stack_frame *sf) {
    __cilkrts_leave_frame(sf);
}

__attribute__((always_inline)) void
__cilk_helper_epilogue(__cilkrts_stack_frame *sf) {
    __cilkrts_leave_frame_helper(sf);
}

__attribute__((always_inline))
void __cilkrts_enter_landingpad(__cilkrts_stack_frame *sf, int32_t sel) {
    // Don't do anything special during cleanups.
    if (sel == 0)
        return;

    if (0 == __builtin_setjmp(sf->ctx))
        __cilkrts_cleanup_fiber(sf, sel);
}

__attribute__((always_inline))
void __cilkrts_pause_frame(__cilkrts_stack_frame *sf, char *exn) {
    __cilkrts_worker *w = get_tls_worker(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_pause_frame %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    w->current_stack_frame = sf->call_parent;
    sf->call_parent = NULL;

    // A __cilkrts_pause_frame may be reached before the spawn-helper frame has
    // detached.  In that case, THE is not required.
    if (sf->flags & CILK_FRAME_DETACHED) {
        __cilkrts_stack_frame **tail =
            atomic_load_explicit(&w->tail, memory_order_relaxed);
        --tail;
        /* The store of tail must precede the load of exc in global order.
           See comment in do_dekker_on. */
        atomic_store_explicit(&w->tail, tail, memory_order_seq_cst);
        __cilkrts_stack_frame **exc =
            atomic_load_explicit(&w->exc, memory_order_seq_cst);
        /* Currently no other modifications of flags are atomic so this
           one isn't either.  If the thief wins it may run in parallel
           with the clear of DETACHED.  Does it modify flags too? */
        sf->flags &= ~CILK_FRAME_DETACHED;
        if (__builtin_expect(exc > tail, 0)) {
            Cilk_exception_handler(exn);
            // If Cilk_exception_handler returns this thread won
            // the race and can return to the parent function.
        }
        // CILK_ASSERT(w, *(w->tail) == w->current_stack_frame);
    }
}

__attribute__((always_inline)) void
__cilk_helper_epilogue_exn(__cilkrts_stack_frame *sf, char *exn) {
    __cilkrts_pause_frame(sf, exn);
}

/// Computes a grainsize for a cilk_for loop, using the following equation:
///
///     grainsize = min(2048, ceil(n / (8 * nworkers)))
#define __cilkrts_grainsize_fn_impl(NAME, INT_T)                               \
    __attribute__((always_inline)) INT_T NAME(INT_T n) {                       \
        INT_T small_loop_grainsize = n / (8 * cilkg_nproc);                    \
        if (small_loop_grainsize <= 1)                                         \
            return 1;                                                          \
        INT_T large_loop_grainsize = 2048;                                     \
        return large_loop_grainsize < small_loop_grainsize                     \
                   ? large_loop_grainsize                                      \
                   : small_loop_grainsize;                                     \
    }
#define __cilkrts_grainsize_fn(SZ)                                             \
    __cilkrts_grainsize_fn_impl(__cilkrts_cilk_for_grainsize_##SZ, uint##SZ##_t)

__attribute__((always_inline)) uint8_t
__cilkrts_cilk_for_grainsize_8(uint8_t n) {
    uint8_t small_loop_grainsize = n / (8 * cilkg_nproc);
    if (small_loop_grainsize <= 1)
        return 1;
    return small_loop_grainsize;
}

__cilkrts_grainsize_fn(16) __cilkrts_grainsize_fn(32) __cilkrts_grainsize_fn(64)
