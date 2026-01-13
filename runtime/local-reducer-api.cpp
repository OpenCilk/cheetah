#include "cilk-internal.h"
#include "cilk2c_inlined.h"
// #include "local-hypertable.h"
#include "local-hyper-pagetable.h"
#include "local-reducer-api.h"
#include "rts-config.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

__reducer_base::__reducer_base() {
    // This would be a great place to register the reducer,
    // but doing so would break the equivalence between
    // leftmost view and dynamic views.  The derived class
    // identity operation would need to pass a flag to this
    // constructor to suppress registration.
}

__reducer_base::~__reducer_base() {}

static void reducer_register(bucket &b) __CILKRTS_NOTHROW {
    struct hyper_table *table =
        get_local_hyper_table(__cilkrts_get_tls_worker());
    [[maybe_unused]] bool success = insert_hyperobject(table, b);
    CILK_ASSERT(success && "Failed to register reducer.");
}

void __cilkrts_reducer_register_0(__reducer_base *key) __CILKRTS_NOTHROW {
    bucket b{.key = (uintptr_t)key, .data = {.view = nullptr, .extra = key}};
    reducer_register(b);
}

void __cilkrts_reducer_register_1(void *key,
                                  __reducer_callbacks *cb) __CILKRTS_NOTHROW {
    bucket b{
        .key = (uintptr_t)key,
        .data = {.view = key, .extra = &cb->reduce},
    };
    reducer_register(b);
}

void __cilkrts_reducer_register_2(void *key, __cilk_c_reduce_fn *reduce)
    __CILKRTS_NOTHROW {
    bucket b{
        .key = (uintptr_t)key,
        .data = {.view = key, .extra = reduce},
    };
    reducer_register(b);
}

void __cilkrts_reducer_unregister(void *key) noexcept {
    if (struct hyper_table *table = get_hyper_table()) {
        [[maybe_unused]] bool success =
            remove_hyperobject(table, (uintptr_t)key);
        // CILK_ASSERT(success && "Failed to unregister reducer.");
    }
}

#pragma clang diagnostic pop

CHEETAH_INTERNAL
__reducer_base *internal_reducer_lookup(__cilkrts_worker *w,
                                        __reducer_base *key) {
    struct hyper_table *table = get_local_hyper_table(w);
    bucket *b = find_hyperobject(table, (uintptr_t)key);
    if (__builtin_expect(!!b, true)) {
        CILK_ASSERT_POINTER_EQUAL(key, (void *)getAddrFromKey(b->key));
        // Return the existing view.
        return std::get<__reducer_base *>(b->data.extra);
    }

    return __cilkrts_insert_new_view_0(table, key);
}

CHEETAH_INTERNAL
void internal_reducer_remove(__cilkrts_worker *w, void *key) {
    struct hyper_table *table = get_local_hyper_table(w);
    [[maybe_unused]] bool success = remove_hyperobject(table, (uintptr_t)key);
}
