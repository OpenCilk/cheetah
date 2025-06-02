// Runtime functions that are known to the compiler.

#include <stdint.h>
#include "cilk/cilk_api.h"

struct __cilkrts_stack_frame;
struct __cilkrts_worker;

#ifdef __cplusplus
extern "C" {
#endif

// Inserted at the entry of a spawning function that is not itself a spawn
// helper.  Initializes the stack frame sf allocated for that function.
void __cilkrts_enter_frame(struct __cilkrts_stack_frame *sf) __CILKRTS_NOTHROW;
// Inserted at the entry of a spawn helper, i.e., a function that must have been
// spawned.  Initializes the stack frame sf allocated for that function.
void __cilkrts_enter_frame_helper(struct __cilkrts_stack_frame *sf,
                                  struct __cilkrts_stack_frame *parent,
                                  bool spawner) __CILKRTS_NOTHROW;

// Prepare to perform a spawn.  This function may return once or twice,
// returning 0 the first time and 1 the second time.  This function is intended
// to be used follows:
//
//   if (0 == __cilk_spawn_prepare(sf)) {
//     spawn_helper(args);
//   }
int __cilk_prepare_spawn(struct __cilkrts_stack_frame *sf) __CILKRTS_NOTHROW;

// Called in the spawn helper immediately before the spawned computation.
// Enables the parent function to be stollen.
void __cilkrts_detach(struct __cilkrts_stack_frame *sf,
                      struct __cilkrts_stack_frame *parent) __CILKRTS_NOTHROW;

// Inserted on return from a spawning function that is not itself a spawn
// helper.  Performs Cilk's return protocol for such functions.
// Marked API because it is used inside the library by personality.cpp.
CHEETAH_API
void __cilkrts_leave_frame(struct __cilkrts_stack_frame *sf);
// Inserted on return from a spawn-helper function.  Performs Cilk's return
// protocol for such functions.
void __cilkrts_leave_frame_helper(struct __cilkrts_stack_frame *sf,
                                  struct __cilkrts_stack_frame *parent,
                                  bool spawner);

// Performs all necessary operations on return from a spawning function that is
// not itself a spawn helper.
void __cilk_parent_epilogue(struct __cilkrts_stack_frame *sf);
// Performs all necessary operations on return from a spawn-helper function.
__attribute__((always_inline))
void __cilk_helper_epilogue(struct __cilkrts_stack_frame *sf,
                            struct __cilkrts_stack_frame *parent,
                            bool spawner);
void __cilk_helper_epilogue_exn(struct __cilkrts_stack_frame *sf,
                                struct __cilkrts_stack_frame *parent,
                                char *exn, bool spawner);

void __cilkrts_enter_landingpad(struct __cilkrts_stack_frame *sf, int32_t sel);
// Performs Cilk's return protocol on an exceptional return (i.e., a resume)
// from a spawn-helper function.
void __cilkrts_pause_frame(struct __cilkrts_stack_frame *sf,
                           struct __cilkrts_stack_frame *parent,
                           char *exn, bool spawner);

// Compute the grainsize for a cilk_for loop at runtime, based on the number n
// of loop iterations.
uint8_t __cilkrts_cilk_for_grainsize_8(uint8_t n) __CILKRTS_NOTHROW;
uint16_t __cilkrts_cilk_for_grainsize_16(uint16_t n) __CILKRTS_NOTHROW;
uint32_t __cilkrts_cilk_for_grainsize_32(uint32_t n) __CILKRTS_NOTHROW;
uint64_t __cilkrts_cilk_for_grainsize_64(uint64_t n) __CILKRTS_NOTHROW;

// Performs runtime operations to handle a cilk_sync.
void __cilk_sync(struct __cilkrts_stack_frame *sf);

// Implements a cilk_sync when the cilk_sync is guaranteed not to produce an
// exception that needs to be handled locally.
void __cilk_sync_nothrow(struct __cilkrts_stack_frame *sf);

void *__cilkrts_reducer_lookup(void *key, size_t size,
                               void *id, void *reduce);

void __cilkrts_reducer_register_32(void *key, uint32_t size,
                                   void (*id)(void *),
                                   void (*reduce)(void *, void *))
  __CILKRTS_NOTHROW;

void __cilkrts_reducer_register_64(void *key, uint64_t size,
                                   void (*id)(void *),
                                   void (*reduce)(void *, void *))
  __CILKRTS_NOTHROW;

void __cilkrts_reducer_unregister(void *key) __CILKRTS_NOTHROW;

#ifdef __cplusplus
}
#endif
