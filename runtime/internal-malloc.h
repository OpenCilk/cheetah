#ifndef _INTERAL_MALLOC_H
#define _INTERAL_MALLOC_H

#include <stdint.h>
#include <stdlib.h>

#include "debug.h"
#include "rts-config.h"

CHEETAH_INTERNAL extern int cheetah_page_shift;

#define NUM_BUCKETS 7

#define INTERNAL_MALLOC_STATS CILK_STATS

struct global_im_pool_stats {
    size_t allocated; // bytes allocated into the pool
    size_t wasted;    // bytes at the end of a chunk that could not be used
};

struct im_bucket_stats {
    unsigned int num_free;  // number of free blocks left; computed at terminate
    unsigned int allocated; // number of batch allocated and not freed
    unsigned int max_allocated; // high watermark of batch_allocated
};
#if INTERNAL_MALLOC_STATS
#define WHEN_IM_STATS(ex) ex
#else
#define WHEN_IM_STATS(ex)
#endif

/* struct for managing global memory pool; each memory block in mem_list starts
   out with size INTERMAL_MALLOC_CHUNK.  We will allocate small pieces off the
   memory block and free the pieces into per-worker im_descriptor free list. */
struct global_im_pool {
    char *mem_begin;    // beginning of the free memory block that we are using
    char *mem_end;      // end of the free memory block that we are using
    char **mem_list;    // list of memory blocks obtained from system
    int mem_list_index; // index to the current mem block in use
    int mem_list_size;  // length of the mem_list
    WHEN_IM_STATS(struct global_im_pool_stats stats); // im pool stats
};

struct im_bucket {
    void *free_list;        // beginning of free list
    unsigned int list_size; // length of free list
    unsigned int
        count_until_free; // number of allocations to make on the free list
                          // before calling batch_free (back to the global)
    WHEN_IM_STATS(struct im_bucket_stats stats);
};

struct cilk_im_desc {
    struct im_bucket buckets[NUM_BUCKETS];
    size_t used;
    unsigned long num_malloc;
};

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
cilk_internal_malloc(__cilkrts_worker *w, size_t size);
CHEETAH_INTERNAL void cilk_internal_free(__cilkrts_worker *w, void *p,
                                         size_t size);

#endif // _INTERAL_MALLOC_H
