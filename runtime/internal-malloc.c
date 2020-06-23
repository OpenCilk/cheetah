#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h> /* ffs */
#include <sys/mman.h>
#include <unistd.h> /* sysconf */

#include "cilk-internal.h"
#include "debug.h"
#include "global.h"

CHEETAH_INTERNAL int cheetah_page_shift = 0;

#define MEM_LIST_SIZE 8
#define INTERNAL_MALLOC_CHUNK_SIZE (32 * 1024)
#define SIZE_THRESH bucket_sizes[NUM_BUCKETS - 1]

static const unsigned int bucket_sizes[NUM_BUCKETS] = {32,  64,   128, 256,
                                                       512, 1024, 2048};
static const unsigned int bucket_capacity[NUM_BUCKETS] = {
    256, /*   32 bytes a piece; 2 pages */
    128, /*   64 bytes a piece; 2 pages */
    64,  /*  128 bytes a piece; 2 pages */
    64,  /*  256 bytes a piece; 4 pages */
    32,  /*  512 bytes a piece; 4 pages */
    16,  /* 1024 bytes a piece; 4 pages */
    8    /* 2048 bytes a piece; 4 pages */
};

struct free_block {
    void *next;
};

//=========================================================
// Private helper functions
//=========================================================

static inline int is_page_aligned(size_t size) {
    size_t mask = ((size_t)1 << cheetah_page_shift) - 1;
    return ((size & mask) == 0);
}

static inline unsigned int size_to_bucket(size_t size) {
    for (unsigned int i = 0; i < NUM_BUCKETS; i++) {
        if (size <= bucket_sizes[i]) {
            return i;
        }
    }
    return -1; /* = infinity */
}

static inline unsigned int bucket_to_size(int which_bucket) {
    return bucket_sizes[which_bucket];
}

#if CILK_DEBUG || INTERNAL_MALLOC_STATS // used in these cases only
/* compute the length of a free list starting at pointer p */
static unsigned int free_list_length(void *p) {
    unsigned int count = 0;
    while (p) {
        count++;
        // next pointer is stored at the first 8 bytes
        p = ((struct free_block *)p)->next;
    }
    return count;
}
#endif

#if INTERNAL_MALLOC_STATS
static inline void init_im_bucket_stats(struct im_bucket_stats *s) {
    s->num_free = 0;
    s->allocated = 0;
    s->max_allocated = 0;
}
#else
#define init_im_bucket_stats(s)
#endif

/* initialize the buckets in struct cilk_im_desc */
static void init_im_buckets(struct cilk_im_desc *im_desc) {
    for (int i = 0; i < NUM_BUCKETS; i++) {
        struct im_bucket *bucket = &(im_desc->buckets[i]);
        bucket->free_list = NULL;
        bucket->count_until_free = bucket_capacity[i];
        init_im_bucket_stats(&bucket->stats);
    }
}

//=========================================================
// Private helper functions for debugging
//=========================================================

#if CILK_DEBUG
CHEETAH_INTERNAL
void internal_malloc_global_check(global_state *g) {

    struct cilk_im_desc *d = &(g->im_desc);
    size_t total_size = d->used;
    size_t total_malloc = d->num_malloc;

    for (unsigned int i = 0; i < g->options.nproc; i++) {
        d = &(g->workers[i]->l->im_desc);
        total_size += d->used;
        total_malloc += d->num_malloc;
    }

    // these fields must add up to 0, as they keep track of sizes and number of
    // malloc / frees going out of / into the global pool / per-worker pool.
    // Anything batch-freed into per-worker pool had to come from the global
    // pool; similarly, anything batch-allocated out of the per-worker pool gets
    // freed into the global one

    CILK_CHECK(g, (total_size == 0) && (total_malloc == 0),
               "Possible memory leak detected");
}

#else
#define internal_malloc_global_check(g)
#endif // CILK_DEBUG

//=========================================================
// Private helper functions for IM stats
//=========================================================

#if INTERNAL_MALLOC_STATS
static void init_global_im_pool_stats(struct global_im_pool_stats *stats) {
    stats->allocated = 0;
    stats->wasted = 0;
}

static void print_im_buckets_stats(struct global_state *g) {

#define HDR_DESC "%15s"
#define WORKER_HDR_DESC "%10s %3u:" // two char short compared to HDR_DESC
#define FIELD_DESC "%10zu"
    fprintf(stderr, "\nBYTES IN FREE LISTS:\n");
    fprintf(stderr, HDR_DESC, "Bucket size:");
    for (int j = 0; j < NUM_BUCKETS; j++) {
        fprintf(stderr, FIELD_DESC, (size_t)bucket_sizes[j]);
    }
    fprintf(stderr, "\n-------------------------------------------"
                    "---------------------------------------------\n");

    fprintf(stderr, HDR_DESC, "Global:");
    for (unsigned int j = 0; j < NUM_BUCKETS; j++) {
        struct im_bucket_stats *s = &(g->im_desc.buckets[j].stats);
        fprintf(stderr, FIELD_DESC, (size_t_t)s->num_free * bucket_sizes[j]);
    }
    fprintf(stderr, "\n");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        __cilkrts_worker *w = g->workers[i];
        fprintf(stderr, WORKER_HDR_DESC, "Worker", w->self);
        for (unsigned int j = 0; j < NUM_BUCKETS; j++) {
            struct im_bucket_stats *s = &(w->l->im_desc.buckets[j].stats);
            fprintf(stderr, FIELD_DESC, (size_t)s->num_free * bucket_sizes[j]);
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "\nHIGH WATERMARK FOR BYTES ALLOCATED:\n");
    fprintf(stderr, HDR_DESC, "Bucket size:");
    for (int j = 0; j < NUM_BUCKETS; j++) {
        fprintf(stderr, FIELD_DESC, (size_t)bucket_sizes[j]);
    }
    fprintf(stderr, "\n-------------------------------------------"
                    "---------------------------------------------\n");

    for (unsigned int i = 0; i < g->options.nproc; i++) {
        __cilkrts_worker *w = g->workers[i];
        fprintf(stderr, WORKER_HDR_DESC, "Worker", w->self);
        for (unsigned int j = 0; j < NUM_BUCKETS; j++) {
            struct im_bucket_stats *s = &(w->l->im_desc.buckets[j].stats);
            fprintf(stderr, FIELD_DESC,
                    (size_t)s->max_allocated * bucket_sizes[j]);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
}

static void print_global_im_pool_stats(struct global_im_pool_stats *stats) {
    fprintf(stderr,
            "Total bytes allocated from system: %7zu KBytes (%zu pages)\n",
            stats->allocated / 1024,
            (stats->allocated + PAGE_SIZE - 1) / PAGE_SIZE);
    fprintf(stderr, "Total bytes allocated but wasted:  %7zu KBytes\n",
            stats->wasted / 1024);
}

static void print_internal_malloc_stats(struct global_state *g) {
    fprintf(stderr, "\nINTERNAL MALLOC STATS\n");
    print_global_im_pool_stats(&(g->im_pool.stats));
    print_im_buckets_stats(g);
    fprintf(stderr, "\n");
}
#endif // INTERNAL_MALLOC_STATS

//=========================================================
// Global memory allocator
//=========================================================

static char *malloc_from_system(__cilkrts_worker *w, size_t size) {
    void *mem;
    if (is_page_aligned(size)) {
        mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    } else {
        mem = malloc(size);
    }
    CILK_CHECK(w->g, mem, "Internal malloc running out of memory!");
    return mem;
}

static void free_to_system(void *p, size_t size) {
    if (is_page_aligned(size)) {
        munmap(p, size);
    } else {
        free(p);
    }
}

/**
 * Extend the global im pool.  This function is only called when the
 * current chunk in use is not big enough to satisfy an allocation.
 * The size is already canonicalized at this point.
 */
static void extend_global_pool(__cilkrts_worker *w) {

    struct global_im_pool *im_pool = &(w->g->im_pool);
    im_pool->mem_begin = malloc_from_system(w, INTERNAL_MALLOC_CHUNK_SIZE);
    im_pool->mem_end = im_pool->mem_begin + INTERNAL_MALLOC_CHUNK_SIZE;
#if INTERNAL_MALLOC_STATS
    im_pool->stats.allocated += INTERNAL_MALLOC_CHUNK_SIZE;
#endif
    im_pool->mem_list_index++;

    if (im_pool->mem_list_index >= im_pool->mem_list_size) {
        size_t new_list_size = im_pool->mem_list_size + MEM_LIST_SIZE;
        im_pool->mem_list = realloc(im_pool->mem_list,
                                    new_list_size * sizeof(*im_pool->mem_list));
        im_pool->mem_list_size = new_list_size;
        CILK_CHECK(w->g, im_pool->mem_list,
                   "Interal malloc running out of memory!");
    }
    im_pool->mem_list[im_pool->mem_list_index] = im_pool->mem_begin;
}

/**
 * Allocate a piece of memory of 'size' from global im bucket 'bucket'.
 * The free_list is last-in-first-out.
 * The size is already canonicalized at this point.
 */
static void *global_im_alloc(__cilkrts_worker *w, size_t size,
                             unsigned int which_bucket) {

    CILK_ASSERT(w, w->g);
    CILK_ASSERT(w, size <= SIZE_THRESH);
    CILK_ASSERT(w, which_bucket < NUM_BUCKETS);

    struct im_bucket *bucket = &(w->g->im_desc.buckets[which_bucket]);
    void *mem = bucket->free_list;

    WHEN_CILK_DEBUG({ // stats only kept track during debugging
        struct cilk_im_desc *im_desc = &(w->g->im_desc);
        im_desc->used += size;
        im_desc->num_malloc++;
    });
    // look at the global free list for this bucket
    if (mem) {
        bucket->free_list = ((struct free_block *)mem)->next;
        bucket->count_until_free++;
    } else {
        struct global_im_pool *im_pool = &(w->g->im_pool);
        // allocate from the global pool
        if ((im_pool->mem_begin + size) > im_pool->mem_end) {
#if INTERNAL_MALLOC_STATS
            // consider the left over as waste for now
            im_pool->stats.wasted += im_pool->mem_end - im_pool->mem_begin;
#endif
            extend_global_pool(w);
        }
        mem = im_pool->mem_begin;
        im_pool->mem_begin += size;
    }

    return mem;
}

/**
 * Free a piece of memory of 'size' back to global im bucket 'bucket'.
 * The free_list is last-in-first-out.
 * The size is already canonicalized at this point.
 */
static void global_im_free(__cilkrts_worker *w, void *p, size_t size,
                           unsigned int which_bucket) {

    CILK_ASSERT(w, w->g);
    CILK_ASSERT(w, size <= SIZE_THRESH);
    CILK_ASSERT(w, which_bucket < NUM_BUCKETS);
    USE_UNUSED(size);

    WHEN_CILK_DEBUG({ // stats only kept track during debugging
        struct cilk_im_desc *im_desc = &(w->g->im_desc);
        im_desc->used -= size;
        im_desc->num_malloc--;
    });
    struct im_bucket *bucket = &(w->g->im_desc.buckets[which_bucket]);
    void *next = bucket->free_list;
    ((struct free_block *)p)->next = next;
    bucket->free_list = p;
    bucket->count_until_free--;
}

static void global_im_pool_destroy(struct global_im_pool *im_pool) {

    for (int i = 0; i < im_pool->mem_list_size; i++) {
        void *mem = im_pool->mem_list[i];
        free_to_system(mem, INTERNAL_MALLOC_CHUNK_SIZE);
        im_pool->mem_list[i] = NULL;
    }
    free(im_pool->mem_list);
    im_pool->mem_list = NULL;
    im_pool->mem_begin = im_pool->mem_end = NULL;
    im_pool->mem_list_index = -1;
    im_pool->mem_list_size = 0;
}

void cilk_internal_malloc_global_init(global_state *g) {
    if (cheetah_page_shift == 0) {
        long cheetah_page_size = sysconf(_SC_PAGESIZE);
        /* The global store here should be atomic. */
        cheetah_page_shift = ffs(cheetah_page_size) - 1;
        CILK_ASSERT_G((1 << cheetah_page_shift) == cheetah_page_size);
    }
    cilk_mutex_init(&(g->im_lock));
    g->im_pool.mem_begin = g->im_pool.mem_end = NULL;
    g->im_pool.mem_list_index = -1;
    g->im_pool.mem_list_size = MEM_LIST_SIZE;
    g->im_pool.mem_list = malloc(MEM_LIST_SIZE * sizeof(*g->im_pool.mem_list));
    CILK_CHECK(g, g->im_pool.mem_list, "Cannot allocate mem_list");
    init_im_buckets(&g->im_desc);
    WHEN_IM_STATS(init_global_im_pool_stats(&(g->im_pool.stats)));
    WHEN_CILK_DEBUG(g->im_desc.used = 0);
    WHEN_CILK_DEBUG(g->im_desc.num_malloc = 0);
}

void cilk_internal_malloc_global_terminate(global_state *g) {
#if INTERNAL_MALLOC_STATS
    for (unsigned int i = 0; i < NUM_BUCKETS; i++) {
        struct im_bucket *b = &(g->im_desc.buckets[i]);
        b->stats.num_free = free_list_length(b->free_list);
    }
    print_internal_malloc_stats(g);
#endif
}

void cilk_internal_malloc_global_destroy(global_state *g) {
    global_im_pool_destroy(&(g->im_pool)); // free global mem blocks
    cilk_mutex_destroy(&(g->im_lock));
}

//=========================================================
// Per-worker memory allocator
//=========================================================

/**
 * Allocate a batch of memory of size 'size' from global im bucket 'bucket'
 * into per-worker im bucket 'bucket'.
 */
static void im_allocate_batch(__cilkrts_worker *w, size_t size,
                              unsigned int bucket) {

    unsigned int batch_size = bucket_capacity[bucket] / 2;
    cilk_mutex_lock(&(w->g->im_lock));
    for (unsigned int i = 0; i < batch_size; i++) {
        void *p = global_im_alloc(w, size, bucket);
        cilk_internal_free(w, p, size);
    }
    cilk_mutex_unlock(&(w->g->im_lock));
#if INTERNAL_MALLOC_STATS
    struct im_bucket_stats *s = &(w->l->im_desc.buckets[bucket].stats);
    s->allocated += batch_size;
    if (s->allocated > s->max_allocated) {
        s->max_allocated = s->allocated;
    }
#endif
}

/**
 * Free a batch of memory of size 'size' from per-worker im bucket 'bucket'
 * back to global im bucket 'bucket'.
 */
static void im_free_batch(__cilkrts_worker *w, size_t size,
                          unsigned int bucket) {

    unsigned int batch_size = bucket_capacity[bucket] / 2;
    cilk_mutex_lock(&(w->g->im_lock));
    for (unsigned int i = 0; i < batch_size; i++) {
        void *p = cilk_internal_malloc(w, size);
        global_im_free(w, p, size, bucket);
    }
    cilk_mutex_unlock(&(w->g->im_lock));
#if INTERNAL_MALLOC_STATS
    struct im_bucket_stats *s = &(w->l->im_desc.buckets[bucket].stats);
    s->allocated -= batch_size;
#endif
}

/*
 * Malloc returns a piece of memory at the head of the free list;
 * last-in-first-out
 */
CHEETAH_INTERNAL
void *cilk_internal_malloc(__cilkrts_worker *w, size_t size) {

    WHEN_CILK_DEBUG(w->l->im_desc.used += size);
    WHEN_CILK_DEBUG(w->l->im_desc.num_malloc += 1);

    if (size >= SIZE_THRESH) {
        return malloc_from_system(w, size);
    }

    unsigned int which_bucket = size_to_bucket(size);
    CILK_ASSERT(w, which_bucket >= 0 && which_bucket < NUM_BUCKETS);
    unsigned int csize = bucket_to_size(which_bucket); // canonicalize the size
    struct im_bucket *bucket = &(w->l->im_desc.buckets[which_bucket]);
    void *mem = bucket->free_list;

    if (!mem) { // when out of memory, allocate a batch from global pool
        im_allocate_batch(w, csize, which_bucket);
        mem = bucket->free_list;
    }

    /* if there is a block in the free list */
    CILK_ASSERT(w, mem);
    bucket->free_list = ((struct free_block *)mem)->next;
    bucket->count_until_free++;

    return mem;
}

/*
 * Free simply returns to the free list; last-in-first-out
 */
void cilk_internal_free(__cilkrts_worker *w, void *p, size_t size) {

    WHEN_CILK_DEBUG(w->l->im_desc.used -= size);
    WHEN_CILK_DEBUG(w->l->im_desc.num_malloc -= 1);

    if (size > SIZE_THRESH) {
        free_to_system(p, size);
        return;
    }

    unsigned int which_bucket = size_to_bucket(size);
    CILK_ASSERT(w, which_bucket >= 0 && which_bucket < NUM_BUCKETS);
    unsigned int csize = bucket_to_size(which_bucket); // canonicalize the size
    struct im_bucket *bucket = &(w->l->im_desc.buckets[which_bucket]);

    while (bucket->count_until_free <= 0) {
        im_free_batch(w, csize, which_bucket);
    }
    ((struct free_block *)p)->next = bucket->free_list;
    bucket->free_list = p;
    bucket->count_until_free--;
}

void cilk_internal_malloc_per_worker_init(__cilkrts_worker *w) {
    init_im_buckets(&(w->l->im_desc));
}

void cilk_internal_malloc_per_worker_terminate(__cilkrts_worker *w) {
#if INTERNAL_MALLOC_STATS
    for (unsigned int i = 0; i < NUM_BUCKETS; i++) {
        struct im_bucket *b = &(w->l->im_desc.buckets[i]);
        b->stats.num_free = free_list_length(b->free_list);
    }
#endif
}

void cilk_internal_malloc_per_worker_destroy(__cilkrts_worker *w) {
#if CILK_DEBUG
    for (unsigned int i = 0; i < NUM_BUCKETS; i++) {
        struct im_bucket *bucket = &(w->l->im_desc.buckets[i]);
        unsigned int k = free_list_length(bucket->free_list);
        CILK_ASSERT(w, (bucket->count_until_free + k) == bucket_capacity[i]);
    }
#endif
}
