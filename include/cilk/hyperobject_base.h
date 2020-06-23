#ifndef _CILK_HYPEROBJECT_BASE
#define _CILK_HYPEROBJECT_BASE

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback function signatures.  The first argument always points to the
 * reducer itself and is commonly ignored. */
typedef void (*cilk_reduce_fn_t)(void *r, void *lhs, void *rhs);
typedef void (*cilk_identity_fn_t)(void *r, void *view);
typedef void (*cilk_destroy_fn_t)(void *r, void *view);
typedef void *(*cilk_allocate_fn_t)(void *r, size_t bytes);
typedef void (*cilk_deallocate_fn_t)(void *r, void *view);

/** Representation of the monoid */
typedef struct cilk_c_monoid {
    cilk_reduce_fn_t reduce_fn;
    cilk_identity_fn_t identity_fn;
    cilk_destroy_fn_t destroy_fn;
    cilk_allocate_fn_t allocate_fn;
    cilk_deallocate_fn_t deallocate_fn;
} cilk_c_monoid;

/** Base of the hyperobject */
typedef struct __cilkrts_hyperobject_base {
    cilk_c_monoid __c_monoid;
    uint32_t __id_num;      /* for runtime use only, initialize to 0 */
    uint32_t __view_offset; /* offset (in bytes) to leftmost view */
    size_t __view_size;     /* Size of each view */
} __cilkrts_hyperobject_base;

/* Library interface.
   TODO: Add optimization hints like "strand pure" as in Cilk Plus. */
void __cilkrts_hyper_create(__cilkrts_hyperobject_base *key);
void __cilkrts_hyper_destroy(__cilkrts_hyperobject_base *key);
void *__cilkrts_hyper_lookup(__cilkrts_hyperobject_base *key);
void *__cilkrts_hyper_alloc(void *ignore, size_t bytes);
void __cilkrts_hyper_dealloc(void *ignore, void *view);

#ifdef __cplusplus
} /* end extern "C" */
#endif

#endif /* _CILK_HYPEROBJECT_BASE */
