#include <stdatomic.h>
#include <stdio.h>

#include "debug.h"

#include "cilk-internal.h"
#include "cilk2c.h"
#include "fiber.h"
#include "scheduler.h"

CHEETAH_INTERNAL int cilkg_nproc = 0;

// ================================================================
// This file contains the compiler ABI, which corresponds to
// conceptually what the compiler generates to implement Cilk code.
// They are included here in part as documentation, and in part
// allow one to write and run "hand-compiled" Cilk code.
// ================================================================

// inlined by the compiler
void __cilkrts_enter_frame(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_enter_frame %p", sf);

    sf->flags = CILK_FRAME_VERSION;
    sf->call_parent = w->current_stack_frame;
    sf->worker = w;
    w->current_stack_frame = sf;
    // WHEN_CILK_DEBUG(sf->magic = CILK_STACKFRAME_MAGIC);
}

// inlined by the compiler; this implementation is only used in invoke-main.c
void __cilkrts_enter_frame_fast(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_enter_frame_fast %p", sf);

    sf->flags = CILK_FRAME_VERSION;
    sf->call_parent = w->current_stack_frame;
    sf->worker = w;
    w->current_stack_frame = sf;
    // WHEN_CILK_DEBUG(sf->magic = CILK_STACKFRAME_MAGIC);
}

// inlined by the compiler; this implementation is only used in invoke-main.c
void __cilkrts_detach(__cilkrts_stack_frame *sf) {
    struct __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_detach %p", sf);

    CILK_ASSERT(w, GET_CILK_FRAME_VERSION(sf->flags) == __CILKRTS_ABI_VERSION);
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, w->current_stack_frame == sf);

    struct __cilkrts_stack_frame *parent = sf->call_parent;
    struct __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    CILK_ASSERT(w, (tail + 1) < w->ltq_limit);

    // store parent at *tail, and then increment tail
    *tail++ = parent;
    sf->flags |= CILK_FRAME_DETACHED;
    /* Release ordering ensures the two preceding stores are visible. */
    atomic_store_explicit(&w->tail, tail, memory_order_release);
}

// inlined by the compiler; this implementation is only used in invoke-main.c
void __cilkrts_save_fp_ctrl_state(__cilkrts_stack_frame *sf) {
    sysdep_save_fp_ctrl_state(sf);
}

void __cilkrts_sync(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_SYNC, w, "__cilkrts_sync syncing frame %p", sf);

    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, GET_CILK_FRAME_VERSION(sf->flags) == __CILKRTS_ABI_VERSION);
    CILK_ASSERT(w, sf == w->current_stack_frame);

    if (Cilk_sync(w, sf) == SYNC_READY) {
        cilkrts_alert(ALERT_SYNC, w, "__cilkrts_sync synced frame %p!", sf);
        // The Cilk_sync restores the original rsp stored in sf->ctx
        // if this frame is ready to sync.
        sysdep_longjmp_to_sf(sf);
    } else {
        cilkrts_alert(ALERT_SYNC, w, "__cilkrts_sync waiting to sync frame %p!",
                      sf);
        longjmp_to_runtime(w);
    }
}

// inlined by the compiler; this implementation is only used in invoke-main.c
void __cilkrts_pop_frame(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_pop_frame %p", sf);

    CILK_ASSERT(w, GET_CILK_FRAME_VERSION(sf->flags) == __CILKRTS_ABI_VERSION);
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    /* The inlined version in the Tapir compiler uses release
       semantics for the store to call_parent, but relaxed
       order may be acceptable for both.  A thief can't see
       these operations until the Dekker protocol with a
       memory barrier has run. */
    w->current_stack_frame = sf->call_parent;
    sf->call_parent = 0;
}

void __cilkrts_leave_frame(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_leave_frame %p", sf);

    CILK_ASSERT(w, GET_CILK_FRAME_VERSION(sf->flags) == __CILKRTS_ABI_VERSION);
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    if (sf->flags & CILK_FRAME_DETACHED) { // if this frame is detached
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
            Cilk_exception_handler();
            // If Cilk_exception_handler returns this thread won
            // the race and can return to the parent function.
        }
        // CILK_ASSERT(w, *(w->tail) == w->current_stack_frame);
    } else {
        // A detached frame would never need to call Cilk_set_return, which
        // performs the return protocol of a full frame back to its parent
        // when the full frame is called (not spawned).  A spawned full
        // frame returning is done via a different protocol, which is
        // triggered in Cilk_exception_handler.
        if (sf->flags & CILK_FRAME_STOLEN) { // if this frame has a full frame
            cilkrts_alert(ALERT_RETURN, w,
                          "__cilkrts_leave_frame parent is call_parent!");
            // leaving a full frame; need to get the full frame of its call
            // parent back onto the deque
            Cilk_set_return(w);
            CILK_ASSERT(w,
                        GET_CILK_FRAME_VERSION(w->current_stack_frame->flags) ==
                            __CILKRTS_ABI_VERSION);
        }
    }
}

int __cilkrts_get_nworkers(void) { return cilkg_nproc; }
