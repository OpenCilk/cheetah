#ifndef _LOCAL_REDUCER_API_H
#define _LOCAL_REDUCER_API_H

#include "cilk-internal.h"
#include "global.h"
// #include "local-hypertable.h"
#include "local-hyper-pagetable.h"

static inline hyper_table *
get_local_hyper_table(__cilkrts_worker *w) {
    if (nullptr == w->hyper_table) {
        w->hyper_table = __cilkrts_local_hyper_table_alloc();
    }
    return w->hyper_table;
}

__attribute__((always_inline)) static inline struct hyper_table *
get_hyper_table() {
    return get_local_hyper_table(__cilkrts_get_tls_worker());
}

static inline struct hyper_table *
get_local_hyper_table_or_null(const __cilkrts_worker *w) {
    return w->hyper_table;
}

#endif // _LOCAL_REDUCER_API_H
