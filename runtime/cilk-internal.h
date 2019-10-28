#ifndef _CILK_INTERNAL_H
#define _CILK_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Includes
#include "debug.h"
#include "fiber.h"
#include "internal-malloc.h"
#include "jmpbuf.h"
#include "mutex.h"
#include "rts-config.h"
#include "sched_stats.h"
#include "types.h"

#define NOBODY 0xffffffffu /* type worker_id */

#if CILK_STATS
#define WHEN_CILK_STATS(ex) ex
#else
#define WHEN_CILK_STATS(ex)
#endif

//===============================================
// Cilk stack frame related defs
//===============================================

/**
 * Every spawning function has a frame descriptor.  A spawning function
 * is a function that spawns or detaches.  Only spawning functions
 * are visible to the Cilk runtime.
 *
 * NOTE: if you are using the Tapir compiler, you should not change
 * these fields; ok to change for hand-compiled code.
 * See Tapir compiler ABI:
 * https://github.com/wsmoses/Tapir-LLVM/blob/cilkr/lib/Transforms/Tapir/CilkRABI.cpp
 */
struct __cilkrts_stack_frame {
    // Flags is a bitfield with values defined below. Client code
    // initializes flags to 0 (except for the ABI version field)
    // before the first Cilk operation.
    uint32_t flags;

#ifdef OPENCILK_ABI
    uint32_t magic;
#else
    /* 32 bit hole here on 64 bit machines */
#endif

    // call_parent points to the __cilkrts_stack_frame of the closest
    // ancestor spawning function, including spawn helpers, of this frame.
    // It forms a linked list ending at the first stolen frame.
    __cilkrts_stack_frame *call_parent;

    // The client copies the worker from TLS here when initializing
    // the structure.  The runtime ensures that the field always points
    // to the __cilkrts_worker which currently "owns" the frame.
    __cilkrts_worker *worker;

    // Before every spawn and nontrivial sync the client function
    // saves its continuation here.
    jmpbuf ctx;

    /**
     * Architecture-specific floating point state.
     * mxcsr and fpcsr should be set when setjmp is called in client code.
     *
     * They are for linux / x86_64 platforms only.  Note that the Win64
     * jmpbuf for the Intel64 architecture already contains this information
     * so there is no need to use these fields on that OS/architecture.
     */
#ifdef __SSE__
#define CHEETAH_SAVE_MXCSR
    uint32_t mxcsr;
#else
    uint32_t reserved1;
#endif
#ifndef OPENCILK_ABI /* x87 flags not preserved in OpenCilk */
#if defined i386 || defined __x86_64__
#define CHEETAH_SAVE_FPCSR
    uint16_t fpcsr;
#else
    uint16_t reserved2;
#endif
#endif

#ifndef OPENCILK_ABI
    /**
     * reserved is not used at this time.  Client code should initialize it
     * to 0 before the first Cilk operation
     */
    uint16_t reserved3; // ANGE: leave it to make it 8-byte aligned.
    uint32_t magic;
#endif
};

//===========================================================
// Value defines for the flags field in cilkrts_stack_frame
//===========================================================

/* CILK_FRAME_STOLEN is set if the frame has ever been stolen. */
#define CILK_FRAME_STOLEN 0x001

/* CILK_FRAME_UNSYNCHED is set if the frame has been stolen and
   is has not yet executed _Cilk_sync. It is technically a misnomer in that a
   frame can have this flag set even if all children have returned. */
#define CILK_FRAME_UNSYNCHED 0x002

/* Is this frame detached (spawned)? If so the runtime needs
   to undo-detach in the slow path epilogue. */
#define CILK_FRAME_DETACHED 0x004

/* CILK_FRAME_EXCEPTION_PENDING is set if the frame has an exception
   to handle after syncing. */
#define CILK_FRAME_EXCEPTION_PENDING 0x008

/* Is this frame excepting, meaning that a stolen continuation threw? */
#define CILK_FRAME_EXCEPTING 0x010

/* Is this the last (oldest) Cilk frame? */
#define CILK_FRAME_LAST 0x080

/* Is this frame in the epilogue, or more generally after the last
   sync when it can no longer do any Cilk operations? */
#define CILK_FRAME_EXITING 0x100

/* Is this frame handling an exception? */
// TODO: currently only used when throwing an exception from the continuation
//       (i.e. from the personality function). Used in scheduler.c to disable
//       asserts that fail if trying to longjmp back to the personality
//       function.
#define CILK_FRAME_SYNC_READY 0x200

#define GET_CILK_FRAME_VERSION(F) (((F) >> 24) & 255)
#define CILK_FRAME_VERSION (__CILKRTS_ABI_VERSION << 24)

//===========================================================
// Helper functions for the flags field in cilkrts_stack_frame
//===========================================================

/* A frame is set to be stolen as long as it has a corresponding Closure */
static inline void __cilkrts_set_stolen(__cilkrts_stack_frame *sf) {
    sf->flags |= CILK_FRAME_STOLEN;
}

/* A frame is set to be unsynced only if it has parallel subcomputation
 * underneathe, i.e., only if it has spawned children executing on a different
 * worker
 */
static inline void __cilkrts_set_unsynced(__cilkrts_stack_frame *sf) {
    sf->flags |= CILK_FRAME_UNSYNCHED;
}

static inline void __cilkrts_set_synced(__cilkrts_stack_frame *sf) {
    sf->flags &= ~CILK_FRAME_UNSYNCHED;
}

/* Returns nonzero if the frame is not synched. */
static inline int __cilkrts_unsynced(__cilkrts_stack_frame *sf) {
    return (sf->flags & CILK_FRAME_UNSYNCHED);
}

/* Returns nonzero if the frame has been stolen. */
static inline int __cilkrts_stolen(__cilkrts_stack_frame *sf) {
    return (sf->flags & CILK_FRAME_STOLEN);
}

/* Returns nonzero if the frame is synched. */
static inline int __cilkrts_synced(__cilkrts_stack_frame *sf) {
    return ((sf->flags & CILK_FRAME_UNSYNCHED) == 0);
}

/* Returns nonzero if the frame has never been stolen. */
static inline int __cilkrts_not_stolen(__cilkrts_stack_frame *sf) {
    return ((sf->flags & CILK_FRAME_STOLEN) == 0);
}

//===============================================
// Worker related definition
//===============================================

// Forward declaration
typedef struct global_state global_state;
typedef struct local_state local_state;
typedef struct __cilkrts_stack_frame **CilkShadowStack;

// Actual declaration
struct rts_options {
    unsigned int nproc;
    int deqdepth;
    int64_t stacksize;
    int fiber_pool_cap;
};

// clang-format off
#define DEFAULT_OPTIONS                                            \
    {                                                              \
        DEFAULT_NPROC,          /* num of workers to create */     \
        DEFAULT_DEQ_DEPTH,      /* num of entries in deque */      \
        DEFAULT_STACK_SIZE,     /* stack size to use for fiber */  \
        DEFAULT_FIBER_POOL_CAP, /* alloc_batch_size */             \
    }
// clang-format on

// Actual declaration
struct global_state {
    /* globally-visible options (read-only after init) */
    struct rts_options options;

    /*
     * this string is printed when an assertion fails.  If we just inline
     * it, apparently gcc generates many copies of the string.
     */
    const char *assertion_failed_msg;
    const char *stack_overflow_msg;

    /* dynamically-allocated array of deques, one per processor */
    struct ReadyDeque *deques;
    struct __cilkrts_worker **workers;
    pthread_t *threads;
    struct Closure *invoke_main;

    struct cilk_fiber_pool fiber_pool __attribute__((aligned(CILK_CACHE_LINE)));
    struct global_im_pool im_pool __attribute__((aligned(CILK_CACHE_LINE)));
    struct cilk_im_desc im_desc __attribute__((aligned(CILK_CACHE_LINE)));
    cilk_mutex im_lock; // lock for accessing global im_desc

    volatile bool invoke_main_initialized;
    volatile atomic_bool start;
    volatile atomic_bool done;
    volatile atomic_int cilk_main_return;

    cilk_mutex print_lock; // global lock for printing messages

    int cilk_main_argc;
    char **cilk_main_args;

    WHEN_SCHED_STATS(struct global_sched_stats stats;)
};

// Actual declaration

enum __cilkrts_worker_state {
    WORKER_IDLE = 10,
    WORKER_SCHED,
    WORKER_STEAL,
    WORKER_RUN
};

struct local_state {
    __cilkrts_stack_frame **shadow_stack;

    unsigned short state; /* __cilkrts_worker_state */
    bool lock_wait;
    bool provably_good_steal;
    unsigned int rand_next;

    jmpbuf rts_ctx;
    struct cilk_fiber_pool fiber_pool;
    struct cilk_im_desc im_desc;
    struct cilk_fiber *fiber_to_free;
    WHEN_SCHED_STATS(struct sched_stats stats;)
};

/**
 * NOTE: if you are using the Tapir compiler, you should not change
 * these fields; ok to change for hand-compiled code.
 * See Tapir compiler ABI:
 * https://github.com/OpenCilk/opencilk-project/blob/release/9.x/llvm/lib/Transforms/Tapir/CilkRABI.cpp
 **/
struct __cilkrts_worker {
    // T and H pointers in the THE protocol
    _Atomic(__cilkrts_stack_frame **) tail;
    _Atomic(__cilkrts_stack_frame **) head;
    _Atomic(__cilkrts_stack_frame **) exc;

    // Limit of the Lazy Task Queue, to detect queue overflow
    __cilkrts_stack_frame **ltq_limit;

    // Worker id, a small integer
    worker_id self;

    // Global state of the runtime system, opaque to the client.
    global_state *g;

    // Additional per-worker state hidden from the client.
    local_state *l;

    // A slot that points to the currently executing Cilk frame.
    __cilkrts_stack_frame *current_stack_frame;

#ifdef REDUCER_MODULE
    // Map from reducer names to reducer values
    cilkred_map *reducer_map;
#endif
};

#endif // _CILK_INTERNAL_H
