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
#include "frame.h"
#include "global.h"
#include "init.h"
#include "local-reducer-api.h"
#include "readydeque.h"
#include "scheduler.h"

#include "pedigree_ext.c"

// This variable encodes the alignment of a __cilkrts_stack_frame, both in its
// value and in its own alignment.  Because LLVM IR does not associate
// alignments with types, this variable communicates the desired alignment to
// the compiler instead.
_Alignas(__cilkrts_stack_frame)
size_t __cilkrts_stack_frame_align = __alignof__(__cilkrts_stack_frame);

__attribute__((always_inline)) unsigned __cilkrts_get_nworkers(void) {
    return cilkg_nproc;
}

// Internal method to get the Cilk worker ID.  Intended for debugging purposes.
//
// TODO: Figure out how we want to support worker-local storage.
__attribute__((always_inline))
unsigned __cilkrts_get_worker_number(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (w)
        return w->self;
    // If we're not cilkified, pretend we're worker 0.
    return 0;
}

/* void *__cilkrts_reducer_lookup(void *key, size_t size, */
/*                                __cilk_identity_fn identity, */
/*                                __cilk_reduce_fn reduce) { */
void *__cilkrts_reducer_lookup(void *key, size_t size,
                               void *identity_ptr, void *reduce_ptr) {
    // What should we do when the worker is NULL, meaning we're
    // outside of a cilkified region?  If we're simply looking up the
    // reducer, we could just return the key, since that's the correct
    // view.  But if we're registering the reducer, then we should add
    // the reducer to the table, or else the worker might not find the
    // correct view when it subsequently executes a cilkified region.
    //
    // If we're implicitly registering reducers upon lookup, then we
    // could use a reducer lookup from outside the region to
    // implicitly register that reducer.  But we're not guaranteed to
    // always have a reducer lookup from outside a cilkified region
    // nor a reducer lookup that we can distinguish from a
    // registration (i.e., whether to use the key as the view or
    // create a new view).
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (NULL == w)
        return key;
    struct local_hyper_table *table = get_local_hyper_table(w);
    struct bucket *b = find_hyperobject(table, (uintptr_t)key);
    if (b) {
        CILK_ASSERT(w, key == (void *)b->key);
        // Return the existing view.
        return b->value.view;
    }

    __cilk_identity_fn identity = (__cilk_identity_fn)identity_ptr;
    __cilk_reduce_fn reduce = (__cilk_reduce_fn)reduce_ptr;

    // Create a new view and initialize it with the identity function.
    /* void *new_view = __cilkrts_hyper_alloc(size); */
    void *new_view =
        cilk_aligned_alloc(round_size_to_alignment(64, size), size);
    identity(new_view);
    // Insert the new view into the local hypertable.
    struct bucket new_bucket = {
        .key = (uintptr_t)key,
        .value = {.view = new_view, .reduce_fn = reduce}};
    bool success = insert_hyperobject(table, new_bucket);
    CILK_ASSERT(
        w, success && "__cilkrts_reducer_lookup failed to insert new reducer.");
    (void)success;
    // Return the new view.
    return new_view;
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
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    sf->flags = 0;
    if (NULL == w) {
        cilkify(sf);
        w = __cilkrts_get_tls_worker();
    }
    cilkrts_alert(CFRAME, w, "__cilkrts_enter_frame %p", (void *)sf);

    sf->magic = frame_magic;
    sf->call_parent = w->current_stack_frame;
    atomic_store_explicit(&sf->worker, w, memory_order_relaxed);
    w->current_stack_frame = sf;
    // WHEN_CILK_DEBUG(sf->magic = CILK_STACKFRAME_MAGIC);
}

// Enter a spawn helper, i.e., a fucntion containing code that was cilk_spawn'd.
// This function initializes worker and stack_frame structures.  Because this
// routine will always be executed by a Cilk worker, it is optimized compared to
// its counterpart, __cilkrts_enter_frame.
__attribute__((always_inline)) void
__cilkrts_enter_frame_helper(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    cilkrts_alert(CFRAME, w, "__cilkrts_enter_frame_helper %p", (void *)sf);

    sf->flags = 0;
    sf->magic = frame_magic;
    sf->call_parent = w->current_stack_frame;
    atomic_store_explicit(&sf->worker, w, memory_order_relaxed);
    w->current_stack_frame = sf;
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

static inline
__cilkrts_worker *get_worker_from_stack(__cilkrts_stack_frame *sf) {
    // In principle, we should be able to get the worker efficiently by calling
    // __cilkrts_get_tls_worker().  But code-generation on many systems assumes
    // that the thread on which a function runs never changes.  As a result, it
    // may cache the address returned by __cilkrts_get_tls_worker() during
    // enter_frame and load the cached value in later, even though the actual
    // result of __cilkrts_get_tls_worker() may change between those two points.
    // To avoid this buggy behavior, we therefore get the worker from sf.
    //
    // TODO: Fix code-generation of TLS lookups on these systems.
    return atomic_load_explicit(&sf->worker, memory_order_relaxed);
}

// Detach the given Cilk stack frame, allowing other Cilk workers to steal the
// parent frame.
__attribute__((always_inline)) void
__cilkrts_detach(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_detach %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, w->current_stack_frame == sf);

    struct __cilkrts_stack_frame *parent = sf->call_parent;

    if (USE_EXTENSION) {
        __cilkrts_extend_spawn(w, &parent->extension, &w->extension);
    }

    sf->flags |= CILK_FRAME_DETACHED;
    struct __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    CILK_ASSERT(w, (tail + 1) < w->ltq_limit);

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
    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_leave_frame %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    __cilkrts_stack_frame *parent = sf->call_parent;

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    w->current_stack_frame = parent;
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

    CILK_ASSERT(w, !(flags & CILK_FRAME_DETACHED));

    // A detached frame would never need to call Cilk_set_return, which performs
    // the return protocol of a full frame back to its parent when the full
    // frame is called (not spawned).  A spawned full frame returning is done
    // via a different protocol, which is triggered in Cilk_exception_handler.
    if (flags & CILK_FRAME_STOLEN) { // if this frame has a full frame
        cilkrts_alert(RETURN, w,
                      "__cilkrts_leave_frame parent is call_parent!");
        // leaving a full frame; need to get the full frame of its call
        // parent back onto the deque
        Cilk_set_return(w);
        CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    }
}

__attribute__((always_inline)) void
__cilkrts_leave_frame_helper(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_leave_frame_helper %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    __cilkrts_stack_frame *parent = sf->call_parent;
    w->current_stack_frame = parent;
    if (USE_EXTENSION) {
        __cilkrts_extend_return_from_spawn(w, &w->extension);
        w->extension = parent->extension;
    }
    sf->call_parent = NULL;

    CILK_ASSERT(w, sf->flags & CILK_FRAME_DETACHED);

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
    if (__builtin_expect(exc > tail, 0)) {
        Cilk_exception_handler(NULL);
        // If Cilk_exception_handler returns this thread won the race and can
        // return to the parent function.
    }
    // CILK_ASSERT(w, *(w->tail) == w->current_stack_frame);
}

__attribute__((always_inline)) void
__cilk_parent_epilogue(__cilkrts_stack_frame *sf) {
    __cilkrts_leave_frame(sf);
}

__attribute__((always_inline)) void
__cilk_helper_epilogue(__cilkrts_stack_frame *sf) {
    __cilkrts_leave_frame_helper(sf);
}

__attribute__((always_inline))
void __cilkrts_enter_landingpad(__cilkrts_stack_frame *sf, int32_t sel) {
    // Don't do anything special during cleanups.
    if (sel == 0)
        return;

    if (0 == __builtin_setjmp(sf->ctx))
        __cilkrts_cleanup_fiber(sf, sel);
}

__attribute__((always_inline))
void __cilkrts_pause_frame(__cilkrts_stack_frame *sf, char *exn) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    cilkrts_alert(CFRAME, w, "__cilkrts_pause_frame %p", (void *)sf);

    CILK_ASSERT(w, CHECK_CILK_FRAME_MAGIC(w->g, sf));
    CILK_ASSERT(w, sf->worker == __cilkrts_get_tls_worker());

    __cilkrts_stack_frame *parent = sf->call_parent;

    // Pop this frame off the cactus stack.  This logic used to be in
    // __cilkrts_pop_frame, but has been manually inlined to avoid reloading the
    // worker unnecessarily.
    w->current_stack_frame = parent;
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
        if (__builtin_expect(exc > tail, 0)) {
            Cilk_exception_handler(exn);
            // If Cilk_exception_handler returns this thread won
            // the race and can return to the parent function.
        }
        // CILK_ASSERT(w, *(w->tail) == w->current_stack_frame);
    }
}

__attribute__((always_inline)) void
__cilk_helper_epilogue_exn(__cilkrts_stack_frame *sf, char *exn) {
    __cilkrts_pause_frame(sf, exn);
}

/// Computes a grainsize for a cilk_for loop, using the following equation:
///
///     grainsize = min(2048, ceil(n / (8 * nworkers)))
#define __cilkrts_grainsize_fn_impl(NAME, INT_T)                               \
    __attribute__((always_inline)) INT_T NAME(INT_T n) {                       \
        INT_T small_loop_grainsize = n / (8 * cilkg_nproc);                    \
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
    uint8_t small_loop_grainsize = n / (8 * cilkg_nproc);
    if (small_loop_grainsize <= 1)
        return 1;
    return small_loop_grainsize;
}

__cilkrts_grainsize_fn(16) __cilkrts_grainsize_fn(32) __cilkrts_grainsize_fn(64)
