#include "debug.h"
#include "cilk-internal.h"
#include "cilk2c.h"
#include "fiber.h"
#include "global.h"
#include "rts-config.h"
#include "scheduler.h"
#include <unwind.h>

CHEETAH_INTERNAL struct cilkrts_callbacks cilkrts_callbacks = {
    0, 0, false, {nullptr}, {nullptr}};

// Test if the Cilk runtime has been initialized.  This method is intended to
// help initialization of libraries that depend on the OpenCilk runtime.
__attribute__((nothrow))
int __cilkrts_is_initialized(void) { return nullptr != default_cilkrts; }

__attribute__((nothrow))
int __cilkrts_running_on_workers(void) {
    return !__cilkrts_status.need_to_cilkify;
}

// These callback-registration methods can run before the runtime system has
// started.
//
// Init callbacks are called in order of registration.  Exit callbacks are
// called in reverse order of registration.

// Register a callback to run at Cilk-runtime initialization.  Returns 0 on
// successful registration, nonzero otherwise.
__attribute__((nothrow))
int __cilkrts_atinit(void (*callback)(void)) {
    if (cilkrts_callbacks.last_init >= MAX_CALLBACKS ||
        cilkrts_callbacks.after_init)
        return -1;

    cilkrts_callbacks.init[cilkrts_callbacks.last_init++] = callback;
    return 0;
}

// Register a callback to run at Cilk-runtime exit.  Returns 0 on successful
// registration, nonzero otherwise.
__attribute__((nothrow))
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
    __cilkrts_worker *w = get_worker_from_stack(sf);
    CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());

    closure_exception *exn_r = get_exception_reducer(w);
    char *exn = exn_r->exn;

    // zero exception storage, so we don't unintentionally try to
    // handle/propagate this exception again
    clear_exception_reducer(w, exn_r);
    sf->flags &= ~CILK_FRAME_EXCEPTION_PENDING;

    if (exn != nullptr) {
        _Unwind_RaiseException((_Unwind_Exception *)exn); // noreturn
        __builtin_unreachable();
    }

    return;
}

// Checks if there is an exception that needs to be propagated, and if so,
// resumes unwinding with that exception.
void __cilkrts_check_exception_resume(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());

    closure_exception *exn_r = get_exception_reducer(w);
    char *exn = exn_r->exn;

    // zero exception storage, so we don't unintentionally try to
    // handle/propagate this exception again
    clear_exception_reducer(w, exn_r);
    sf->flags &= ~CILK_FRAME_EXCEPTION_PENDING;

    if (exn != nullptr) {
        _Unwind_Resume((_Unwind_Exception *)exn); // noreturn
        __builtin_unreachable();
    }

    return;
}

// Called by generated exception-handling code, specifically, at the beginning
// of each landingpad in a spawning function.  Ensures that the stack pointer
// points at the fiber and call-stack frame containing sf before any catch
// handlers in that frame execute.
extern "C" void __cilkrts_cleanup_fiber(__cilkrts_stack_frame *sf,
                                        [[maybe_unused]] int32_t sel) noexcept {

    __cilkrts_worker *w = get_worker_from_stack(sf);
    CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());

    CILK_ASSERT(__cilkrts_synced(sf));

    closure_exception *exn_r = get_exception_reducer_or_null(w);
    cilk_fiber *throwing_fiber = nullptr;
    char *parent_rsp = nullptr;
    if (exn_r != nullptr) {
        throwing_fiber = exn_r->throwing_fiber;
        parent_rsp = exn_r->parent_rsp;

        exn_r->throwing_fiber = nullptr;
        clear_exception_reducer(w, exn_r);
    }

    // If parent_rsp is non-null, then the Cilk personality function executed
    // __cilkrts_sync(sf), which implies that sf is at the top of the deque.
    // Because we're executing a non-cleanup landingpad, execution is continuing
    // within this function frame, rather than unwinding further to a parent
    // frame, which would belong to a distinct closure.  Hence, if we reach this
    // point, set the stack pointer in sf to parent_rsp if parent_rsp is
    // non-null.

    if (nullptr == parent_rsp) {
        // If parent_rsp is null, we might have unwound past the point where the
        // Cilk personality function performed a sync.  Because we're executing
        // a non-cleanup landing pad, execution is continuing within this frame,
        // and we can safely free any saved throwing fiber.
        if (throwing_fiber) {
            cilk_fiber_deallocate_to_pool(w, throwing_fiber);
        }
        return;
    }

    SP(sf) = (void *)parent_rsp;

    // Since we're longjmping to another fiber, we don't need to save
    // throwing_fiber anymore.
    if (throwing_fiber) {
        cilk_fiber_deallocate_to_pool(w, throwing_fiber);
    }
    __builtin_longjmp(sf->ctx, 1); // Does not return
    return;
}

extern "C"
__attribute__((noreturn))
void __cilkrts_sync(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());

    CILK_ASSERT(CHECK_CILK_FRAME_MAGIC(w->g, sf));

    if (Cilk_sync(w, sf) == SYNC_READY) {
        // The Cilk_sync restores the original rsp stored in sf->ctx
        // if this frame is ready to sync.
        sysdep_longjmp_to_sf(sf);
    } else {
        longjmp_to_runtime(w);
    }
}

///////////////////////////////////////////////////////////////////////////
/// Methods for handling extensions

extern "C"
void __cilkrts_register_extension(void *extension) {
    __cilkrts_status.use_extension = true;
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    w->extension = extension;
}

extern "C"
void *__cilkrts_get_extension(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    return w->extension;
}
