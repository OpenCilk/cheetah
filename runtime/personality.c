#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unwind.h>

#include <cilk/cilk_api.h>

#include "cilk-internal.h"
#include "cilk2c.h"
#include "closure-type.h"
#include "closure.h"
#include "fiber-header.h"
#include "fiber.h"
#include "frame.h"
#include "init.h"
#include "local-reducer-api.h"
#include "readydeque.h"
#include "types.h"
#include "worker.h"

typedef _Unwind_Reason_Code (*__personality_routine)(
    int version, _Unwind_Action actions, uint64_t exception_class,
    struct _Unwind_Exception *exception_object,
    struct _Unwind_Context *context);

static char *get_cfa(struct _Unwind_Context *context) {
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

void init_exception_reducer(void *v) {
    struct closure_exception *ex = (struct closure_exception *)(v);
    ex->exn = NULL;
    ex->reraise_cfa = NULL;
    ex->parent_rsp = NULL;
    ex->throwing_fiber = NULL;
}

void reduce_exception_reducer(void *l, void *r) {
    struct closure_exception *lex = (struct closure_exception *)(l);
    struct closure_exception *rex = (struct closure_exception *)(r);
    if (lex->exn == NULL) {
        lex->exn = rex->exn;
        rex->exn = NULL;
    }
    if (rex->exn != NULL) {
        _Unwind_DeleteException((struct _Unwind_Exception *)(rex->exn));
        rex->exn = NULL;
    }
    // Use right-holder logic for reraise_cfa, parent_rsp, and throwing_fiber.
    lex->reraise_cfa = rex->reraise_cfa;
    lex->parent_rsp = rex->parent_rsp;
    if (lex->throwing_fiber)
        cilk_fiber_deallocate_to_pool(__cilkrts_get_tls_worker(),
                                      lex->throwing_fiber);
    lex->throwing_fiber = rex->throwing_fiber;
}

struct closure_exception *get_exception_reducer(__cilkrts_worker *w) {
    return (struct closure_exception *)internal_reducer_lookup(
        w, &exception_reducer, sizeof(exception_reducer),
        init_exception_reducer, reduce_exception_reducer);
}

struct closure_exception *get_exception_reducer_or_null(__cilkrts_worker *w) {
    void *key = (void *)(&exception_reducer);
    struct local_hyper_table *table = get_local_hyper_table(w);
    struct bucket *b = find_hyperobject(table, (uintptr_t)key);
    if (b) {
        CILK_ASSERT(w, key == (void *)b->key);
        // Return the existing view.
        return (struct closure_exception *)(b->value.view);
    }
    // No view was found; just return NULL.
    return NULL;
}

void clear_exception_reducer(__cilkrts_worker *w,
                             struct closure_exception *exn_r) {
    CILK_ASSERT(w, exn_r->throwing_fiber == NULL);
    free(exn_r);
    internal_reducer_remove(w, &exception_reducer);
}

__attribute__((noinline)) static void
sync_in_personality(__cilkrts_worker *w, __cilkrts_stack_frame *sf,
                    struct _Unwind_Exception *ue_header) {
    worker_id self = w->self;
    ReadyDeque *deques = w->g->deques;
    // save floating point state
    sysdep_save_fp_ctrl_state(sf);

    if (__builtin_setjmp(sf->ctx) == 0) {
        // set closure_exception
        struct closure_exception *exn_r = get_exception_reducer(w);
        exn_r->exn = (char *)ue_header;

        deque_lock_self(deques, self);
        Closure *t = deque_peek_bottom(deques, w, self, self);
        Closure_lock(w, self, t);

        // ensure that we return here after a cilk_sync.
        exn_r->parent_rsp = t->orig_rsp;
        t->orig_rsp = (char *)SP(sf);

        Closure_unlock(w, self, t);
        deque_unlock_self(deques, self);

        // save the current fiber for further stack unwinding.
        if (exn_r->throwing_fiber == NULL) {
            exn_r->throwing_fiber = t->fiber;
            t->fiber = NULL;
        }

        // For now, use this flag to indicate that we are setjmping from the
        // personality function. This will "disable" some asserts in
        // scheduler.cpp that we generally want to keep, but are broken in this
        // particular case.
        sf->flags |= CILK_FRAME_THROWING;
        __cilkrts_sync(sf);
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

__attribute__((always_inline)) static void
resume_from_last_frame(__cilkrts_worker *w, __cilkrts_stack_frame *sf,
                 struct _Unwind_Exception *ue_header) {
    cilkrts_alert(CFRAME, w, "__cilkrts_leave_last_frame %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    sf->call_parent = NULL;

    // Terminate the Cilkified region.
    uncilkify(w->g, sf);
    _Unwind_Resume(ue_header); // noreturn, although not marked as such
}

_Unwind_Reason_Code __cilk_personality_internal(
    __personality_routine std_lib_personality, int version,
    _Unwind_Action actions, uint64_t exception_class,
    struct _Unwind_Exception *ue_header, struct _Unwind_Context *context) {

    // If called from outside a Cilkified region --- i.e., after the personality
    // function leaves the last __cilkrts_stack_frame --- then just use
    // std_lib_personality.
    if (__cilkrts_need_to_cilkify)
        return std_lib_personality(version, actions, exception_class, ue_header,
                                   context);

    struct fiber_header *fh = get_this_fiber_header();
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    __cilkrts_stack_frame *sf = __cilkrts_get_current_stack_frame();
    fh->worker = w;

    if (actions & _UA_SEARCH_PHASE) {
        // don't do anything out of the ordinary during search phase.
        return std_lib_personality(version, actions, exception_class, ue_header,
                                   context);
    } else if (actions & _UA_CLEANUP_PHASE) {
        cilkrts_alert(EXCEPT, w,
                      "cilk_personality called %p  CFA %p\n", (void *)sf,
                      (void *)get_cfa(context));

        if (sf->flags & CILK_FRAME_UNSYNCHED) {
            sync_in_personality(w, sf, ue_header);
        }

        // after longjmping back, the worker may have changed.
        w = __cilkrts_get_tls_worker();
        // Update the fiber header, because the execution might be using a saved
        // throwing fiber.
        fh = get_this_fiber_header();
        fh->worker = w;
        // Unset the CILK_FRAME_THROWING flag.
        sf->flags &= ~CILK_FRAME_THROWING;

        // get closure_exception
        struct closure_exception *exn_r = get_exception_reducer_or_null(w);

        // Check for a reraised exception, and determine whether to skip
        // performing __cilkrts_leave_frame.
        bool in_reraised_cfa = false;
        bool skip_leaveframe = false;
        if (exn_r != NULL) {
            in_reraised_cfa = (exn_r->reraise_cfa == (char *)get_cfa(context));
            skip_leaveframe = ((exn_r->reraise_cfa != NULL) && !in_reraised_cfa);
        }
        if (in_reraised_cfa) {
            exn_r->reraise_cfa = NULL;
        }

        if ((exn_r != NULL) && (exn_r->exn != NULL) &&
            (exn_r->exn != (char *)ue_header)) {

            struct _Unwind_Exception *exn =
                    (struct _Unwind_Exception *)(exn_r->exn);
            exn_r->exn = NULL;
            cilkrts_alert(EXCEPT, w,
                          "cilk_personality calling RaiseException %p\n",
                          (void *)sf);

            // Remember the CFA from which we raised the new exception.
            exn_r->reraise_cfa = (char *)get_cfa(context);

            // Raise the new exception.  NOTE: Calling resume does not seem to
            // work on macOS.
            sf->flags &= ~CILK_FRAME_EXCEPTION_PENDING;
            _Unwind_RaiseException(exn); // noreturn
        }

        // Record whether this frame is detached, which indicates that
        // it's a spawn helper.  The Cilk personality may run on this
        // frame if it itself spawns.  If we end up rerunning the Cilk
        // personality function on the frame after running cleanups, we
        // want to skip doing a pop_frame and leave_frame at the end,
        // because the cleanup will have already performed a pop_frame and
        // pause_frame.
        bool isSpawnHelper = (sf->flags & CILK_FRAME_DETACHED);
        bool isLastFrame = (sf->flags & CILK_FRAME_LAST);

        // Run std_lib_personality in cleanup phase on the reduced exception
        // object
        _Unwind_Reason_Code cleanup_res = std_lib_personality(
            version, actions, exception_class, ue_header, context);

        // if we need to continue unwinding the stack, leave_frame here
        if ((cleanup_res == _URC_CONTINUE_UNWIND) && !isSpawnHelper &&
            !skip_leaveframe) {

            if (isLastFrame) {
                // If we're leaving the last Cilk stack frame, we will be
                // longjmping back to the original program call stack.
                if (exn_r != NULL) {
                    if (exn_r->throwing_fiber) {
                        // Free any fiber we're saving for stack-unwinding,
                        // since we don't need it anymore.
                        cilk_fiber_deallocate_to_pool(w, exn_r->throwing_fiber);
                        exn_r->throwing_fiber = NULL;
                    }
                    // Free the exception-reducer view.
                    clear_exception_reducer(w, exn_r);
                }
                resume_from_last_frame(w, sf, ue_header); // noreturn
            }
            __cilkrts_leave_frame(sf);
            if (exn_r != NULL) {
                // We have unwound the stack past the point of parent_rsp, so
                // discard it.
                exn_r->parent_rsp = NULL;
            }
        }

        return cleanup_res;
    } else {
        return _URC_FATAL_PHASE1_ERROR;
    }
}
