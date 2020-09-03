#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h> /* ffs */
#include <sys/mman.h>
#include <unistd.h> /* sysconf */

#include "cilk-internal.h"
#include "debug.h"
#include "global.h"
#include "local.h"

CHEETAH_INTERNAL int cheetah_page_shift = 0;

#define MEM_LIST_SIZE 8U
#define INTERNAL_MALLOC_CHUNK_SIZE (32 * 1024)
#define SIZE_THRESH bucket_sizes[NUM_BUCKETS - 1]

/* TODO: Use sizeof(fiber), sizeof(closure), etc. */
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
    /* TODO: If sizes are powers of 2 use ffs() */
    for (unsigned int i = 0; i < NUM_BUCKETS; i++) {
        if (size <= bucket_sizes[i]) {
            return i;
        }
    }
    return -1; /* = infinity */
}

static inline unsigned int bucket_to_size(int which_bucket) {
    /* TODO: 1U << (which_bucket + 5) */
    return bucket_sizes[which_bucket];
}

static void add_to_free_list(struct im_bucket *bucket, void *p) {
    ((struct free_block *)p)->next = bucket->free_list;
    bucket->free_list = p;
    ++bucket->free_list_size;
}

static void *remove_from_free_list(struct im_bucket *bucket) {
    void *mem = bucket->free_list;
    if (mem) {
        bucket->free_list = ((struct free_block *)mem)->next;
        --bucket->free_list_size;
    }
    return mem;
}

/* initialize the buckets in struct cilk_im_desc */
static void init_im_buckets(struct cilk_im_desc *im_desc) {
    for (int i = 0; i < NUM_BUCKETS; i++) {
        struct im_bucket *bucket = &(im_desc->buckets[i]);
        bucket->free_list = NULL;
        bucket->free_list_size = 0;
        bucket->free_list_limit = bucket_capacity[i];
        bucket->allocated = 0;
        bucket->max_allocated = 0;
        bucket->wasted = 0;
    }
    im_desc->used = 0;
    for (int j = 0; j < IM_NUM_TAGS; ++j)
        im_desc->num_malloc[j] = 0;
}

//=========================================================
// Private helper functions for debugging
//=========================================================

static void dump_buckets(FILE *out, struct cilk_im_desc *d) {
    fprintf(out, "  %zd bytes used\n", d->used);
    for (unsigned i = 0; i < NUM_BUCKETS; ++i) {
        struct im_bucket *b = &d->buckets[i];
        if (!b->free_list && !b->free_list_size && !b->allocated)
            continue;
        fprintf(out, "  [%u] %d allocated (%d max, %zd wasted), %u free\n",
                bucket_to_size(i), b->allocated, b->max_allocated, b->wasted,
                b->free_list_size);
    }
}

static size_t free_bytes(struct cilk_im_desc *desc) {
    size_t free = 0;
    for (unsigned i = 0; i < NUM_BUCKETS; ++i)
        free += (size_t)desc->buckets[i].free_list_size * bucket_sizes[i];
    return free;
}

static long wasted_bytes(struct cilk_im_desc *desc) {
    long wasted = 0;
    for (unsigned i = 0; i < NUM_BUCKETS; ++i)
        wasted += desc->buckets[i].wasted;
    return wasted;
}

static size_t workers_used_and_free(global_state *g) {
    size_t worker_free = 0;
    long worker_used = 0, worker_wasted = 0;
    for (unsigned int i = 0; i < g->nworkers; i++) {
        __cilkrts_worker *w = g->workers[i];
        if (!w)
            continue; /* starting up or shutting down */
        local_state *l = w->l;
        worker_free += free_bytes(&l->im_desc);
        worker_used += l->im_desc.used;
        worker_wasted += wasted_bytes(&l->im_desc);
    }
    CILK_ASSERT_G(worker_used >= 0 && worker_wasted >= 0);
    return worker_used + worker_free + worker_wasted;
}

CHEETAH_INTERNAL
void dump_memory_state(FILE *out, global_state *g) {
    if (out == NULL)
        out = stderr;
    size_t global_free = free_bytes(&g->im_desc);
    ptrdiff_t available =
        (char *)g->im_pool.mem_end - (char *)g->im_pool.mem_begin;
    fprintf(out,
            "Global memory:\n  %zu allocated in %u blocks (%zu wasted)\n"
            "  %zd used + %tu available + %zu free = %zu\n",
            g->im_pool.allocated, g->im_pool.mem_list_index + 1,
            g->im_pool.wasted, g->im_desc.used, available, global_free,
            g->im_desc.used + available + global_free);
    dump_buckets(out, &g->im_desc);
    for (unsigned int i = 0; i < g->nworkers; i++) {
        __cilkrts_worker *w = g->workers[i];
        if (!w)
            continue;
        fprintf(out, "Worker %u:\n", i);
        dump_buckets(out, &w->l->im_desc);
    }
}

void dump_memory_state_stderr(global_state *g) { dump_memory_state(stderr, g); }

CHEETAH_INTERNAL
void internal_malloc_global_check(global_state *g) {
    /* TODO: Test should be
       global used = worker used + free
       global used + global free = allocated. */

    struct cilk_im_desc *d = &(g->im_desc);

    size_t total_malloc[IM_NUM_TAGS];
    for (int i = 0; i < IM_NUM_TAGS; ++i)
        total_malloc[i] = d->num_malloc[i];

    for (unsigned int i = 0; i < g->nworkers; i++) {
        __cilkrts_worker *w = g->workers[i];
        if (!w)
            continue; /* starting up or shutting down */
        local_state *l = w->l;
        for (int i = 0; i < IM_NUM_TAGS; ++i)
            total_malloc[i] += l->im_desc.num_malloc[i];
    }

    size_t allocated = g->im_pool.allocated;
    CILK_ASSERT_G(g->im_desc.used >= 0);
    size_t global_used = g->im_desc.used;
    size_t global_free = free_bytes(&g->im_desc);
    size_t worker_total = workers_used_and_free(g);
    size_t global_available =
        (char *)g->im_pool.mem_end - (char *)g->im_pool.mem_begin;

    if (global_used != worker_total ||
        global_used + global_free + global_available != allocated)
        dump_memory_state(stderr, g);

    CILK_CHECK(g,
               global_used + global_free + global_available == allocated &&
                   global_used == worker_total,
               "Possible memory leak: %zu+%zu+%zu global used+free+available, "
               "%zu allocated, %zu in workers",
               global_used, global_free, global_available, allocated,
               worker_total);
}

static void assert_global_pool(struct global_im_pool *pool) {
    CILK_ASSERT_G(pool->mem_list_index < pool->mem_list_size);
    if (pool->wasted > 0)
        CILK_ASSERT_G(pool->wasted < pool->allocated);
}

static void assert_bucket(struct im_bucket *bucket) {
    CILK_ASSERT_G(!!bucket->free_list == !!bucket->free_list_size);
    CILK_ASSERT_G_LE(bucket->free_list_size, bucket->free_list_limit, "%u");
    CILK_ASSERT_G_LE(bucket->allocated, bucket->max_allocated, "%d");
}

//=========================================================
// Private helper functions for IM stats
//=========================================================

#define HDR_DESC "%15s"
#define WORKER_HDR_DESC "%10s %3u:" // two char short compared to HDR_DESC
#define FIELD_DESC "%10zu"

static void print_worker_buckets_free(__cilkrts_worker *w, void *data) {
    FILE *fp = (FILE *)data;
    local_state *l = w->l;
    fprintf(fp, WORKER_HDR_DESC, "Worker", w->self);
    for (unsigned int j = 0; j < NUM_BUCKETS; j++) {
        fprintf(fp, FIELD_DESC,
                (size_t)l->im_desc.buckets[j].free_list_size * bucket_sizes[j]);
    }
    fprintf(fp, "\n");
}

static void print_worker_buckets_hwm(__cilkrts_worker *w, void *data) {
    FILE *fp = (FILE *)data;
    local_state *l = w->l;
    fprintf(fp, WORKER_HDR_DESC, "Worker", w->self);
    for (unsigned int j = 0; j < NUM_BUCKETS; j++) {
        fprintf(fp, FIELD_DESC,
                (size_t)l->im_desc.buckets[j].max_allocated * bucket_sizes[j]);
    }
    fprintf(fp, "\n");
}

static void print_im_buckets_stats(struct global_state *g) {
    fprintf(stderr, "\nBYTES IN FREE LISTS:\n");
    fprintf(stderr, HDR_DESC, "Bucket size:");
    for (int j = 0; j < NUM_BUCKETS; j++) {
        fprintf(stderr, FIELD_DESC, (size_t)bucket_sizes[j]);
    }
    fprintf(stderr, "\n-------------------------------------------"
                    "---------------------------------------------\n");

    fprintf(stderr, HDR_DESC, "Global:");
    for (unsigned int j = 0; j < NUM_BUCKETS; j++) {
        fprintf(stderr, FIELD_DESC,
                (size_t)g->im_desc.buckets[j].free_list_size * bucket_sizes[j]);
    }
    fprintf(stderr, "\n");
    for_each_worker(g, &print_worker_buckets_free, stderr);

    fprintf(stderr, "\nHIGH WATERMARK FOR BYTES ALLOCATED:\n");
    fprintf(stderr, HDR_DESC, "Bucket size:");
    for (int j = 0; j < NUM_BUCKETS; j++) {
        fprintf(stderr, FIELD_DESC, (size_t)bucket_sizes[j]);
    }
    fprintf(stderr, "\n-------------------------------------------"
                    "---------------------------------------------\n");
    for_each_worker(g, &print_worker_buckets_hwm, stderr);

    fprintf(stderr, "\n");
}

static void print_internal_malloc_stats(struct global_state *g) {
    unsigned page_size = 1U << cheetah_page_shift;
    fprintf(stderr, "\nINTERNAL MALLOC STATS\n");
    fprintf(stderr,
            "Total bytes allocated from system: %7zu KBytes (%zu pages)\n",
            g->im_pool.allocated / 1024,
            (g->im_pool.allocated + page_size - 1) / page_size);
    fprintf(stderr, "Total bytes allocated but wasted:  %7zu KBytes\n",
            g->im_pool.wasted / 1024);
    print_im_buckets_stats(g);
    fprintf(stderr, "\n");
}

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
    CILK_CHECK(w->g, mem, "Internal malloc failed to allocate %zu bytes", size);
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
    im_pool->allocated += INTERNAL_MALLOC_CHUNK_SIZE;
    im_pool->mem_list_index++;

    if (im_pool->mem_list_index >= im_pool->mem_list_size) {
        size_t new_list_size = im_pool->mem_list_size + MEM_LIST_SIZE;
        im_pool->mem_list = realloc(im_pool->mem_list,
                                    new_list_size * sizeof(*im_pool->mem_list));
        im_pool->mem_list_size = new_list_size;
        CILK_CHECK(w->g, im_pool->mem_list,
                   "Failed to extend global memory list by %zu bytes",
                   MEM_LIST_SIZE * sizeof(*im_pool->mem_list));
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
    global_state *g = w->g;
    CILK_ASSERT(w, g);
    CILK_ASSERT(w, size <= SIZE_THRESH);
    CILK_ASSERT(w, which_bucket < NUM_BUCKETS);

    struct im_bucket *bucket = &(g->im_desc.buckets[which_bucket]);
    struct cilk_im_desc *im_desc = &(g->im_desc);
    im_desc->used += size;
    /* ??? count calls to this function? */

    void *mem = remove_from_free_list(bucket);
    if (!mem) {
        struct global_im_pool *im_pool = &(g->im_pool);
        // allocate from the global pool
        if ((im_pool->mem_begin + size) > im_pool->mem_end) {
            // consider the left over as waste for now
            // TODO: Adding it to a random free list would be better.
            im_pool->wasted += im_pool->mem_end - im_pool->mem_begin;
            extend_global_pool(w);
        }
        mem = im_pool->mem_begin;
        im_pool->mem_begin += size;
    }

    return mem;
}

static void global_im_pool_destroy(struct global_im_pool *im_pool) {

    for (unsigned i = 0; i < im_pool->mem_list_size; i++) {
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
    g->im_pool.mem_list = calloc(MEM_LIST_SIZE, sizeof(*g->im_pool.mem_list));
    CILK_CHECK(g, g->im_pool.mem_list,
               "Cannot allocate %u * %zu bytes for mem_list", MEM_LIST_SIZE,
               sizeof(*g->im_pool.mem_list));
    g->im_pool.allocated = 0;
    g->im_pool.wasted = 0;
    init_im_buckets(&g->im_desc);

    g->im_desc.used = 0;
    for (int i = 0; i < IM_NUM_TAGS; ++i)
        g->im_desc.num_malloc[i] = 0;
}

void cilk_internal_malloc_global_terminate(global_state *g) {
    if (DEBUG_ENABLED(MEMORY))
        internal_malloc_global_check(g);
    if (ALERT_ENABLED(MEMORY))
        print_internal_malloc_stats(g);
}

void cilk_internal_malloc_global_destroy(global_state *g) {
    global_im_pool_destroy(&(g->im_pool)); // free global mem blocks
    cilk_mutex_destroy(&(g->im_lock));
    for (int i = 0; i < IM_NUM_TAGS; ++i) {
        CILK_ASSERT_G(g->im_desc.num_malloc[i] == 0);
    }
}

//=========================================================
// Per-worker memory allocator
//=========================================================

/**
 * Allocate a batch of memory of size 'size' from global im bucket 'bucket'
 * into per-worker im bucket 'bucket'.
 */
static void im_allocate_batch(__cilkrts_worker *w, size_t size,
                              unsigned int bucket_index) {
    global_state *g = w->g;
    local_state *l = w->l;
    struct im_bucket *bucket = &l->im_desc.buckets[bucket_index];
    unsigned int batch_size = bucket_capacity[bucket_index] / 2;
    cilk_mutex_lock(&(g->im_lock));
    for (unsigned int i = 0; i < batch_size; i++) {
        void *p = global_im_alloc(w, size, bucket_index);
        add_to_free_list(bucket, p);
    }
    cilk_mutex_unlock(&(g->im_lock));
    bucket->allocated += batch_size;
    if (bucket->allocated > bucket->max_allocated) {
        bucket->max_allocated = bucket->allocated;
    }
}

/**
 * Free a batch of memory of size 'size' from per-worker im bucket 'bucket'
 * back to global im bucket 'bucket'.
 */
static void im_free_batch(__cilkrts_worker *w, size_t size,
                          unsigned int which_bucket) {
    global_state *g = w->g;
    local_state *l = w->l;
    unsigned int batch_size = bucket_capacity[which_bucket] / 2;
    struct im_bucket *bucket = &(l->im_desc.buckets[which_bucket]);
    cilk_mutex_lock(&(g->im_lock));
    for (unsigned int i = 0; i < batch_size; ++i) {
        void *mem = remove_from_free_list(bucket);
        if (!mem)
            break;
        add_to_free_list(&g->im_desc.buckets[which_bucket], mem);
        g->im_desc.used -= size;
        --bucket->allocated;
    }
    cilk_mutex_unlock(&(w->g->im_lock));
    /* Account for bytes allocated change? */
}

/*
 * Malloc returns a piece of memory at the head of the free list;
 * last-in-first-out
 */
CHEETAH_INTERNAL
void *cilk_internal_malloc(__cilkrts_worker *w, size_t size, enum im_tag tag) {
    local_state *l = w->l;
    unsigned int which_bucket = size_to_bucket(size);
    if (which_bucket >= NUM_BUCKETS) {
        return malloc_from_system(w, size);
    }
    if (ALERT_ENABLED(MEMORY))
        fprintf(stderr, "[W%d] alloc %zu tag %d\n", w->self, size, (int)tag);

    l->im_desc.used += size;
    l->im_desc.num_malloc[tag] += 1;

    unsigned int csize = bucket_to_size(which_bucket); // canonicalize the size
    struct im_bucket *bucket = &(l->im_desc.buckets[which_bucket]);
    bucket->wasted += csize - size;
    void *mem = remove_from_free_list(bucket);

    if (!mem) { // when out of memory, allocate a batch from global pool
        im_allocate_batch(w, csize, which_bucket);
        mem = remove_from_free_list(bucket);
        CILK_ASSERT(w, mem);
    }
    if (ALERT_ENABLED(MEMORY))
        dump_memory_state(NULL, w->g);
#if 0 /* race condition if workers are running */
    if (DEBUG_ENABLED(MEMORY_SLOW))
        internal_malloc_global_check(w->g);
#endif
    return mem;
}

/*
 * Free simply returns to the free list; last-in-first-out
 */
void cilk_internal_free(__cilkrts_worker *w, void *p, size_t size,
                        enum im_tag tag) {
    if (size > SIZE_THRESH) {
        free_to_system(p, size);
        return;
    }
    if (ALERT_ENABLED(MEMORY))
        fprintf(stderr, "[W%d] free %zu tag %d\n", w->self, size, (int)tag);

    local_state *l = w->l;
    l->im_desc.used -= size;
    l->im_desc.num_malloc[tag] -= 1;

    unsigned int which_bucket = size_to_bucket(size);
    CILK_ASSERT(w, which_bucket >= 0 && which_bucket < NUM_BUCKETS);
    unsigned int csize = bucket_to_size(which_bucket); // canonicalize the size
    struct im_bucket *bucket = &(l->im_desc.buckets[which_bucket]);
    bucket->wasted -= csize - size;

    add_to_free_list(bucket, p);

    while (bucket->free_list_size > bucket->free_list_limit) {
        im_free_batch(w, csize, which_bucket);
    }
    if (ALERT_ENABLED(MEMORY))
        dump_memory_state(NULL, w->g);
#if 0 /* not safe with multiple workers */
    if (DEBUG_ENABLED(MEMORY_SLOW))
        internal_malloc_global_check(w->g);
#endif
}

/* This function is called after workers have terminated.
   It has no locking. */
void cilk_internal_free_global(global_state *g, void *p, size_t size,
                               enum im_tag tag) {
    unsigned int which_bucket = size_to_bucket(size);
    add_to_free_list(&g->im_desc.buckets[which_bucket], p);
    g->im_desc.num_malloc[tag]--;
    g->im_desc.used -= bucket_to_size(which_bucket);
}

void cilk_internal_malloc_per_worker_init(__cilkrts_worker *w) {
    init_im_buckets(&(w->l->im_desc));
}

void cilk_internal_malloc_per_worker_terminate(__cilkrts_worker *w) {
    global_state *g = w->g; /* Global state is locked by caller. */
    local_state *l = w->l;
    assert_global_pool(&g->im_pool);
    if (DEBUG_ENABLED(MEMORY_SLOW))
        internal_malloc_global_check(g);
    for (unsigned int i = 0; i < NUM_BUCKETS; i++) {
        assert_bucket(&l->im_desc.buckets[i]);
        while (l->im_desc.buckets[i].free_list)
            im_free_batch(w, bucket_to_size(i), i);
    }
    for (int i = 0; i < IM_NUM_TAGS; ++i) {
        g->im_desc.num_malloc[i] += l->im_desc.num_malloc[i];
        l->im_desc.num_malloc[i] = 0;
    }
    if (ALERT_ENABLED(MEMORY))
        dump_memory_state(NULL, w->g);
    /* This check is safe because all worker threads have exited. */
    if (DEBUG_ENABLED(MEMORY_SLOW))
        internal_malloc_global_check(g);
}

void cilk_internal_malloc_per_worker_destroy(__cilkrts_worker *w) {
    /* The main closure and fiber have not yet been destroyed.  They are
       allocated with system malloc instead of internal malloc. */
    local_state *l = w->l;
    for (unsigned int i = 0; i < NUM_BUCKETS; i++) {
        CILK_ASSERT_INDEX_ZERO(w, l->im_desc.buckets, i, .free_list_size, "%u");
        CILK_ASSERT_INDEX_ZERO(w, l->im_desc.buckets, i, .free_list, "%p");
        /* allocated may be nonzero due to memory migration */
    }
}

const char *name_for_im_tag(enum im_tag tag) {
    switch (tag) {
    case IM_UNCLASSIFIED:
        return "unclassified";
    case IM_CLOSURE:
        return "closure";
    case IM_FIBER:
        return "fiber";
    case IM_REDUCER_MAP:
        return "reducer map";
    default:
        return "unknown";
    }
}
