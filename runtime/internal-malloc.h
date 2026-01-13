#ifndef _INTERAL_MALLOC_H
#define _INTERAL_MALLOC_H

#include "rts-config.h"
#include <cstdlib>

struct __cilkrts_worker;
struct global_state;

CHEETAH_INTERNAL extern int cheetah_page_shift;

enum im_tag {
    IM_UNCLASSIFIED,
    IM_CLOSURE,
    IM_FIBER,
    IM_REDUCER_MAP,
    IM_NUM_TAGS
};

CHEETAH_INTERNAL const char *name_for_im_tag(enum im_tag);

/* Helper routine to round sizes to alignments, for use with cilk_aligned_alloc.
 */
static inline size_t round_size_to_alignment(size_t alignment, size_t size) {
    return (size + alignment - 1) & -alignment;
}

/* Custom implementation of aligned_alloc. */
static inline void *cilk_aligned_alloc(size_t alignment, size_t size) {
#if (defined(__linux__) && (__STDC_VERSION__ >= 201112L)) ||                   \
    defined(_ISOC11_SOURCE) || __FreeBSD__ >= 10 ||                            \
    __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 101500
    return aligned_alloc(alignment, size);
#else
    void *ptr;
    if (posix_memalign(&ptr, alignment, size) == 0)
        return ptr;
    return nullptr;
#endif
}

// public functions (external to source file, internal to library)
CHEETAH_INTERNAL void cilk_internal_malloc_global_init(global_state *g);
CHEETAH_INTERNAL void internal_malloc_global_check(global_state *g);
CHEETAH_INTERNAL void cilk_internal_malloc_global_terminate(global_state *g);
CHEETAH_INTERNAL void cilk_internal_malloc_global_destroy(global_state *g);
CHEETAH_INTERNAL void cilk_internal_malloc_per_worker_init(__cilkrts_worker *w);
CHEETAH_INTERNAL void
cilk_internal_malloc_per_worker_destroy(__cilkrts_worker *w);
CHEETAH_INTERNAL void
cilk_internal_malloc_per_worker_terminate(__cilkrts_worker *w);
__attribute__((alloc_size(2), assume_aligned(32),
               malloc)) CHEETAH_INTERNAL void *
cilk_internal_malloc(__cilkrts_worker *w, size_t size, im_tag tag);
CHEETAH_INTERNAL void cilk_internal_free(__cilkrts_worker *w, void *p,
                                         size_t size, im_tag tag);
/* Release memory to the global pool after workers have stopped. */
CHEETAH_INTERNAL void cilk_internal_free_global(global_state *, void *p,
                                                size_t size, im_tag tag);

#endif // _INTERAL_MALLOC_H
