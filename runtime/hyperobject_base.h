#ifndef _HYPEROBJECT_BASE
#define _HYPEROBJECT_BASE

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <cilk/cilk_api.h>

typedef uint32_t hyper_id_t;

struct hyperobject_base;

typedef struct hyperobject_base {
    __cilk_identity_fn identity_fn;
    __cilk_reduce_fn   reduce_fn;
    size_t             view_size; // rounded to CACHE_LINE
    hyper_id_t         id_num;
    int                valid;
    void               *key;
    /* 3 words left in cache line */
} hyperobject_base;

// This needs to be exported so cilksan can preempt it.
__attribute__((weak)) void *__cilkrts_hyper_alloc(size_t size);
// This needs to be exported so cilksan can preempt it.
__attribute__((weak)) void __cilkrts_hyper_dealloc(void *view, size_t size);
CHEETAH_INTERNAL
void cilkrts_hyper_register(hyperobject_base *hyper);
CHEETAH_INTERNAL
void cilkrts_hyper_unregister(hyperobject_base *hyper);
CHEETAH_INTERNAL
void *cilkrts_hyper_lookup(hyperobject_base *key);

#endif /* _CILK_HYPEROBJECT_BASE */
