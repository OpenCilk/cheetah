#ifndef _CILK2C_H
#define _CILK2C_H

#include "cilk-internal.h"
#include <stdlib.h>

// Returns 1 if the current exection is running on Cilk workers, 0 otherwise.
CHEETAH_API int __cilkrts_running_on_workers(void);

// ABI functions inlined by the compiler (provided as a bitcode file after
// compiling runtime) are defined in cilk2c_inline.c.
// ABI functions not inlined by the compiler are defined in cilk2c.c.

// Inserted at the entry of a spawning function that is not itself a spawn
// helper.  Initializes the stack frame sf allocated for that function.
CHEETAH_INTERNAL void __cilkrts_enter_frame(__cilkrts_stack_frame *sf);

// Inserted at the entry of a spawn helper, i.e., a function that must have been
// spawned.  Initializes the stack frame sf allocated for that function.
CHEETAH_INTERNAL void __cilkrts_enter_frame_helper(__cilkrts_stack_frame *sf);

// Prepare to perform a spawn.  This function may return once or twice,
// returning 0 the first time and 1 the second time.  This function is intended
// to be used follows:
//
//   if (0 == __cilk_spawn_prepare(sf)) {
//     spawn_helper(args);
//   }
CHEETAH_INTERNAL int __cilk_prepare_spawn(__cilkrts_stack_frame *sf);

// Called in the spawn helper immediately before the spawned computation.
// Enables the parent function to be stollen.
CHEETAH_INTERNAL void __cilkrts_detach(__cilkrts_stack_frame *sf);

// Check if the runtime is storing an exception we need to handle later, and
// raises that exception if so.
CHEETAH_API void __cilkrts_check_exception_raise(__cilkrts_stack_frame *sf);

// Check if the runtime is storing an exception we need to handle later, and
// resumes unwinding with that exception if so.
CHEETAH_API void __cilkrts_check_exception_resume(__cilkrts_stack_frame *sf);

// Performs runtime operations to handle a cilk_sync.
__attribute__((noreturn, nothrow))
CHEETAH_API void __cilkrts_sync(__cilkrts_stack_frame *sf);

// Implements a cilk_sync when the cilk_sync might produce an exception that
// needs to be handled.
CHEETAH_INTERNAL void __cilk_sync(__cilkrts_stack_frame *sf);

// Implements a cilk_sync when the cilk_sync is guaranteed not to produce an
// exception that needs to be handled.
CHEETAH_INTERNAL void __cilk_sync_nothrow(__cilkrts_stack_frame *sf);

// Deprecated?  Removes the current stack frame from the bottom of the stack.
// (This logic has been manually inlined into __cilkrts_leave_frame,
// __cilkrts_leave_frame_helper, and __cilkrts_pause_frame.)
CHEETAH_INTERNAL void __cilkrts_pop_frame(__cilkrts_stack_frame *sf);

// Inserted on return from a spawning function that is not itself a spawn
// helper.  Performs Cilk's return protocol for such functions.
CHEETAH_INTERNAL void __cilkrts_leave_frame(__cilkrts_stack_frame *sf);

// Inserted on return from a spawn-helper function.  Performs Cilk's return
// protocol for such functions.
CHEETAH_INTERNAL void __cilkrts_leave_frame_helper(__cilkrts_stack_frame *sf);

// Performs all necessary operations on return from a spawning function that is
// not itself a spawn helper.
CHEETAH_INTERNAL void __cilk_parent_epilogue(__cilkrts_stack_frame *sf);

// Performs all necessary operations on return from a spawn-helper function.
CHEETAH_INTERNAL void __cilk_helper_epilogue(__cilkrts_stack_frame *sf);

// Performs all necessary runtime updates when execution enters a landingpad in
// a spawning function.
CHEETAH_INTERNAL
void __cilkrts_enter_landingpad(__cilkrts_stack_frame *sf, int32_t sel);

// Called from __cilkrts_enter_landingpad to optionally fix the current stack
// pointer and cleanup a fiber that was previously saved for exception handling.
CHEETAH_API void __cilkrts_cleanup_fiber(__cilkrts_stack_frame *, int32_t sel);

// Performs Cilk's return protocol on an exceptional return (i.e., a resume)
// from a spawn-helper function.
CHEETAH_INTERNAL void __cilkrts_pause_frame(__cilkrts_stack_frame *sf, char *exn);

// Inserted on an exceptional return (i.e., a resume) from a spawn-helper
// function.
CHEETAH_INTERNAL void __cilk_pause_frame(__cilkrts_stack_frame *sf, char *exn);

// Compute the grainsize for a cilk_for loop at runtime, based on the number n
// of loop iterations.
CHEETAH_INTERNAL uint8_t __cilkrts_cilk_for_grainsize_8(uint8_t n);
CHEETAH_INTERNAL uint16_t __cilkrts_cilk_for_grainsize_16(uint16_t n);
CHEETAH_INTERNAL uint32_t __cilkrts_cilk_for_grainsize_32(uint32_t n);
CHEETAH_INTERNAL uint64_t __cilkrts_cilk_for_grainsize_64(uint64_t n);

// Not marked as CHEETAH_API as it may be deprecated soon
unsigned __cilkrts_get_nworkers(void);
//CHEETAH_API int64_t* __cilkrts_get_pedigree(void);
//void __cilkrts_pedigree_bump_rank(void);
#endif
