#include "cilk-internal.h"
#include "global.h"
#include "hyperobject_base.h"
#include "local-hypertable.h"

static inline __cilkrts_worker *get_worker_or_default(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (NULL == w)
        w = default_cilkrts->workers[0];
    return w;
}

static struct local_hyper_table *get_local_hyper_table(__cilkrts_worker *w) {
    return w->hyper_table;
}

void *__cilkrts_reducer_lookup(void *key, size_t size,
                               __cilk_identity_fn identity,
                               __cilk_reduce_fn reduce) {
    // What should we do when the worker is NULL, meaning we're
    // outside of a cilkified region?  If we're simply looking up the
    // reducer, we could just return the key, since that's the correct
    // view.  But if we're registering the reducer, then we should add
    // the reducer to the table, or else the worker might not find the
    // correct view when it subsequently executes a cilkified region.
    //
    // If we're implicitly registering reducers upon lookup, then we
    // could use a reducer lookup from outside the region to
    // implicitly register that reducer.  But we're not guaranteed to
    // always have a reducer lookup from outside a cilkified region
    // nor a reducer lookup that we can distinguish from a
    // registration (i.e., whether to use the key as the view or
    // create a new view).
    __cilkrts_worker *w = get_worker_or_default();
    struct local_hyper_table *table = get_local_hyper_table(w);
    struct bucket *b = find_hyperobject(table, (uintptr_t)key);
    if (b && is_valid(b->key))
        // Return the existing view.
        return b->value.view;

    // Create a new view and initialize it with the identity function.
    /* void *new_view = __cilkrts_hyper_alloc(size); */
    void *new_view =
        cilk_aligned_alloc(round_size_to_alignment(64, size), size);
    identity(new_view);
    // Insert the new view into the local hypertable.
    struct bucket new_bucket = {
        .key = (uintptr_t)key,
        .value = {.view = new_view, .reduce_fn = reduce}};
    bool success = insert_hyperobject(table, new_bucket);
    CILK_ASSERT(
        w, success && "__cilkrts_reducer_lookup failed to insert new reducer.");
    // Return the new view.
    return new_view;
}

void __cilkrts_reducer_register(void *key, size_t size,
				__cilk_identity_fn id,
				__cilk_reduce_fn reduce) {
    __cilkrts_worker *w = get_worker_or_default();
    struct local_hyper_table *table = get_local_hyper_table(w);
    struct bucket b = {.key = (uintptr_t)key,
                       .value = {.view = key, .reduce_fn = reduce}};
    bool success = insert_hyperobject(table, b);
    CILK_ASSERT(w, success && "Failed to register reducer.");
}

void __cilkrts_reducer_register_32(void *key, uint32_t size,
                                   __cilk_identity_fn id,
                                   __cilk_reduce_fn reduce) {
    __cilkrts_reducer_register(key, size, id, reduce);
}

void __cilkrts_reducer_register_64(void *key, uint64_t size,
                                   __cilk_identity_fn id,
                                   __cilk_reduce_fn reduce) {
    __cilkrts_reducer_register(key, size, id, reduce);
}

void __cilkrts_reducer_unregister(void *key) {
    __cilkrts_worker *w = get_worker_or_default();
    struct local_hyper_table *table = get_local_hyper_table(w);
    bool success = remove_hyperobject(table, (uintptr_t)key);
    CILK_ASSERT(w, success && "Failed to unregister reducer.");
}
