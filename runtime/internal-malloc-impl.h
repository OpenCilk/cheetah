#ifndef _INTERAL_MALLOC_IMPL_H
#define _INTERAL_MALLOC_IMPL_H

#include "debug.h"
#include "rts-config.h"

#include "internal-malloc.h"

#define NUM_BUCKETS 7
#define NUM_IM_CALLERS 4

/* struct for managing global memory pool; each memory block in mem_list starts
   out with size INTERNAL_MALLOC_CHUNK.  We will allocate small pieces off the
   memory block and free the pieces into per-worker im_descriptor free list. */
struct global_im_pool {
    char *mem_begin; // beginning of the free memory block that we are using
    char *mem_end;   // end of the free memory block that we are using
    char **mem_list; // list of memory blocks obtained from system
    unsigned mem_list_index; // index to the current mem block in use
    unsigned mem_list_size;  // length of the mem_list
    size_t num_global_malloc;
    size_t allocated; // bytes allocated into the pool
    size_t wasted;    // bytes at the end of a chunk that could not be used
};

struct im_bucket {
    void *free_list;          // beginning of free list
    unsigned free_list_size;  // Current size of free list
    unsigned free_list_limit; // Maximum allowed size of free list
    // Allocation count and wasted space on a worker may be negative
    // if it frees blocks allocated elsewhere.  In a global bucket
    // these fields should never be negative.
    int allocated;     // Current allocations, in use or free
    int max_allocated; // high watermark of allocated
    long wasted;       // in bytes
};

/* One of these per worker, and one global */
struct cilk_im_desc {
    struct im_bucket buckets[NUM_BUCKETS];
    long used; // local alloc - local free, may be negative
    long num_malloc[IM_NUM_TAGS];
};

#endif /* _INTERAL_MALLOC_IMPL_H */
