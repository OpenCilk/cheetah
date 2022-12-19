#ifndef _HYPEROBJECT_BASE
#define _HYPEROBJECT_BASE

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <cilk/cilk_api.h>

// Reducer data.
//
// NOTE: Since the size and identity_fn are only used when a worker
// looks up a reducer after a steal, we don't need to store these in
// the reducer_base structure as long as the reducer_lookup function
// gets them as parameters.
//
// TODO: For small reducer views of size less than sizeof(void *),
// consider storing the view directly within the reducer_base
// structure.
// - Problem: A reducer_base structure may move around in the hash
//   table as other reducers are inserted.  As a result, a pointer to
//   a view may be invalidated by other hyper_lookup operations.
// - Problem: Need a way to keep track of whether the view in a
//   reducer_base is storing a pointer to the view or the view itself.
typedef struct reducer_base {
    void *view;
    /* size_t view_size; */
    __cilk_reduce_fn reduce_fn;
    /* __cilk_identity_fn identity_fn; */
} reducer_base;

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
