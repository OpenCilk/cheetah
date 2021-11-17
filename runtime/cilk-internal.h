#ifndef _CILK_INTERNAL_H
#define _CILK_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include <cilk/cilk_api.h>

#include "debug.h"
#include "fiber.h"
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
CHEETAH_INTERNAL extern __thread bool is_boss_thread;

static inline __attribute__((always_inline)) __cilkrts_worker *
__cilkrts_get_tls_worker(void) {
    return __cilkrts_tls_worker;
}

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

#ifdef __cplusplus
}
#endif

#endif // _CILK_INTERNAL_H
