#ifndef _CILK2C_H
#define _CILK2C_H

#include "cilk-internal.h"
#include "frame.h"
#include "rts-config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 if the current execution is running on Cilk workers, 0 otherwise.
CHEETAH_API int __cilkrts_running_on_workers(void) __CILKRTS_NOTHROW;

// ABI functions inlined by the compiler (provided as a bitcode file after
// compiling runtime) are declared in cilk2c_inline.h and defined in
// cilk2c_inline.c.
// ABI functions not inlined by the compiler are defined in cilk2c.c.

// Check if the runtime is storing an exception we need to handle later, and
// raises that exception if so.
CHEETAH_API void __cilkrts_check_exception_raise(__cilkrts_stack_frame *sf);

// Check if the runtime is storing an exception we need to handle later, and
// resumes unwinding with that exception if so.
CHEETAH_API void __cilkrts_check_exception_resume(__cilkrts_stack_frame *sf);

// Implements a cilk_sync when the cilk_sync might produce an exception that
// needs to be handled.
CHEETAH_API void __cilkrts_sync(__cilkrts_stack_frame *sf);

// Called from __cilkrts_enter_landingpad to optionally fix the current stack
// pointer and cleanup a fiber that was previously saved for exception handling.
CHEETAH_API void __cilkrts_cleanup_fiber(__cilkrts_stack_frame *, int32_t sel)
  __CILKRTS_NOTHROW;

// Not marked as CHEETAH_API as it may be deprecated soon
unsigned __cilkrts_get_nworkers(void) __CILKRTS_NOTHROW;

CHEETAH_API void __cilkrts_set_return(__cilkrts_worker *const ws);
CHEETAH_API void __cilkrts_exception_handler(__cilkrts_worker *w, char *exn);
CHEETAH_API void __cilkrts_do_reductions(__cilkrts_stack_frame *sf);

#ifdef __cplusplus
}
#endif

#endif
