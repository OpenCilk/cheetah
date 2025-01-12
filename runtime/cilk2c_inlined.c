// =============================================================================
// This file contains the compiler-runtime ABI.  This file is compiled to LLVM
// bitcode, which the compiler then includes and inlines when it compiles a Cilk
// program.
// =============================================================================

#include <stdatomic.h>
#include <stdio.h>
#include <unwind.h>

#include "cilk-internal.h"
#include "cilk2c.h"
#include "debug.h"
#include "fiber.h"
#include "fiber-header.h"
#include "frame.h"
#include "global.h"
#include "init.h"
#include "local-reducer-api.h"
#include "scheduler.h"

#include "pedigree_ext.c"
#include "worker.h"

// This variable encodes the alignment of a __cilkrts_stack_frame, both in its
// value and in its own alignment.  Because LLVM IR does not associate
// alignments with types, this variable communicates the desired alignment to
// the compiler instead.
_Alignas(__cilkrts_stack_frame)
size_t __cilkrts_stack_frame_align = __alignof__(__cilkrts_stack_frame);

__attribute__((always_inline)) unsigned __cilkrts_get_nworkers(void) {
    return __cilkrts_nproc;
}

// Internal method to get the Cilk worker ID.  Intended for debugging purposes.
//
// TODO: Figure out how we want to support worker-local storage.
__attribute__((always_inline))
unsigned __cilkrts_get_worker_number(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (w)
        return w->self;
    // If the worker structure is not yet initialized, pretend we're worker 0.
    return 0;
}

void *__cilkrts_reducer_lookup(void *key, size_t size,
                               void *identity_ptr, void *reduce_ptr) {
    // If we're outside a cilkified region, then the key is the view.
    if (__cilkrts_need_to_cilkify)
        return key;
    struct local_hyper_table *table = get_hyper_table();
    struct bucket *b = find_hyperobject(table, (uintptr_t)key);
    if (__builtin_expect(!!b, true)) {
        // Return the existing view.
        return b->value.view;
    }

    return __cilkrts_insert_new_view(table, (uintptr_t)key, size,
                                     (__cilk_identity_fn)identity_ptr,
                                     (__cilk_reduce_fn)reduce_ptr);
}

// Begin a Cilkified region.  The routine runs on a Cilkifying thread to
// transfer the execution of this function to the workers in global_state g.
// This routine must be inlined for correctness.
static inline __attribute__((always_inline)) void
cilkify(__cilkrts_stack_frame *sf) {
    // After inlining, the setjmp saves the processor state, including the frame
    // pointer, of the Cilk function.
    if (__builtin_setjmp(sf->ctx) == 0) {
        sysdep_save_fp_ctrl_state(sf);
        __cilkrts_internal_invoke_cilkified_root(sf);
    } else {
        sanitizer_finish_switch_fiber();
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

// Enter a new Cilk function, i.e., a function that contains a cilk_spawn.  This
// function must be inlined for correctness.
__attribute__((always_inline)) void
__cilkrts_enter_frame(__cilkrts_stack_frame *sf) {
    sf->flags = 0;
    if (__cilkrts_need_to_cilkify) {
        cilkify(sf);
    }
    cilkrts_alert(CFRAME, "__cilkrts_enter_frame %p", (void *)sf);

    sf->magic = frame_magic;

    struct cilk_fiber *fh = __cilkrts_current_fh;
    sf->fh = fh;
    sf->call_parent = fh->current_stack_frame;
    fh->current_stack_frame = sf;

    // WHEN_CILK_DEBUG(sf->magic = CILK_STACKFRAME_MAGIC);
}

// Enter a spawn helper, i.e., a fucntion containing code that was cilk_spawn'd.
// This function initializes worker and stack_frame structures.  Because this
// routine will always be executed by a Cilk worker, it is optimized compared to
// its counterpart, __cilkrts_enter_frame.
__attribute__((always_inline)) void
__cilkrts_enter_frame_helper(__cilkrts_stack_frame *sf,
                             __cilkrts_stack_frame *parent, bool spawner) {
    cilkrts_alert(CFRAME, "__cilkrts_enter_frame_helper %p", (void *)sf);

    sf->flags = 0;
    sf->magic = frame_magic;

    struct cilk_fiber *fh = parent->fh;
    sf->fh = fh;
    if (spawner) {
        sf->call_parent = parent;
        fh->current_stack_frame = sf;
    }
}

__attribute__((always_inline)) int
__cilk_prepare_spawn(__cilkrts_stack_frame *sf) {
    sysdep_save_fp_ctrl_state(sf);
    int res = __builtin_setjmp(sf->ctx);
    if (res != 0) {
        sanitizer_finish_switch_fiber();
    }
    return res;
}

// Detach the given Cilk stack frame, allowing other Cilk workers to steal the
// parent frame.
__attribute__((always_inline)) void
__cilkrts_detach(__cilkrts_stack_frame *sf, __cilkrts_stack_frame *parent) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, "__cilkrts_detach %p", (void *)sf);

    CILK_ASSERT(CHECK_CILK_FRAME_MAGIC(w->g, sf));

    if (USE_EXTENSION) {
        __cilkrts_extend_spawn(w, &parent->extension, &w->extension);
    }

    sf->flags |= CILK_FRAME_DETACHED;
    struct __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    CILK_ASSERT((tail + 1) < w->ltq_limit);

    // store parent at *tail, and then increment tail
    *tail++ = parent;
    /* Release ordering ensures the two preceding stores are visible. */
    atomic_store_explicit(&w->tail, tail, memory_order_release);
}

__attribute__((always_inline)) void __cilk_sync(__cilkrts_stack_frame *sf) {
    if (sf->flags & CILK_FRAME_UNSYNCHED || USE_EXTENSION) {
        if (sf->flags & CILK_FRAME_UNSYNCHED) {
            if (__builtin_setjmp(sf->ctx) == 0) {
                sysdep_save_fp_ctrl_state(sf);
                __cilkrts_sync(sf);
            } else {
                sanitizer_finish_switch_fiber();
                if (sf->flags & CILK_FRAME_EXCEPTION_PENDING) {
                    __cilkrts_check_exception_raise(sf);
                }
            }
        }
        if (USE_EXTENSION) {
            __cilkrts_worker *w = get_worker_from_stack(sf);
            __cilkrts_extend_sync(&w->extension);
        }
    }
}

__attribute__((always_inline)) void
__cilk_sync_nothrow(__cilkrts_stack_frame *sf) {
    if (sf->flags & CILK_FRAME_UNSYNCHED || USE_EXTENSION) {
        if (sf->flags & CILK_FRAME_UNSYNCHED) {
            if (__builtin_setjmp(sf->ctx) == 0) {
                sysdep_save_fp_ctrl_state(sf);
                __cilkrts_sync(sf);
            } else {
                sanitizer_finish_switch_fiber();
            }
        }
        if (USE_EXTENSION) {
            __cilkrts_worker *w = get_worker_from_stack(sf);
            __cilkrts_extend_sync(&w->extension);
        }
    }
}

__attribute__((always_inline)) void
__cilkrts_leave_frame(__cilkrts_stack_frame *sf) {
    // TODO: Move load of worker pointer out of fast path.
    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, "__cilkrts_leave_frame %p", (void *)sf);

    CILK_ASSERT(CHECK_CILK_FRAME_MAGIC(w->g, sf));
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    __cilkrts_stack_frame *parent = sf->call_parent;

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    sf->fh->current_stack_frame = parent;
    sf->call_parent = NULL;

    // Check if sf is the final stack frame, and if so, terminate the Cilkified
    // region.
    uint32_t flags = sf->flags;
    if (flags & CILK_FRAME_LAST) {
        uncilkify(w->g, sf);
        flags = sf->flags;
    }

    if (flags == 0) {
        return;
    }

    CILK_ASSERT(!(flags & CILK_FRAME_DETACHED));

    // A detached frame would never need to call Cilk_set_return, which performs
    // the return protocol of a full frame back to its parent when the full
    // frame is called (not spawned).  A spawned full frame returning is done
    // via a different protocol, which is triggered in Cilk_exception_handler.
    if (flags & CILK_FRAME_STOLEN) { // if this frame has a full frame
        cilkrts_alert(RETURN,
                      "__cilkrts_leave_frame parent is call_parent!");
        // leaving a full frame; need to get the full frame of its call
        // parent back onto the deque
        Cilk_set_return(w);
        CILK_ASSERT(CHECK_CILK_FRAME_MAGIC(w->g, sf));
    }
}

__attribute__((always_inline)) void
__cilkrts_leave_frame_helper(__cilkrts_stack_frame *sf,
                             __cilkrts_stack_frame *parent, bool spawner) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, "__cilkrts_leave_frame_helper %p", (void *)sf);

    CILK_ASSERT(CHECK_CILK_FRAME_MAGIC(w->g, sf));
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    if (spawner)
        sf->fh->current_stack_frame = parent;
    if (USE_EXTENSION) {
        __cilkrts_extend_return_from_spawn(w, &w->extension);
        w->extension = parent->extension;
    }
    sf->call_parent = NULL;

    CILK_ASSERT(sf->flags & CILK_FRAME_DETACHED);

    __cilkrts_stack_frame **tail =
            atomic_load_explicit(&w->tail, memory_order_relaxed);
    --tail;
    /* The store of tail must precede the load of exc in global order.  See
       comment in do_dekker_on. */
    atomic_store_explicit(&w->tail, tail, memory_order_seq_cst);
    __cilkrts_stack_frame **exc =
            atomic_load_explicit(&w->exc, memory_order_seq_cst);
    /* Currently no other modifications of flags are atomic so this one isn't
       either.  If the thief wins it may run in parallel with the clear of
       DETACHED.  Does it modify flags too? */
    sf->flags &= ~CILK_FRAME_DETACHED;
    if (__builtin_expect(exc > tail, false)) {
        Cilk_exception_handler(w, NULL);
        // If Cilk_exception_handler returns this thread won the race and can
        // return to the parent function.
    }
}

__attribute__((always_inline)) void
__cilk_parent_epilogue(__cilkrts_stack_frame *sf) {
    __cilkrts_leave_frame(sf);
}

__attribute__((always_inline)) void
__cilk_helper_epilogue(__cilkrts_stack_frame *sf, __cilkrts_stack_frame *parent,
                       bool spawner) {
    __cilkrts_leave_frame_helper(sf, parent, spawner);
}

__attribute__((always_inline))
void __cilkrts_enter_landingpad(__cilkrts_stack_frame *sf, int32_t sel) {
    if (__cilkrts_need_to_cilkify)
        return;

    sf->fh->current_stack_frame = sf;

    // Don't do anything special during cleanups.
    if (sel == 0)
        return;

    if (0 == __builtin_setjmp(sf->ctx))
        __cilkrts_cleanup_fiber(sf, sel);
}

__attribute__((always_inline)) void
__cilkrts_pause_frame(__cilkrts_stack_frame *sf, __cilkrts_stack_frame *parent,
                      char *exn, bool spawner) {
    if (0 == __builtin_setjmp(sf->ctx))
        __cilkrts_cleanup_fiber(sf, 1);

    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, "__cilkrts_pause_frame %p", (void *)sf);

    CILK_ASSERT(CHECK_CILK_FRAME_MAGIC(w->g, sf));

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    if (spawner)
        sf->fh->current_stack_frame = parent;
    sf->call_parent = NULL;

    // A __cilkrts_pause_frame may be reached before the spawn-helper frame has
    // detached.  In that case, THE is not required.
    if (sf->flags & CILK_FRAME_DETACHED) {
        if (USE_EXTENSION) {
            __cilkrts_extend_return_from_spawn(w, &w->extension);
            w->extension = parent->extension;
        }
        __cilkrts_stack_frame **tail =
            atomic_load_explicit(&w->tail, memory_order_relaxed);
        --tail;
        /* The store of tail must precede the load of exc in global order.
           See comment in do_dekker_on. */
        atomic_store_explicit(&w->tail, tail, memory_order_seq_cst);
        __cilkrts_stack_frame **exc =
            atomic_load_explicit(&w->exc, memory_order_seq_cst);
        /* Currently no other modifications of flags are atomic so this
           one isn't either.  If the thief wins it may run in parallel
           with the clear of DETACHED.  Does it modify flags too? */
        sf->flags &= ~CILK_FRAME_DETACHED;
        if (__builtin_expect(exc > tail, false)) {
            Cilk_exception_handler(w, exn);
            // If Cilk_exception_handler returns this thread won
            // the race and can return to the parent function.
        }
    }
}

__attribute__((always_inline)) void
__cilk_helper_epilogue_exn(__cilkrts_stack_frame *sf,
                           __cilkrts_stack_frame *parent, char *exn,
                           bool spawner) {
    __cilkrts_pause_frame(sf, parent, exn, spawner);
}

// Internal helper function to ensure the __cilkrts_stack_frame type is present
// in the bitcode file.  Does not end up in compiled Cilk code nor the OpenCilk
// runtime library.
CHEETAH_INTERNAL __cilkrts_stack_frame
__internal_preserve_stack_frame_type_helper(void) {
    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame(&sf);
    return sf;
}

/// Computes a grainsize for a cilk_for loop, using the following equation:
///
///     grainsize = min(2048, ceil(n / (8 * nworkers)))
#define __cilkrts_grainsize_fn_impl(NAME, INT_T)                               \
    __attribute__((always_inline)) INT_T NAME(INT_T n) {                       \
        INT_T small_loop_grainsize = n / (8 * __cilkrts_nproc);                \
        if (small_loop_grainsize <= 1)                                         \
            return 1;                                                          \
        INT_T large_loop_grainsize = 2048;                                     \
        return large_loop_grainsize < small_loop_grainsize                     \
                   ? large_loop_grainsize                                      \
                   : small_loop_grainsize;                                     \
    }
#define __cilkrts_grainsize_fn(SZ)                                             \
    __cilkrts_grainsize_fn_impl(__cilkrts_cilk_for_grainsize_##SZ, uint##SZ##_t)

__attribute__((always_inline)) uint8_t
__cilkrts_cilk_for_grainsize_8(uint8_t n) {
    uint8_t small_loop_grainsize = n / (8 * __cilkrts_nproc);
    if (small_loop_grainsize <= 1)
        return 1;
    return small_loop_grainsize;
}

__cilkrts_grainsize_fn(16) __cilkrts_grainsize_fn(32) __cilkrts_grainsize_fn(64)
