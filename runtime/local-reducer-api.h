#include "local-hypertable.h"

static inline __cilkrts_worker *get_worker_or_default(void) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    if (NULL == w)
        w = default_cilkrts->workers[0];
    return w;
}

static inline struct local_hyper_table *
get_local_hyper_table(__cilkrts_worker *w) {
    if (NULL == w->hyper_table) {
        w->hyper_table = local_hyper_table_alloc();
    }
    return w->hyper_table;
}