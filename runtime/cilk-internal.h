#ifndef _CILK_INTERNAL_H
#define _CILK_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include <cilk/cilk_api.h>

#include "debug.h"
#include "fiber-header.h"
#include "frame.h"
#include "internal-malloc.h"
#include "jmpbuf.h"
#include "rts-config.h"
#include "sched_stats.h"
#include "types.h"
#include "worker.h"

#if defined __i386__ || defined __x86_64__
#ifdef __SSE__
#define CHEETAH_SAVE_MXCSR
#endif
#endif

struct global_state;
typedef struct global_state global_state;
typedef struct local_state local_state;

struct cilkrts_callbacks {
    unsigned last_init;
    unsigned last_exit;
    bool after_init;
    void (*init[MAX_CALLBACKS])(void);
    void (*exit[MAX_CALLBACKS])(void);
};

extern CHEETAH_INTERNAL struct cilkrts_callbacks cilkrts_callbacks;

extern bool __cilkrts_use_extension;
#if ENABLE_EXTENSION
#define USE_EXTENSION __cilkrts_use_extension
#else
#define USE_EXTENSION false
#endif
extern __thread __cilkrts_worker *__cilkrts_tls_worker;
extern __thread struct cilk_fiber *__cilkrts_current_fh;
extern bool __cilkrts_need_to_cilkify;

static inline __attribute__((always_inline)) __cilkrts_worker *
__cilkrts_get_tls_worker(void) {
    return __cilkrts_tls_worker;
}

static inline __attribute__((always_inline)) __cilkrts_worker *
get_worker_from_stack(const __cilkrts_stack_frame *sf) {
    // Although we can get the current worker by calling
    // __cilkrts_get_tls_worker(), that method accesses the worker via TLS,
    // which can be slow on some systems.  This method gets the current worker
    // from the given __cilkrts_stack_frame, which is more efficient than a TLS
    // access on those systems.
    return sf->fh->worker;
}

CHEETAH_INTERNAL
void *internal_reducer_lookup(__cilkrts_worker *w, void *key, size_t size,
                              void *identity_ptr, void *reduce_ptr);
CHEETAH_INTERNAL
void internal_reducer_remove(__cilkrts_worker *w, void *key);

void __cilkrts_register_extension(void *extension);
void *__cilkrts_get_extension(void);
void __cilkrts_extend_spawn(__cilkrts_worker *w, void **parent_extension,
                            void **child_extension);
void __cilkrts_extend_return_from_spawn(__cilkrts_worker *w, void **extension);
void __cilkrts_extend_sync(void **extension);

static inline __attribute__((always_inline)) void *
__cilkrts_push_ext_stack(__cilkrts_worker *w, size_t size) {
    uint8_t *ext_stack_ptr = ((uint8_t *)w->ext_stack) - size;
    w->ext_stack = (void *)ext_stack_ptr;
    return ext_stack_ptr;
}

static inline __attribute__((always_inline)) void *
__cilkrts_pop_ext_stack(__cilkrts_worker *w, size_t size) {
    uint8_t *ext_stack_ptr = ((uint8_t *)w->ext_stack) + size;
    w->ext_stack = (void *)ext_stack_ptr;
    return ext_stack_ptr;
}

/*
 * All the data needed to properly handle a thrown exception.
 */
struct closure_exception {
    char *exn;
    /* Canonical frame address (CFA) of the call-stack frame from which an
       exception was rethrown.  Used to ensure that the rethrown exception
       appears to be rethrown from the correct frame and to avoid repeated calls
       to __cilkrts_leave_frame during stack unwinding. */
    char *reraise_cfa;
    /* Stack pointer for the parent fiber.  Used to restore the stack pointer
       properly after entering a landingpad. */
    char *parent_rsp;
    /* Fiber holding the stack frame of a call to _Unwind_RaiseException that is
       currently running. */
    struct cilk_fiber *throwing_fiber;
};

#ifdef __cplusplus
}
#endif

#endif // _CILK_INTERNAL_H
