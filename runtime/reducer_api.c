
/* Begin new reducer interface */

#include <stdatomic.h>
#include <stdint.h>
#include <cilk/cilk_api.h>
#include "rts-config.h"
#include "hyperobject_base.h"
#include "cilk-internal.h"
#include "hypertable.h"
#include "local.h"

static const size_t HSIZE = 0; // meaning use default

hyperobject_base *
__cilkrts_add_key(void *leftmost, size_t size,
                  __cilk_identity_fn id,
                  __cilk_reduce_fn reduce) {
  __cilkrts_worker *w = __cilkrts_get_tls_worker();

  if (size <= 0)
    cilkrts_bug(w, "User error: reducer size not positive");

  size = size + (CILK_CACHE_LINE - 1) & ~(size_t)(CILK_CACHE_LINE - 1);

  /* TODO: Internal malloc (which wants a non-null worker) */
  hyperobject_base *hyper =
    cilk_aligned_alloc(CILK_CACHE_LINE, sizeof (hyperobject_base));
  if (!hyper)
    cilkrts_bug(w, "unable to allocate hyperobject");
  hyper->identity_fn = id;
  hyper->reduce_fn   = reduce;
  hyper->key         = leftmost;
  hyper->view_size   = size;
  hyper->id_num      = 0;
  cilkrts_hyper_register(hyper);

  if (w && w->l->hyper_table) {
    enum hyper_table_error error =
      hyper_table_cache_insert(w->l->hyper_table, leftmost, hyper);
    if (error != HYPER_OK) {
      cilkrts_bug(w, "unable to insert hyperobject in table (%s)",
                  hyper_table_error_string(error));
      cilkrts_hyper_unregister(hyper);
      return 0;
    }
    return hyper;
  }

  struct hyper_table *table = hyper_table_get_or_create(HSIZE);
  if (hyper_table_insert(table, leftmost, hyper) != HYPER_OK) {
    cilkrts_bug(w, "unable to insert hyperobject in table");
    cilkrts_hyper_unregister(hyper);
    return 0;
  }
  return hyper;
}

void __cilkrts_drop_key(void *key) {
  __cilkrts_worker *w = __cilkrts_get_tls_worker();
  hyperobject_base *hyper;
  if (w && w->l->hyper_table) {
    hyper = hyper_table_cache_remove(w->l->hyper_table, key);
  } else {
    struct hyper_table *table = hyper_table_get_or_create(HSIZE);
    hyper = hyper_table_remove(table, key);
  }
  if (!hyper)
    return;
  cilkrts_hyper_unregister(hyper);
  free(hyper);
}

hyperobject_base *__cilkrts_hyper_key(void *key) {
  __cilkrts_worker *w = __cilkrts_get_tls_worker();
  if (w && w->l->hyper_table)
    return hyper_table_cache_lookup(w->l->hyper_table, key);
  struct hyper_table *table = hyper_table_get_or_create(HSIZE);
  return hyper_table_lookup(table, key);
}

/* ABI, declared in cilk_api.h */
/* void *__cilkrts_reducer_lookup(void *key) { */
void *__cilkrts_reducer_lookup(void *key, size_t size,
			       __cilk_identity_fn id,
			       __cilk_reduce_fn reduce) {
  hyperobject_base *hyper = __cilkrts_hyper_key(key);
  if (hyper)
    return cilkrts_hyper_lookup(hyper);
  return key;
}

void
__cilkrts_reducer_register(void *key, size_t size,
                           __cilk_identity_fn id,
                           __cilk_reduce_fn reduce) {
  __cilkrts_add_key(key, size, id, reduce);
}

void
__cilkrts_reducer_register_32(void *key, uint32_t size,
                              __cilk_identity_fn id,
                              __cilk_reduce_fn reduce) {
  __cilkrts_add_key(key, size, id, reduce);
}

void
__cilkrts_reducer_register_64(void *key, uint64_t size,
                              __cilk_identity_fn id,
                              __cilk_reduce_fn reduce) {
  __cilkrts_add_key(key, size, id, reduce);
}

void __cilkrts_reducer_unregister(void *key) {
  __cilkrts_drop_key(key);
}
