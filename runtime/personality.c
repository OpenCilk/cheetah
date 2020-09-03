#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unwind.h>

#include "cilk-internal.h"
#include "cilk2c.h"
#include "closure.h"
#include "readydeque.h"

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

_Unwind_Reason_Code __cilk_personality_internal(
    __personality_routine std_lib_personality, int version,
    _Unwind_Action actions, uint64_t exception_class,
    struct _Unwind_Exception *ue_header, struct _Unwind_Context *context) {

    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    __cilkrts_stack_frame *sf = w->current_stack_frame;

    if (actions & _UA_SEARCH_PHASE) {
        // don't do anything out of the ordinary during search phase.
        return std_lib_personality(version, actions, exception_class, ue_header,
                                   context);
    } else if (actions & _UA_CLEANUP_PHASE) {
        cilkrts_alert(EXCEPT, sf->worker,
                      "cilk_personality called %p  CFA %p\n", (void *)sf,
                      (void *)get_cfa(context));

        if (sf->flags & CILK_FRAME_UNSYNCHED) {
            // save floating point state
            __cilkrts_save_fp_ctrl_state(sf);

            if (__builtin_setjmp(sf->ctx) == 0) {
                deque_lock_self(w);
                Closure *t = deque_peek_bottom(w, w->self);

                // ensure that we return here after a cilk_sync.
                t->parent_rsp = t->orig_rsp;
                t->orig_rsp = (char *)SP(sf);

                // set closure_exception
                t->user_exn.exn = (char *)ue_header;
                /*
                t->user_exn.frame = sf;
                t->user_exn.fiber = t->fiber;
                */

                deque_unlock_self(w);

                // For now, use this flag to indicate that we are setjmping from
                // the personality function. This will "disable" some asserts in
                // scheduler.cpp that we generally want to keep, but are broken
                // in this particular case.
                sf->flags |= CILK_FRAME_EXCEPTING;
                __cilkrts_sync(sf);
            }
        }

        // after longjmping back, the worker may have changed.
        w = __cilkrts_get_tls_worker();
        deque_lock_self(w);
        Closure *t = deque_peek_bottom(w, w->self);
        deque_unlock_self(w);
        bool in_reraised_cfa = (t->reraise_cfa == (char *)get_cfa(context));
        bool skip_leaveframe = ((t->reraise_cfa != NULL) && !in_reraised_cfa);
        if (in_reraised_cfa)
            t->reraise_cfa = NULL;

        if (t->user_exn.exn != NULL && t->user_exn.exn != (char *)ue_header) {
            cilkrts_alert(EXCEPT, sf->worker,
                          "cilk_personality calling RaiseException %p\n",
                          (void *)sf);

            // Remember the CFA from which we raised the new exception.
            t->reraise_cfa = (char *)get_cfa(context);
            // Raise the new exception.
            __cilkrts_check_exception_raise(sf);
            // Calling Resume instead of RaiseException also appears to work,
            // and is a bit faster.
            // NOTE: Calling resume does not seem to work on MacOSX.
            // __cilkrts_check_exception_resume(sf);
        }

        // Record whether this frame is detached, which indicates that
        // it's a spawn helper.  The Cilk personality may run on this
        // frame if it itself spawns.  If we end up rerunning the Cilk
        // personality function on the frame after running cleanups, we
        // want to skip doing a pop_frame and leave_frame at the end,
        // because the cleanup will have already performed a pop_frame and
        // pause_frame.
        bool isSpawnHelper = (sf->flags & CILK_FRAME_DETACHED);

        // run gxx_personality_v0 in cleanup phase on the reduced exception
        // object
        _Unwind_Reason_Code cleanup_res = std_lib_personality(
            version, actions, exception_class, ue_header, context);

        // if we need to continue unwinding the stack, pop_frame + leave_frame
        // here
        if ((cleanup_res == _URC_CONTINUE_UNWIND) && !isSpawnHelper &&
            !skip_leaveframe) {
            __cilkrts_pop_frame(sf);
            __cilkrts_leave_frame(sf);
        }

        return cleanup_res;
    } else {
        return _URC_FATAL_PHASE1_ERROR;
    }
}
