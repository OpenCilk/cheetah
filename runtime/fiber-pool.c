#include <stdio.h>
#include <stdlib.h>

#include "cilk-internal.h"
#include "debug.h"
#include "fiber.h"
#include "global.h"
#include "mutex.h"

// Whent the pool becomes full (empty), free (allocate) this fraction
// of the pool back to (from) parent / the OS.
#define BATCH_FRACTION 2
#define GLOBAL_POOL_RATIO 10 // make global pool this much larger

//=========================================================================
// Currently the fiber pools are organized into two-levels, like in Hoard
// --- per-worker private pool plus a global pool.  The per-worker private
// pool are accessed by the owner worker only and thus do not require
// synchronization.  The global pool may be accessed concurrently and thus
// require synchronization.  Thus, the pool->lock is initialized to NULL for
// per-worker pools and cilk_mutex for the global one.
//
// The per-worker pools are initlaized with some free fibers preallocated
// already and the global one starts out empty.  A worker typically acquires
// and free fibers from / to the its per-worker pool but only allocate / free
// batches from / to the global parent pool when necessary (i.e., buffer
// exceeds capacity and there are fibers needed to be freed, or need fibers
// but the buffer is empty.
//
// For now, we don't ever allocate fibers into the global one --- we only use
// the global one to load balance between per-worker pools.
//=========================================================================

//=========================================================
// Private helper functions for maintaining pool stats
//=========================================================

#if FIBER_STATS
static void fiber_pool_stat_init(struct cilk_fiber_pool *pool) {
    pool->stats.in_use = 0;
    pool->stats.max_in_use = 0;
    pool->stats.max_free = 0;
}

static void fiber_pool_stat_print(struct global_state *g) {

#define HDR_DESC "%15s"
#define WORKER_HDR_DESC "%10s %3u:" // two char short compared to HDR_DESC
#define FIELD_STR_DESC "%10s"
#define FIELD_DESC "%10d"

    fprintf(stderr, "\nFIBER POOL STATS\n");
    fprintf(stderr, HDR_DESC, "Fiber stats:");
    fprintf(stderr,
            FIELD_STR_DESC FIELD_STR_DESC FIELD_STR_DESC FIELD_STR_DESC "\n",
            "in-use", "max in-use", "curr-free", "max-free");
    fprintf(stderr, "-------------------------------------------"
                    "-----------------------------\n");
    fprintf(stderr, HDR_DESC, "Global:");
    fprintf(stderr, FIELD_DESC FIELD_DESC FIELD_DESC FIELD_DESC "\n",
            g->fiber_pool.stats.in_use, g->fiber_pool.stats.max_in_use,
            g->fiber_pool.size, g->fiber_pool.stats.max_free);

    for (unsigned int i = 0; i < g->options.nproc; i++) {
        __cilkrts_worker *w = g->workers[i];
        fprintf(stderr, WORKER_HDR_DESC, "Worker", w->self);
        fprintf(stderr, FIELD_DESC FIELD_DESC FIELD_DESC FIELD_DESC "\n",
                w->l->fiber_pool.stats.in_use,
                w->l->fiber_pool.stats.max_in_use, w->l->fiber_pool.size,
                w->l->fiber_pool.stats.max_free);
    }
    fprintf(stderr, "\n");
}
#else
#define fiber_pool_stat_init(pool)
#define fiber_pool_stat_print(g)
#endif // FIBER_STATS

//=========================================================
// Private helper functions
//=========================================================

// forward decl
static void fiber_pool_allocate_batch(__cilkrts_worker *w,
                                      struct cilk_fiber_pool *pool,
                                      unsigned int num_to_allocate);
static void fiber_pool_free_batch(__cilkrts_worker *w,
                                  struct cilk_fiber_pool *pool,
                                  unsigned int num_to_free);

/* Helper function for initializing fiber pool */
static void fiber_pool_init(struct cilk_fiber_pool *pool, size_t stacksize,
                            unsigned int bufsize,
                            struct cilk_fiber_pool *parent, int is_shared) {
    if (is_shared) {
        pool->lock = malloc(sizeof(*pool->lock));
        cilk_mutex_init(pool->lock);
        pool->mutex_owner = NO_WORKER;
    } else {
        pool->lock = NULL;
    }
    pool->stack_size = stacksize;
    pool->parent = parent;
    pool->capacity = bufsize;
    pool->size = 0;
    pool->fibers = calloc(bufsize, sizeof(*pool->fibers));
}

/* Helper function for destroying fiber pool */
static void fiber_pool_destroy(struct cilk_fiber_pool *pool) {

    if (pool->lock) {
        CILK_ASSERT_G(pool->mutex_owner == NO_WORKER);
        cilk_mutex_destroy(pool->lock);
        free(pool->lock);
        pool->lock = NULL;
    }
    free(pool->fibers);
    pool->parent = NULL;
    pool->fibers = NULL;
}

static inline void fiber_pool_assert_ownership(__cilkrts_worker *w,
                                               struct cilk_fiber_pool *pool) {
    CILK_ASSERT(w, pool->lock == NULL || pool->mutex_owner == w->self);
}

static inline void fiber_pool_assert_alienation(__cilkrts_worker *w,
                                                struct cilk_fiber_pool *pool) {
    CILK_ASSERT(w, pool->lock == NULL || pool->mutex_owner != w->self);
}

static inline void fiber_pool_lock(__cilkrts_worker *w,
                                   struct cilk_fiber_pool *pool) {
    if (pool->lock) {
        fiber_pool_assert_alienation(w, pool);
        cilk_mutex_lock(pool->lock);
        pool->mutex_owner = w->self;
    }
}

static inline void fiber_pool_unlock(__cilkrts_worker *w,
                                     struct cilk_fiber_pool *pool) {
    if (pool->lock) {
        fiber_pool_assert_ownership(w, pool);
        pool->mutex_owner = NO_WORKER;
        cilk_mutex_unlock(pool->lock);
    }
}

/**
 * Increase the buffer size for the free fibers.  If the current size is
 * already larger than the new size, do nothing.  Assume lock acquired upon
 * entry.
 */
static void fiber_pool_increase_capacity(__cilkrts_worker *w,
                                         struct cilk_fiber_pool *pool,
                                         unsigned int new_size) {

    fiber_pool_assert_ownership(w, pool);
    if (pool->capacity < new_size) {
        pool->fibers = realloc(pool->fibers, new_size * sizeof(*pool->fibers));
        pool->capacity = new_size;
    }
}

/**
 * Decrease the buffer size for the free fibers.  If the current size is
 * already smaller than the new size, do nothing.  Assume lock acquired upon
 * entry.
 */
__attribute__((unused)) // unused for now
static void
fiber_pool_decrease_capacity(__cilkrts_worker *w, struct cilk_fiber_pool *pool,
                             unsigned int new_size) {

    fiber_pool_assert_ownership(w, pool);

    if (pool->size > new_size) {
        int diff = pool->size - new_size;
        fiber_pool_free_batch(w, pool, diff);
        CILK_ASSERT(w, pool->size == new_size);
    }
    if (pool->capacity > new_size) {
        pool->fibers = (struct cilk_fiber **)realloc(
            pool->fibers, new_size * sizeof(struct cilk_fiber *));
        pool->capacity = new_size;
    }
}

/**
 * Allocate num_to_allocate number of new fibers into the pool.
 * We will first look into the parent pool, and if the parent pool does not
 * have enough, we then get it from the system.
 */
static void fiber_pool_allocate_batch(__cilkrts_worker *w,
                                      struct cilk_fiber_pool *pool,
                                      const unsigned int batch_size) {
    fiber_pool_assert_ownership(w, pool);
    fiber_pool_increase_capacity(w, pool, batch_size + pool->size);

    unsigned int from_parent = 0;
    if (pool->parent) {
        struct cilk_fiber_pool *parent = pool->parent;
        fiber_pool_lock(w, parent);
        from_parent = parent->size <= batch_size ? parent->size : batch_size;
        for (unsigned int i = 0; i < from_parent; i++) {
            pool->fibers[pool->size++] = parent->fibers[--parent->size];
        }
#if FIBER_STATS
        // update parent pool stats before releasing the lock on it
        parent->stats.in_use += from_parent;
        if (parent->stats.in_use > parent->stats.max_in_use) {
            parent->stats.max_in_use = parent->stats.in_use;
        }
#endif
        fiber_pool_unlock(w, parent);
    }
    if (batch_size - from_parent) { // if we need more still
        for (unsigned int i = from_parent; i < batch_size; i++) {
            pool->fibers[pool->size++] = cilk_fiber_allocate(w);
        }
    }
#if FIBER_STATS
    if (pool->size > pool->stats.max_free) {
        pool->stats.max_free = pool->size;
    }
#endif
}

/**
 * Free num_to_free fibers from this pool back to either the parent
 * or the system.
 */
static void fiber_pool_free_batch(__cilkrts_worker *w,
                                  struct cilk_fiber_pool *pool,
                                  const unsigned int batch_size) {

    fiber_pool_assert_ownership(w, pool);
    CILK_ASSERT(w, batch_size <= pool->size);

    unsigned int to_parent = 0;
    if (pool->parent) { // first try to free into the parent
        struct cilk_fiber_pool *parent = pool->parent;
        fiber_pool_lock(w, parent);
        to_parent = (batch_size <= (parent->capacity - parent->size))
                        ? batch_size
                        : (parent->capacity - parent->size);
        // free what we can within the capacity of the parent pool
        for (unsigned int i = 0; i < to_parent; i++) {
            parent->fibers[parent->size++] = pool->fibers[--pool->size];
        }
        CILK_ASSERT(w, parent->size <= parent->capacity);
#if FIBER_STATS
        parent->stats.in_use -= to_parent;
        if (parent->size > parent->stats.max_free) {
            parent->stats.max_free = parent->size;
        }
#endif
        fiber_pool_unlock(w, parent);
    }
    if ((batch_size - to_parent) > 0) { // still need to free more
        for (unsigned int i = to_parent; i < batch_size; i++) {
            struct cilk_fiber *fiber = pool->fibers[--pool->size];
            cilk_fiber_deallocate(w, fiber);
        }
    }
    CILK_ASSERT(w, pool->size >= 0);
}

//=========================================================
// Supported public functions
//=========================================================

/* Global fiber pool initialization: */
void cilk_fiber_pool_global_init(global_state *g) {

    unsigned int bufsize = GLOBAL_POOL_RATIO * g->options.fiber_pool_cap;
    struct cilk_fiber_pool *pool = &(g->fiber_pool);
    fiber_pool_init(pool, g->options.stacksize, bufsize, NULL, 1 /*shared*/);
    CILK_ASSERT_G(NULL != pool->fibers);
    fiber_pool_stat_init(pool);
    /* let's not preallocate for global fiber pool for now */
}

/* This does not yet destroy the fiber pool; merely collects
 * stats and print them out (if FIBER_STATS is set)
 */
void cilk_fiber_pool_global_terminate(global_state *g) {
    fiber_pool_stat_print(g);
}

/* Global fiber pool clean up. */
void cilk_fiber_pool_global_destroy(global_state *g) {

    struct cilk_fiber_pool *pool = &(g->fiber_pool);
    CILK_ASSERT_G(pool->size == 0); // worker 0 should have freed everything
    fiber_pool_destroy(pool);
}

/**
 * Per-worker fiber pool initialization: should be called per worker so
 * so that fiber comes from the core on which the worker is running on.
 */
void cilk_fiber_pool_per_worker_init(__cilkrts_worker *w) {

    unsigned int bufsize = w->g->options.fiber_pool_cap;
    struct cilk_fiber_pool *pool = &(w->l->fiber_pool);
    fiber_pool_init(pool, w->g->options.stacksize, bufsize, &(w->g->fiber_pool),
                    0 /* private */);
    CILK_ASSERT(w, NULL != pool->fibers);
    CILK_ASSERT(w, w->g->fiber_pool.stack_size == pool->stack_size);

    fiber_pool_allocate_batch(w, pool, bufsize / BATCH_FRACTION);
    fiber_pool_stat_init(pool);
}

/* This does not yet destroy the fiber pool; merely collects
 * stats and print them out (if FIBER_STATS is set)
 */
void cilk_fiber_pool_per_worker_terminate(__cilkrts_worker *w) {
    // nothing to do at the moment
}

/* Per-worker fiber pool clean up. */
void cilk_fiber_pool_per_worker_destroy(__cilkrts_worker *w) {

    struct cilk_fiber_pool *pool = &(w->l->fiber_pool);
    for (unsigned i = 0; i < pool->size; i++) {
        cilk_fiber_deallocate(w, pool->fibers[i]);
    }
    fiber_pool_destroy(pool);

    // ANGE FIXME: is there a better way to do this?
    // worker 0 responsible for freeing fibers in global pool
    // need to do this here, since we can't free fibers into internal malloc
    // without having a worker pointer
    if (w->self == 0) {
        struct cilk_fiber_pool *parent = &(w->g->fiber_pool);
        fiber_pool_lock(w, parent);
        fiber_pool_free_batch(w, parent, parent->size);
        fiber_pool_unlock(w, parent);
    }
}

/**
 * Allocate a fiber from this pool; if this pool is empty,
 * allocate a batch of fibers from the parent pool (or system).
 */
struct cilk_fiber *cilk_fiber_allocate_from_pool(__cilkrts_worker *w) {
    struct cilk_fiber_pool *pool = &(w->l->fiber_pool);
    if (pool->size == 0) {
        fiber_pool_allocate_batch(w, pool, pool->capacity / BATCH_FRACTION);
    }
    CILK_ASSERT(w, pool->size > 0);
    struct cilk_fiber *ret = pool->fibers[--pool->size];
#if WHEN_FIBER_STATS
    pool->stats.in_use++;
    if (pool->stats.in_use > pool->stats.max_in_use) {
        pool->stats.max_in_use = pool->stats.in_use;
    }
#endif
    CILK_ASSERT(w, ret);
    return ret;
}

/**
 * Free fiber_to_return into this pool; if this pool is full,
 * free a batch of fibers back into the parent pool (or system).
 */
void cilk_fiber_deallocate_to_pool(__cilkrts_worker *w,
                                   struct cilk_fiber *fiber_to_return) {
    struct cilk_fiber_pool *pool = &(w->l->fiber_pool);
    if (pool->size == pool->capacity) {
        fiber_pool_free_batch(w, pool, pool->capacity / BATCH_FRACTION);
        CILK_ASSERT(w, (pool->capacity - pool->size) >=
                           (pool->capacity / BATCH_FRACTION));
    }
    if (fiber_to_return) {
        pool->fibers[pool->size++] = fiber_to_return;
#if WHEN_FIBER_STATS
        pool->stats.in_use--;
        if (pool->size > pool->stats.max_free) {
            pool->stats.max_free = pool->size;
        }
#endif
    }
}
