#ifndef _CILK_FRAME_H
#define _CILK_FRAME_H

#include "rts-config.h"

#include <stdint.h>
#include "jmpbuf.h"

struct __cilkrts_worker;
struct __cilkrts_stack_frame;

/**
 * Every spawning function has a frame descriptor.  A spawning function
 * is a function that spawns or detaches.  Only spawning functions
 * are visible to the Cilk runtime.
 */
struct __cilkrts_stack_frame {
    // Flags is a bitfield with values defined below. Client code
    // initializes flags to 0 before the first Cilk operation.
    uint32_t flags;
    // The magic number includes the ABI version and a hash of the
    // layout of this structure.
    uint32_t magic;

    // call_parent points to the __cilkrts_stack_frame of the closest
    // ancestor spawning function, including spawn helpers, of this frame.
    // It forms a linked list ending at the first stolen frame.
    struct __cilkrts_stack_frame *call_parent;

    // The client copies the worker from TLS here when initializing
    // the structure.  The runtime ensures that the field always points
    // to the __cilkrts_worker which currently "owns" the frame.
    //
    // TODO: Remove this pointer?  This pointer only seems to be needed for
    // debugging purposes.  When the worker structure is genuinely needed, it
    // seems to be accessible by calling __cilkrts_get_tls_worker(), which will
    // be inlined and optimized to a simple move from TLS.
    _Atomic(struct __cilkrts_worker *) worker;

    // Before every spawn and nontrivial sync the client function
    // saves its continuation here.
    jmpbuf ctx;

    // Optional state for an extension, only maintained if
    // __cilkrts_use_extension == true.
    void *extension;
};

//===========================================================
// Value defines for the flags field in cilkrts_stack_frame
//===========================================================

/* CILK_FRAME_STOLEN is set if the frame has ever been stolen. */
#define CILK_FRAME_STOLEN            0x001

/* CILK_FRAME_UNSYNCHED is set if the frame has been stolen and
   is has not yet executed _Cilk_sync. It is technically a misnomer in that a
   frame can have this flag set even if all children have returned. */
#define CILK_FRAME_UNSYNCHED         0x002

/* Is this frame detached (spawned)? If so the runtime needs
   to undo-detach in the slow path epilogue. */
#define CILK_FRAME_DETACHED          0x004

/* CILK_FRAME_EXCEPTION_PENDING is set if the frame has an exception
   to handle after syncing. */
#define CILK_FRAME_EXCEPTION_PENDING 0x008

/* Is this frame excepting, meaning that a stolen continuation threw? */
#define CILK_FRAME_EXCEPTING         0x010

/* Is this the last (oldest) Cilk frame? */
#define CILK_FRAME_LAST              0x080

/* Is this frame handling an exception? */
// TODO: currently only used when throwing an exception from the continuation
//       (i.e. from the personality function). Used in scheduler.c to disable
//       asserts that fail if trying to longjmp back to the personality
//       function.
#define CILK_FRAME_SYNC_READY        0x200

static const uint32_t frame_magic =
    (((((((((((((__CILKRTS_ABI_VERSION * 13) +
                offsetof(struct __cilkrts_stack_frame, worker)) *
               13) +
              offsetof(struct __cilkrts_stack_frame, ctx)) *
             13) +
            offsetof(struct __cilkrts_stack_frame, magic)) *
           13) +
          offsetof(struct __cilkrts_stack_frame, flags)) *
         13) +
        offsetof(struct __cilkrts_stack_frame, call_parent)) *
       13) +
      offsetof(struct __cilkrts_stack_frame, extension)));

#define CHECK_CILK_FRAME_MAGIC(G, F) (frame_magic == (F)->magic)

//===========================================================
// Helper functions for the flags field in cilkrts_stack_frame
//===========================================================

/* A frame is set to be stolen as long as it has a corresponding Closure */
static inline void __cilkrts_set_stolen(struct __cilkrts_stack_frame *sf) {
    sf->flags |= CILK_FRAME_STOLEN;
}

/* A frame is set to be unsynced only if it has parallel subcomputation
 * underneathe, i.e., only if it has spawned children executing on a different
 * worker
 */
static inline void __cilkrts_set_unsynced(struct __cilkrts_stack_frame *sf) {
    sf->flags |= CILK_FRAME_UNSYNCHED;
}

static inline void __cilkrts_set_synced(struct __cilkrts_stack_frame *sf) {
    sf->flags &= ~CILK_FRAME_UNSYNCHED;
}

/* Returns nonzero if the frame has been stolen.
   Only used in assertions. */
static inline int __cilkrts_stolen(struct __cilkrts_stack_frame *sf) {
    return (sf->flags & CILK_FRAME_STOLEN);
}

/* Returns nonzero if the frame is synched.  Only used in assertions. */
static inline int __cilkrts_synced(struct __cilkrts_stack_frame *sf) {
    return ((sf->flags & CILK_FRAME_UNSYNCHED) == 0);
}

/* Returns nonzero if the frame has never been stolen. */
static inline int __cilkrts_not_stolen(struct __cilkrts_stack_frame *sf) {
    return ((sf->flags & CILK_FRAME_STOLEN) == 0);
}

#endif /* _CILK_FRAME_H */
