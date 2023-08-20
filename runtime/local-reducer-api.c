#include "cilk-internal.h"
#include "global.h"
#include "hyperobject_base.h"
#include "local-hypertable.h"
#include "local-reducer-api.h"
#include "rts-config.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void __cilkrts_reducer_register(void *key, size_t size,
				__cilk_identity_fn id,
				__cilk_reduce_fn reduce) {
    struct local_hyper_table *table = get_hyper_table();
    struct bucket b = {.key = (uintptr_t)key,
                       .value = {.view = key, .reduce_fn = reduce}};
    bool success = insert_hyperobject(table, b);
    CILK_ASSERT(get_worker_or_default(),
                success && "Failed to register reducer.");
    (void)success;
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
    struct local_hyper_table *table = get_hyper_table();
    bool success = remove_hyperobject(table, (uintptr_t)key);
    /* CILK_ASSERT(get_worker_or_default(), success && "Failed to unregister
     * reducer."); */
    (void)success;
}

#pragma clang diagnostic pop

CHEETAH_INTERNAL
void *internal_reducer_lookup(__cilkrts_worker *w, void *key, size_t size,
                              void *identity_ptr, void *reduce_ptr) {
    struct local_hyper_table *table = get_local_hyper_table(w);
    struct bucket *b = find_hyperobject(table, (uintptr_t)key);
    if (__builtin_expect(!!b, true)) {
        CILK_ASSERT(w, key == (void *)b->key);
        // Return the existing view.
        return b->value.view;
    }

    return __cilkrts_insert_new_view(table, (uintptr_t)key, size,
                                     (__cilk_identity_fn)identity_ptr,
                                     (__cilk_reduce_fn)reduce_ptr);
}

CHEETAH_INTERNAL
void internal_reducer_remove(__cilkrts_worker *w, void *key) {
    struct local_hyper_table *table = get_local_hyper_table(w);
    bool success = remove_hyperobject(table, (uintptr_t)key);
    (void)success;
}
