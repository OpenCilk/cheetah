// ================================================================
// This file contains the compiler ABI, which corresponds to
// conceptually what the compiler generates to implement Cilk code.
// They are included here in part as documentation, and in part
// allow one to write and run "hand-compiled" Cilk code.
// ================================================================

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
    // After inlining, orig_rsp will receive the stack pointer in the stack
    // frame of the Cilk function instantiation on the Cilkifying thread.
    void *orig_rsp = NULL;
    ASM_GET_SP(orig_rsp);

#ifdef ENABLE_CILKRTS_PEDIGREE
    __cilkrts_init_dprng();
#endif

    // After inlining, the setjmp saves the processor state, including the frame
    // pointer, of the Cilk function.
    if (__builtin_setjmp(sf->ctx) == 0) {
        sysdep_save_fp_ctrl_state(sf);
        invoke_cilkified_root(g, sf);

        wait_until_cilk_done(g);

        // At this point, some Cilk worker must have completed the Cilkified
        // region and executed uncilkify at the end of the Cilk function.  The
        // longjmp will therefore jump to the end of the Cilk function.  We need
        // only restore the stack pointer to its original value on the
        // Cilkifying thread's stack.
        SP(sf) = orig_rsp;
        sysdep_restore_fp_state(sf);
        __builtin_longjmp(sf->ctx, 1);
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
        exit_cilkified_root(g, sf);
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
__cilkrts_enter_frame_fast(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    cilkrts_alert(CFRAME, w, "__cilkrts_enter_frame_fast %p", (void *)sf);

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

// Detach the given Cilk stack frame, allowing other Cilk workers to steal the
// parent frame.
__attribute__((always_inline))
void __cilkrts_detach(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w =
        atomic_load_explicit(&sf->worker, memory_order_relaxed);
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

// inlined by the compiler; this implementation is only used in invoke-main.c
__attribute__((always_inline))
void __cilkrts_save_fp_ctrl_state(__cilkrts_stack_frame *sf) {
    sysdep_save_fp_ctrl_state(sf);
}

// Pop this Cilk stack frame off of the bottom of the linked list of
// __cilkrts_stack_frames, and if popping the last Cilk stack frame, call
// uncilkify to terminate the Cilkified region.  This function must be inlined
// for correctness.
__attribute__((always_inline))
void __cilkrts_pop_frame(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w =
        atomic_load_explicit(&sf->worker, memory_order_relaxed);
    cilkrts_alert(CFRAME, w, "__cilkrts_pop_frame %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    /* The inlined version in the Tapir compiler uses release
       semantics for the store to call_parent, but relaxed
       order may be acceptable for both.  A thief can't see
       these operations until the Dekker protocol with a
       memory barrier has run. */
    w->current_stack_frame = sf->call_parent;
    sf->call_parent = NULL;
    // Check if sf is the final stack frame, and if so, terminate the Cilkified
    // region.
    if (sf->flags & CILK_FRAME_LAST) {
        uncilkify(w->g, sf);
    }
}
