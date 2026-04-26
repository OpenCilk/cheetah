#include "busyclosure.h"
#include "cilk-internal.h"
#include "cilk2c.h"
#include "cilk2c_inlined.h"
#include "closure.h"
#include "debug.h"
#include "fiber.h"
#include "frame.h"
#include "init.h"
#include "local-reducer-api.h"
#include <cilk/cilk_api.h>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unwind.h>

using cilk::reducer_base;

static struct closure_exception exception_reducer;

typedef _Unwind_Reason_Code (*__personality_routine)(
    int version, _Unwind_Action actions, uint64_t exception_class,
    _Unwind_Exception *exception_object, _Unwind_Context *context);

extern "C"
_Unwind_Reason_Code __gcc_personality_v0(int version, _Unwind_Action actions,
                                         uint64_t exception_class,
                                         struct _Unwind_Exception *ue_header,
                                         struct _Unwind_Context *context);
extern "C"
_Unwind_Reason_Code __gxx_personality_v0(int version, _Unwind_Action actions,
                                         uint64_t exception_class,
                                         struct _Unwind_Exception *ue_header,
                                         struct _Unwind_Context *context);


static char *get_cfa(_Unwind_Context *context) {
    /* _Unwind_GetCFA is originally a gcc extension.  FreeBSD has its
       own library without that extension. */
#if defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
    return (char *)_Unwind_GetCFA(context);
#else
    /* See *RegisterInfo.td in LLVM source */
#ifdef __i386__
    int sp_regno = 5; /* unclear if 5 or 6 is right here */
#elif defined __x86_64__
    int sp_regno = 7;
#elif defined __aarch64__
    int sp_regno = 31;
#elif defined __arm__
    int sp_regno = 13;
#else
    /* Probably 14 for SPARC, 2 for RISCV, and 1 for PPC. */
#error "no CFA"
#endif
    return (char *)_Unwind_GetGR(context, sp_regno);
#endif
}

bool exception_reducer_is_empty() noexcept {
    return exception_reducer.exn == nullptr;
}

// Identity method for the exception reducer.
reducer_base *closure_exception::identity(void *v) {
    return new (v) closure_exception;
}

// Reduce method for the exception reducer.
void closure_exception::reduce(reducer_base *l, reducer_base *r) {
    closure_exception *lex = static_cast<closure_exception *>(l);
    closure_exception *rex = static_cast<closure_exception *>(r);
    if (lex->exn == nullptr) {
        lex->exn = rex->exn;
        rex->exn = nullptr;
    }
    if (rex->exn != nullptr) {
        _Unwind_DeleteException((_Unwind_Exception *)(rex->exn));
        rex->exn = nullptr;
    }
    // Use right-holder logic for reraise_cfa, parent_rsp, and throwing_fiber.
    lex->reraise_cfa = rex->reraise_cfa;
    lex->parent_rsp = rex->parent_rsp;
    if (lex->throwing_fiber)
        cilk_fiber_deallocate_to_pool(__cilkrts_get_tls_worker(),
                                      lex->throwing_fiber);
    lex->throwing_fiber = rex->throwing_fiber;
}

// Get the current view of the exception-reducer state, creating a new view if
// none exists.
closure_exception *get_exception_reducer(__cilkrts_worker *w) noexcept {
    return static_cast<closure_exception *>(
        internal_reducer_lookup(w, &exception_reducer));
}

// Try to get the current view of the exception-reducer state, but return NULL
// if no view exists.
closure_exception *get_exception_reducer_or_null(__cilkrts_worker *w) noexcept {
    void *key = (void *)(&exception_reducer);
    hyper_table *table = get_local_hyper_table_or_null(w);
    if (nullptr == table)
        return nullptr;

    bucket *b = find_hyperobject(table, (uintptr_t)key);
    if (b) {
        CILK_ASSERT_POINTER_EQUAL(key, (void *)b->key);
        // Return the existing view.
        reducer_base *base = std::get<reducer_base *>(b->data.extra);
        return static_cast<closure_exception *>(base);
    }
    // No view was found.  Don't create a new reducer view; just return NULL.
    return nullptr;
}

// Destroy the current view of the exception-reducer state.
void clear_exception_reducer(__cilkrts_worker *w,
                             closure_exception *exn_r) noexcept {
    CILK_ASSERT_NULL(exn_r->throwing_fiber);
    free(exn_r);
    internal_reducer_remove(w, &exception_reducer);
}

// Perform a cilk_sync from within the personality function.  This method saves
// exception-handling state into a new view of the exception reducer and then
// calls __cilkrts_sync.
//
// This function is marked __attribute__((noinline)) to prevent the values in
// local variables in the caller from being disrupted by the setjmp.
__attribute__((noinline)) static void
sync_in_personality(__cilkrts_worker *w, __cilkrts_stack_frame *sf,
                    _Unwind_Exception *ue_header) {
    worker_id self = w->self;
    BusyClosure *busy = w->g->busy;
    // save floating point state
    sysdep_save_fp_ctrl_state(sf);

    if (__builtin_setjmp(sf->ctx) == 0) {
        // set closure_exception
        closure_exception *exn_r = get_exception_reducer(w);
        exn_r->exn = (char *)ue_header;

        BusyClosure::lock_self(busy, self);
        Closure *t = BusyClosure::peek(busy, self, self);
        t->lock(self);

        // ensure that we return here after a cilk_sync.
        exn_r->parent_rsp = t->orig_rsp;
        t->orig_rsp = (char *)SP(sf);

        t->unlock(self);
        BusyClosure::unlock_self(busy, self);

        // save the current fiber for further stack unwinding.
        if (exn_r->throwing_fiber == nullptr) {
            exn_r->throwing_fiber = t->fiber;
            t->fiber = nullptr;
        }

        // For now, use this flag to indicate that we are setjmping from the
        // personality function. This will "disable" some asserts in
        // scheduler.cpp that we generally want to keep, but are broken in this
        // particular case.
        sf->flags |= CILK_FRAME_THROWING;
        __cilkrts_sync(sf);
    } else {
        sanitizer_finish_switch_fiber();
        __cilkrts_do_reductions(sf);
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

// Custom routine to resume handling an exception after leaving a cilkified
// region.
static void resume_from_last_frame(__cilkrts_worker *w,
                                   __cilkrts_stack_frame *sf,
                                   _Unwind_Exception *ue_header) {
    cilkrts_alert(CFRAME, "resume_from_last_frame %p", (void *)sf);
    CILK_ASSERT(CHECK_CILK_FRAME_MAGIC(w->g, sf));
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    sf->call_parent = nullptr;

    // Terminate the Cilkified region.
    uncilkify(w->g, sf);
    _Unwind_Resume(ue_header); // noreturn, although not marked as such
    __builtin_unreachable();
}

extern "C" _Unwind_Reason_Code __cilk_personality_internal(
    __personality_routine std_lib_personality, int version,
    _Unwind_Action actions, uint64_t exception_class,
    _Unwind_Exception *ue_header, _Unwind_Context *context) {

    // If called from outside a Cilkified region --- i.e., after the personality
    // function leaves the last __cilkrts_stack_frame --- then just use
    // std_lib_personality.
    if (__cilkrts_status.need_to_cilkify)
        return std_lib_personality(version, actions, exception_class, ue_header,
                                   context);

    cilk_fiber *fh = __cilkrts_tls.fh;
    __cilkrts_worker *w = fh->worker;
    CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());
    __cilkrts_stack_frame *sf = fh->current_stack_frame;

    if (actions & _UA_SEARCH_PHASE) {
        // don't do anything out of the ordinary during search phase.
        return std_lib_personality(version, actions, exception_class, ue_header,
                                   context);
    } else if (actions & _UA_CLEANUP_PHASE) {
        cilkrts_alert(EXCEPT, "cilk_personality called %p  CFA %p\n",
                      (void *)sf, (void *)get_cfa(context));

        if (sf->flags & CILK_FRAME_UNSYNCHED) {
            sync_in_personality(w, sf, ue_header);
        }

        // After the cilk_sync, the worker may have changed.
        w = get_worker_from_stack(sf);
        CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());

        // Unset the CILK_FRAME_THROWING flag.
        sf->flags &= ~CILK_FRAME_THROWING;

        // Get the saved exception state, if it exists.
        closure_exception *exn_r = get_exception_reducer_or_null(w);

        // Check for a reraised exception, and determine whether to skip
        // performing __cilkrts_leave_frame.
        bool in_reraised_cfa = false;
        bool skip_leaveframe = false;
        if (exn_r != nullptr) {
            in_reraised_cfa = (exn_r->reraise_cfa == (char *)get_cfa(context));
            skip_leaveframe =
                ((exn_r->reraise_cfa != nullptr) && !in_reraised_cfa);
        }
        if (in_reraised_cfa) {
            exn_r->reraise_cfa = nullptr;
        }

        // If the saved exception state contains a different exception than what
        // this personality function is handling, raise that one instead.
        if ((exn_r != nullptr) && (exn_r->exn != nullptr) &&
            (exn_r->exn != (char *)ue_header)) {

            _Unwind_Exception *exn = (_Unwind_Exception *)(exn_r->exn);
            exn_r->exn = nullptr;
            cilkrts_alert(EXCEPT,
                          "cilk_personality calling RaiseException %p\n",
                          (void *)sf);

            // Remember the CFA from which we raised the new exception.
            exn_r->reraise_cfa = (char *)get_cfa(context);

            // Raise the new exception.  NOTE: Calling resume does not seem to
            // work on macOS.
            sf->flags &= ~CILK_FRAME_EXCEPTION_PENDING;
            _Unwind_RaiseException(exn); // noreturn
            __builtin_unreachable();
        }

        // Record whether this frame is detached, which indicates that it's a
        // spawn helper.  The Cilk personality may run on this frame if it
        // itself spawns.  If we end up rerunning the Cilk personality function
        // on the frame after running cleanups, we want to skip doing a
        // __cilkrts_leave_frame at the end, because the cleanup will have
        // already performed a __cilkrts_pause_frame.
        bool isSpawnHelper = (sf->flags & CILK_FRAME_DETACHED);
        bool isLastFrame = (sf->flags & CILK_FRAME_LAST);

        // Run std_lib_personality in cleanup phase on the reduced exception
        // object
        _Unwind_Reason_Code cleanup_res = std_lib_personality(
            version, actions, exception_class, ue_header, context);

        // If we need to continue unwinding the stack, call
        // __cilkrts_leave_frame here.
        if ((cleanup_res == _URC_CONTINUE_UNWIND) && !isSpawnHelper &&
            !skip_leaveframe) {

            if (isLastFrame) {
                // If we're leaving the last Cilk stack frame, we will be
                // longjmping back to the original program call stack.
                if (exn_r != nullptr) {
                    if (exn_r->throwing_fiber) {
                        // Free any fiber we're saving for stack-unwinding,
                        // since we don't need it anymore.
                        cilk_fiber_deallocate_to_pool(w, exn_r->throwing_fiber);
                        exn_r->throwing_fiber = nullptr;
                    }
                    // Free the exception-reducer view.
                    clear_exception_reducer(w, exn_r);
                }
                resume_from_last_frame(w, sf, ue_header); // noreturn
            }
            __cilkrts_leave_frame(sf);
            if (exn_r != nullptr) {
                // We have unwound the stack past the point of parent_rsp, so
                // discard it.
                exn_r->parent_rsp = nullptr;
            }
        }

        return cleanup_res;
    } else {
        return _URC_FATAL_PHASE1_ERROR;
    }
}

extern "C" {
_Unwind_Reason_Code __cilk_personality_c_v0(int version, _Unwind_Action actions,
                                            uint64_t exception_class,
                                            struct _Unwind_Exception *ue_header,
                                            struct _Unwind_Context *context) {
    return __cilk_personality_internal(__gcc_personality_v0, version, actions,
                                       exception_class, ue_header, context);
}

// Legacy name
_Unwind_Reason_Code __cilk_personality_v0(int version, _Unwind_Action actions,
                                          uint64_t exception_class,
                                          struct _Unwind_Exception *ue_header,
                                          struct _Unwind_Context *context) {
    return __cilk_personality_c_v0(version, actions, exception_class, ue_header,
                                   context);
}

_Unwind_Reason_Code __cilk_personality_cpp_v0(
    int version, _Unwind_Action actions, uint64_t exception_class,
    struct _Unwind_Exception *ue_header, struct _Unwind_Context *context) {
    return __cilk_personality_internal(__gxx_personality_v0, version, actions,
                                       exception_class, ue_header, context);
}

}
