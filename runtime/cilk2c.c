#include <stdatomic.h>
#include <stdio.h>
#include <unwind.h>

#include "debug.h"

#include "cilk-internal.h"
#include "cilk2c.h"
#include "fiber.h"
#include "global.h"
#include "readydeque.h"
#include "scheduler.h"

extern void _Unwind_Resume(struct _Unwind_Exception *);
extern _Unwind_Reason_Code _Unwind_RaiseException(struct _Unwind_Exception *);

CHEETAH_INTERNAL unsigned cilkg_nproc = 0;

CHEETAH_INTERNAL struct cilkrts_callbacks cilkrts_callbacks = {
    0, 0, false, {NULL}, {NULL}};

// These callback-registration methods can run before the runtime system has
// started.
//
// Init callbacks are called in order of registration.  Exit callbacks are
// called in reverse order of registration.

// Register a callback to run at Cilk-runtime initialization.  Returns 0 on
// successful registration, nonzero otherwise.
int __cilkrts_atinit(void (*callback)(void)) {
    if (cilkrts_callbacks.last_init >= MAX_CALLBACKS ||
        cilkrts_callbacks.after_init)
        return -1;

    cilkrts_callbacks.init[cilkrts_callbacks.last_init++] = callback;
    return 0;
}

// Register a callback to run at Cilk-runtime exit.  Returns 0 on successful
// registration, nonzero otherwise.
int __cilkrts_atexit(void (*callback)(void)) {
    if (cilkrts_callbacks.last_exit >= MAX_CALLBACKS)
        return -1;

    cilkrts_callbacks.exit[cilkrts_callbacks.last_exit++] = callback;
    return 0;
}

// Internal method to get the Cilk worker ID.  Intended for debugging purposes.
//
// TODO: Figure out how we want to support worker-local storage.
unsigned __cilkrts_get_worker_number(void) {
    return __cilkrts_get_tls_worker()->self;
}

#ifdef __linux__ /* This feature requires the GNU linker */
CHEETAH_INTERNAL
const char get_workerwarn_msg[]
    __attribute__((section(".gnu.warning.__cilkrts_get_worker_number"))) =
        "__cilkrts_get_worker_number is deprecated";
#endif

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

    sf->flags = 0;
    sf->magic = w->g->frame_magic;
    sf->call_parent = w->current_stack_frame;
    sf->worker = w;
    w->current_stack_frame = sf;
    // WHEN_CILK_DEBUG(sf->magic = CILK_STACKFRAME_MAGIC);
}

// inlined by the compiler; this implementation is only used in invoke-main.c
void __cilkrts_enter_frame_fast(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_enter_frame_fast %p", sf);

    sf->flags = 0;
    sf->magic = w->g->frame_magic;
    sf->call_parent = w->current_stack_frame;
    sf->worker = w;
    w->current_stack_frame = sf;
}

// inlined by the compiler; this implementation is only used in invoke-main.c
void __cilkrts_detach(__cilkrts_stack_frame *sf) {
    struct __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_detach %p", sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
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

// Called after a normal cilk_sync (i.e. not the cilk_sync called in the
// personality function.) Checks if there is an exception that needs to be
// propagated. This is called from the frame that will handle whatever exception
// was thrown.
void __cilkrts_check_exception_raise(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;
    deque_lock_self(w);
    Closure *t = deque_peek_bottom(w, w->self);
    Closure_lock(w, t);
    char *exn = t->user_exn.exn;

    // zero exception storage, so we don't unintentionally try to
    // handle/propagate this exception again
    clear_closure_exception(&(t->user_exn));
    sf->flags &= ~CILK_FRAME_EXCEPTION_PENDING;

    Closure_unlock(w, t);
    deque_unlock_self(w);
    if (exn != NULL) {
        _Unwind_RaiseException((struct _Unwind_Exception *)exn); // noreturn
    }

    return;
}

// Called after a cilk_sync in the personality function.  Checks if
// there is an exception that needs to be propagated, and if so,
// resumes unwinding with that exception.
void __cilkrts_check_exception_resume(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;
    deque_lock_self(w);
    Closure *t = deque_peek_bottom(w, w->self);
    Closure_lock(w, t);
    char *exn = t->user_exn.exn;

    // zero exception storage, so we don't unintentionally try to
    // handle/propagate this exception again
    clear_closure_exception(&(t->user_exn));
    sf->flags &= ~CILK_FRAME_EXCEPTION_PENDING;

    Closure_unlock(w, t);
    deque_unlock_self(w);
    if (exn != NULL) {
        _Unwind_Resume((struct _Unwind_Exception *)exn); // noreturn
    }

    return;
}

// Called by generated exception-handling code, specifically, at the beginning
// of each landingpad in a spawning function.  Ensures that the stack pointer
// points at the fiber and call-stack frame containing sf before any catch
// handlers in that frame execute.
void __cilkrts_cleanup_fiber(__cilkrts_stack_frame *sf, int32_t sel) {

    if (sel == 0)
        // Don't do anything during cleanups.
        return;

    __cilkrts_worker *w = sf->worker;
    deque_lock_self(w);
    Closure *t = deque_peek_bottom(w, w->self);

    // If t->parent_rsp is non-null, then the Cilk personality function executed
    // __cilkrts_sync(sf), which implies that sf is at the top of the deque.
    // Because we're executing a non-cleanup landingpad, execution is continuing
    // within this function frame, rather than unwinding further to a parent
    // frame, which would belong to a distinct closure.  Hence, if we reach this
    // point, set the stack pointer in sf to t->parent_rsp if t->parent_rsp is
    // non-null.

    if (NULL == t->parent_rsp) {
        deque_unlock_self(w);
        return;
    }

    SP(sf) = (void *)t->parent_rsp;
    t->parent_rsp = NULL;

    deque_unlock_self(w);
    __builtin_longjmp(sf->ctx, 1); // Does not return
    return;
}

void __cilkrts_sync(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;

    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf == w->current_stack_frame);

    if (Cilk_sync(w, sf) == SYNC_READY) {
        // The Cilk_sync restores the original rsp stored in sf->ctx
        // if this frame is ready to sync.
        sysdep_longjmp_to_sf(sf);
    } else {
        longjmp_to_runtime(w);
    }
}

// inlined by the compiler; this implementation is only used in invoke-main.c
void __cilkrts_pop_frame(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_pop_frame %p", sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    /* The inlined version in the Tapir compiler uses release
       semantics for the store to call_parent, but relaxed
       order may be acceptable for both.  A thief can't see
       these operations until the Dekker protocol with a
       memory barrier has run. */
    w->current_stack_frame = sf->call_parent;
    sf->call_parent = 0;
}

void __cilkrts_pause_frame(__cilkrts_stack_frame *sf, char *exn) {

    __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_pause_frame %p", sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());

    CILK_ASSERT(w, sf->flags & CILK_FRAME_DETACHED);
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

void __cilkrts_leave_frame(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;
    cilkrts_alert(ALERT_CFRAME, w, "__cilkrts_leave_frame %p", sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
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
            Cilk_exception_handler(NULL);
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
            CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
        }
    }
}

unsigned __cilkrts_get_nworkers(void) { return cilkg_nproc; }
