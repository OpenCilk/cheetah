#ifndef _LOCAL_REDUCER_API_H
#define _LOCAL_REDUCER_API_H

#include "cilk-internal.h"
#include "global.h"
#include "local-hypertable.h"

static inline struct local_hyper_table *
get_local_hyper_table(__cilkrts_worker *w) {
    if (NULL == w->hyper_table) {
        w->hyper_table = __cilkrts_local_hyper_table_alloc();
    }
    return w->hyper_table;
}

__attribute__((always_inline)) static inline struct local_hyper_table *
get_hyper_table() {
    return get_local_hyper_table(__cilkrts_get_tls_worker());
}

static inline struct local_hyper_table *
get_local_hyper_table_or_null(const __cilkrts_worker *w) {
    return w->hyper_table;
}

#endif // _LOCAL_REDUCER_API_H
