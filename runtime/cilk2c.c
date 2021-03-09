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

CHEETAH_INTERNAL struct cilkrts_callbacks cilkrts_callbacks = {
    0, 0, false, {NULL}, {NULL}};

unsigned __cilkrts_get_nworkers(void) { return cilkg_nproc; }

// Internal method to get the Cilk worker ID.  Intended for debugging purposes.
//
// TODO: Figure out how we want to support worker-local storage.
unsigned __cilkrts_get_worker_number(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (w)
        return w->self;
    // Use the last exiting worker from default_cilkrts instead
    return default_cilkrts->exiting_worker;
}

// Test if the Cilk runtime has been initialized.  This method is intended to
// help initialization of libraries that depend on the OpenCilk runtime.
int __cilkrts_is_initialized(void) { return NULL != default_cilkrts; }

int __cilkrts_running_on_workers(void) {
    return NULL != __cilkrts_get_tls_worker();
}

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

// Called after a normal cilk_sync or a cilk_sync performed within the
// personality function.  Checks if there is an exception that needs to be
// propagated. This is called from the frame that will handle whatever exception
// was thrown.
void __cilkrts_check_exception_raise(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());

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

// Checks if there is an exception that needs to be propagated, and if so,
// resumes unwinding with that exception.
void __cilkrts_check_exception_resume(__cilkrts_stack_frame *sf) {

    __cilkrts_worker *w = sf->worker;
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());

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

    __cilkrts_worker *w = sf->worker;
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());

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

    if (t->saved_throwing_fiber) {
        cilk_fiber_deallocate_to_pool(w, t->saved_throwing_fiber);
        t->saved_throwing_fiber = NULL;
    }

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
