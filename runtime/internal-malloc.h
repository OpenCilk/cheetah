#ifndef _INTERAL_MALLOC_H
#define _INTERAL_MALLOC_H

#include <stdint.h>
#include <stdlib.h>

#include "rts-config.h"

CHEETAH_INTERNAL extern int cheetah_page_shift;

enum im_tag {
    IM_UNCLASSIFIED,
    IM_CLOSURE,
    IM_FIBER,
    IM_REDUCER_MAP,
    IM_NUM_TAGS
};

CHEETAH_INTERNAL const char *name_for_im_tag(enum im_tag);

/* Custom implementation of aligned_alloc. */
static inline void *cilk_aligned_alloc(size_t alignment, size_t size) {
#if defined(_ISOC11_SOURCE)
    return aligned_alloc(alignment, size);
#else
    void *ptr;
    posix_memalign(&ptr, alignment, size);
    return ptr;
#endif
}

// public functions (external to source file, internal to library)
CHEETAH_INTERNAL void cilk_internal_malloc_global_init(struct global_state *g);
CHEETAH_INTERNAL void internal_malloc_global_check(global_state *g);
CHEETAH_INTERNAL void
cilk_internal_malloc_global_terminate(struct global_state *g);
CHEETAH_INTERNAL void
cilk_internal_malloc_global_destroy(struct global_state *g);
CHEETAH_INTERNAL void cilk_internal_malloc_per_worker_init(__cilkrts_worker *w);
CHEETAH_INTERNAL void
cilk_internal_malloc_per_worker_destroy(__cilkrts_worker *w);
CHEETAH_INTERNAL void
cilk_internal_malloc_per_worker_terminate(__cilkrts_worker *w);
__attribute__((alloc_size(2), assume_aligned(32), malloc))
CHEETAH_INTERNAL void *
cilk_internal_malloc(__cilkrts_worker *w, size_t size, enum im_tag tag);
CHEETAH_INTERNAL void cilk_internal_free(__cilkrts_worker *w, void *p,
                                         size_t size, enum im_tag tag);
/* Release memory to the global pool after workers have stopped. */
CHEETAH_INTERNAL void cilk_internal_free_global(struct global_state *, void *p,
                                                size_t size, enum im_tag tag);

#endif // _INTERAL_MALLOC_H
