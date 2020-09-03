// LONG_BIT is defined in limits.h when _GNU_SOURCE is defined.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "reducer_impl.h"
#include "cilk/hyperobject_base.h"
#include "global.h"
#include "init.h"
#include "internal-malloc.h"
#include "mutex.h"
#include "scheduler.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <limits.h>

#define USE_INTERNAL_MALLOC 1

#define REDUCER_LIMIT 1024U
#define GLOBAL_REDUCER_LIMIT 100U

// =================================================================
// ID managers for reducers
// =================================================================

/* This structure may need to exist before Cilk is started.
 */
typedef struct reducer_id_manager {
    pthread_mutex_t mutex; // enfore mutual exclusion on access to this desc
    worker_id mutex_owner; // worker id who holds the mutex
    hyper_id_t spa_cap;
    hyper_id_t next; // a hint
    hyper_id_t hwm;  // one greater than largest ID ever used
    unsigned long *used;
    /* When Cilk is not running, global holds all the registered
       hyperobjects so they can be imported into the first worker.
       Size is GLOBAL_REDUCER_LIMIT, regardless of spa_cap.   */
    __cilkrts_hyperobject_base **global;
} reducer_id_manager;


static void reducer_id_manager_assert_ownership(reducer_id_manager *m,
                                                __cilkrts_worker *const w) {
    if (w)
        CILK_ASSERT(w, m->mutex_owner == w->self);
}

static inline void reducer_id_manager_lock(reducer_id_manager *m,
                                           __cilkrts_worker *w) {
    int error = pthread_mutex_lock(&m->mutex);
    if (error == 0) {
        m->mutex_owner = w ? w->self : NO_WORKER;
    } else {
        cilkrts_bug(w, "unable to lock reducer ID manager");
    }
}

static void reducer_id_manager_unlock(reducer_id_manager *m,
                                      __cilkrts_worker *w) {
    reducer_id_manager_assert_ownership(m, w);
    m->mutex_owner = NO_WORKER;
    pthread_mutex_unlock(&m->mutex);
}

static reducer_id_manager *init_reducer_id_manager(hyper_id_t cap) {
    size_t align = sizeof(reducer_id_manager) > 32 ? 64 : 32;
    // To appease tools such as Valgrind and ThreadSanitizer, ensure that the
    // allocated size matches the alignment.
    size_t alloc_size = (sizeof(reducer_id_manager) + align - 1) & ~(align - 1);
    reducer_id_manager *m = cilk_aligned_alloc(align, alloc_size);
    memset(m, 0, sizeof *m);
    cilkrts_alert(BOOT, NULL, "(reducers_init) Initializing reducers");
    cap = (cap + LONG_BIT - 1) / LONG_BIT * LONG_BIT; /* round up */
    CILK_ASSERT_G(cap > 0 && cap < 9999999);
    pthread_mutex_init(&m->mutex, NULL);
    m->spa_cap = cap;
    m->next = 0;
    m->hwm = 0;
    m->used = calloc(cap / LONG_BIT, sizeof(unsigned long));
    m->global = NULL;
    return m;
}

static void free_reducer_id_manager(reducer_id_manager *m) {
    m->spa_cap = 0;
    m->next = 0;
    m->hwm = 0;
    unsigned long *old = m->used;
    if (old) {
        m->used = NULL;
        free(old);
    }
    __cilkrts_hyperobject_base **global = m->global;
    if (global) {
        m->global = NULL;
        free(global);
    }
    free(m);
}

static hyper_id_t reducer_id_get(reducer_id_manager *m, __cilkrts_worker *w) {
    reducer_id_manager_lock(m, w);
    hyper_id_t id = m->next;
    unsigned long mask = 1UL << (id % LONG_BIT);
    unsigned long *used = m->used;
    if ((used[id / LONG_BIT] & mask) == 0) {
        used[id / LONG_BIT] |= mask;
    } else {
        id = ~(hyper_id_t)0;
        hyper_id_t cap = m->spa_cap;
        for (unsigned i = 0; i < cap / LONG_BIT; ++i) {
            if (~used[i]) {
                int index = __builtin_ctzl(~used[i]);
                CILK_ASSERT(w, !(used[i] & (1UL << index)));
                used[i] |= 1UL << index;
                id = i * LONG_BIT + index;
                break;
            }
        }
    }
    cilkrts_alert(REDUCE_ID, w, "allocate reducer ID %lu", (unsigned long)id);
    m->next = id + 1 >= m->spa_cap ? 0 : id + 1;
    if (id >= m->hwm)
        m->hwm = id + 1;
    if (id >= m->spa_cap) {
        cilkrts_bug(w, "SPA resize not supported yet! (cap %lu)",
                    (unsigned long)m->spa_cap);
    }
    reducer_id_manager_unlock(m, w);
    return id;
}

static void reducer_id_free(__cilkrts_worker *const ws, hyper_id_t id) {
    global_state *g = ws ? ws->g : default_cilkrts;
    reducer_id_manager *m = g->id_manager;
    reducer_id_manager_lock(m, ws);
    cilkrts_alert(REDUCE_ID, ws, "free reducer ID %lu of %lu",
                  (unsigned long)id, (unsigned long)m->spa_cap);
    CILK_ASSERT(ws, id < m->spa_cap);
    CILK_ASSERT(ws, m->used[id / LONG_BIT] & (1UL << id % LONG_BIT));
    m->used[id / LONG_BIT] &= ~(1UL << id % LONG_BIT);
    m->next = id;
    if (m->global && id < GLOBAL_REDUCER_LIMIT)
        m->global[id] = NULL;
    reducer_id_manager_unlock(m, ws);
}

// =================================================================
// Init / deinit functions
// =================================================================

void reducers_init(global_state *g) {
    /* TODO: It is safe to grow capacity now while the system
       is single threaded. */
    if (g->id_manager) {
        return;
    } else {
        g->id_manager = init_reducer_id_manager(REDUCER_LIMIT);
    }
}

void reducers_deinit(global_state *g) {
    cilkrts_alert(BOOT, NULL, "(reducers_deinit) Cleaning up reducers");
    free_reducer_id_manager(g->id_manager);
    g->id_manager = NULL;
}

CHEETAH_INTERNAL void reducers_import(global_state *g, __cilkrts_worker *w) {
    reducer_id_manager *m = g->id_manager;
    CILK_ASSERT(w, m);
    if (m->hwm == 0)
        return;
    /* TODO: There may need to be a marker saying that the ID manager
       should be exported when Cilk exits. */
    cilkred_map *map = cilkred_map_make_map(w, m->spa_cap);
    for (hyper_id_t i = 0; i < m->hwm; ++i) {
        __cilkrts_hyperobject_base *h = m->global[i];
        if (h) {
            map->vinfo[i].key = h;
            map->vinfo[i].val = (char *)h + (ptrdiff_t)h->__view_offset;
        }
        hyper_id_t id = h->__id_num;
        CILK_ASSERT(w, id & HYPER_ID_VALID);
        cilkred_map_log_id(w, map, id & ~HYPER_ID_VALID);
    }
    w->reducer_map = map;
}

// =================================================================
// Helper functions for interacting with reducer library and
// managing hyperobjects
// =================================================================

static cilkred_map *install_new_reducer_map(__cilkrts_worker *w) {
    global_state *g = w->g;
    reducer_id_manager *m = g->id_manager;
    cilkred_map *h;
    // MAK: w.out worker mem pools, need to reexamine
    h = cilkred_map_make_map(w, m->spa_cap);
    w->reducer_map = h;

    cilkrts_alert(REDUCE, w, "installed reducer map %p", (void *)h);
    return h;
}

/* remove the reducer from the current reducer map.  If the reducer
   exists in maps other than the current one, the behavior is
   undefined. */
void __cilkrts_hyper_destroy(__cilkrts_hyperobject_base *key) {

    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    // If we don't have a worker, use instead the last exiting worker from the
    // default CilkRTS.
    if (!w)
        w = default_cilkrts->workers[default_cilkrts->exiting_worker];

    hyper_id_t id = key->__id_num;
    cilkrts_alert(REDUCE_ID, w, "Destroy reducer %x at %p", (unsigned)id, key);
    if (!__builtin_expect(id & HYPER_ID_VALID, HYPER_ID_VALID)) {
        cilkrts_bug(w, "unregistering unregistered hyperobject %p", key);
        return;
    }
    id &= ~HYPER_ID_VALID;
    key->__id_num = id;

    if (w) {
#define UNSYNCED_REDUCER_MSG                                                   \
    "Destroying a reducer while it is visible to unsynced child tasks, or\n"   \
    "calling CILK_C_UNREGISTER_REDUCER() on an unregistered reducer.\n"        \
    "Did you forget a _Cilk_sync or CILK_C_REGISTER_REDUCER()?"

        cilkred_map *h = w->reducer_map;
        if (NULL == h)
            cilkrts_bug(w, UNSYNCED_REDUCER_MSG); // Does not return
        if (h->merging)
            cilkrts_bug(w,
                        "User error: hyperobject used by another hyperobject");
        cilkred_map_unlog_id(w, h, id);
    }
    reducer_id_free(w, id);
}

void __cilkrts_hyper_create(__cilkrts_hyperobject_base *key) {
    // This function registers the specified hyperobject in the current
    // reducer map and registers the initial value of the hyperobject as the
    // leftmost view of the reducer.
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    reducer_id_manager *m = NULL;

    if (__builtin_expect(!w, 0)) {
        // Use the ID manager of the last exiting worker from the default
        // CilkRTS.
        m = default_cilkrts->id_manager;
        w = default_cilkrts->workers[default_cilkrts->exiting_worker];
    } else {
        m = w->g->id_manager;
    }

    hyper_id_t id = reducer_id_get(m, w);
    key->__id_num = id | HYPER_ID_VALID;

    cilkrts_alert(REDUCE_ID, w, "Create reducer %x at %p", (unsigned)id, key);

    if (__builtin_expect(!w, 0)) {
        if (id >= GLOBAL_REDUCER_LIMIT) {
            cilkrts_bug(w, "Global reducer pool exhausted");
        }
        if (!m->global) {
            m->global = calloc(GLOBAL_REDUCER_LIMIT, sizeof *m->global);
        }
        m->global[id] = key;
        return;
    }

    cilkred_map *h = w->reducer_map;

    if (__builtin_expect(!h, 0)) {
        h = install_new_reducer_map(w);
    }

    /* Must not exist. */
    CILK_ASSERT(w, cilkred_map_lookup(h, key) == NULL);

    if (h->merging)
        cilkrts_bug(w, "User error: hyperobject used by another hyperobject");

    CILK_ASSERT(w, w->reducer_map == h);

    ViewInfo *vinfo = &h->vinfo[id];
    vinfo->key = key;
    // init with left most view
    vinfo->val = (char *)key + (ptrdiff_t)key->__view_offset;
    cilkred_map_log_id(w, h, id);

    static_assert(sizeof(__cilkrts_hyperobject_base) <= 64,
                  "hyperobject base is too large");
}

void *__cilkrts_hyper_lookup(__cilkrts_hyperobject_base *key) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    hyper_id_t id = key->__id_num;

    if (!__builtin_expect(id & HYPER_ID_VALID, HYPER_ID_VALID)) {
        cilkrts_bug(w, "User error: reference to unregistered hyperobject %p",
                    key);
    }
    id &= ~HYPER_ID_VALID;

    if (__builtin_expect(!w, 0)) {
        return (char *)key + key->__view_offset;
    }

    /* TODO: If this is the first reference to a reducer created at
       global scope, install the leftmost view. */

    if (w->g->options.force_reduce) {
        CILK_ASSERT(w, w->g->nworkers == 1);
        promote_own_deque(w);
    }

    cilkred_map *h = w->reducer_map;

    if (__builtin_expect(!h, 0)) {
        h = install_new_reducer_map(w);
    }

    if (h->merging)
        cilkrts_bug(w, "User error: hyperobject used by another hyperobject");

    ViewInfo *vinfo = cilkred_map_lookup(h, key);
    if (vinfo == NULL) {
        CILK_ASSERT(w, id < h->spa_cap);
        vinfo = &h->vinfo[id];
        CILK_ASSERT(w, vinfo->key == NULL && vinfo->val == NULL);

        void *val = key->__c_monoid.allocate_fn(key, key->__view_size);
        key->__c_monoid.identity_fn(key, val);

        // allocate space for the val and initialize it to identity
        vinfo->key = key;
        vinfo->val = val;
        cilkred_map_log_id(w, h, id);
    }
    return vinfo->val;
}

void *__cilkrts_hyper_alloc(__cilkrts_hyperobject_base *key, size_t bytes) {
    if (USE_INTERNAL_MALLOC) {
        __cilkrts_worker *w = __cilkrts_get_tls_worker();
        if (!w)
            // Use instead the worker from the default CilkRTS that last exited
            // a Cilkified region
            w = default_cilkrts->workers[default_cilkrts->exiting_worker];
        return cilk_internal_malloc(w, bytes, IM_REDUCER_MAP);
    } else
        return cilk_aligned_alloc(16, bytes);
}

void __cilkrts_hyper_dealloc(__cilkrts_hyperobject_base *key, void *view) {
    if (USE_INTERNAL_MALLOC) {
        __cilkrts_worker *w = __cilkrts_get_tls_worker();
        if (!w)
            // Use instead the worker from the default CilkRTS that last exited
            // a Cilkified region
            w = default_cilkrts->workers[default_cilkrts->exiting_worker];
        cilk_internal_free(w, view, key->__view_size, IM_REDUCER_MAP);
    } else
        free(view);
}

// =================================================================
// Helper function for the scheduler
// =================================================================

#if DL_INTERPOSE
#define START_DL_INTERPOSABLE(func, type)                               \
    if (__builtin_expect(dl_##func == NULL, false)) {                   \
        dl_##func = (type)dlsym(RTLD_DEFAULT, #func);                   \
        if (__builtin_expect(dl_##func == NULL, false)) {               \
            char *error = dlerror();                                    \
            if (error != NULL) {                                        \
                fputs(error, stderr);                                   \
                fflush(stderr);                                         \
                abort();                                                \
            }                                                           \
        }                                                               \
    }

typedef cilkred_map *(*merge_two_rmaps_t)(__cilkrts_worker *const,
                                          cilkred_map *,
                                          cilkred_map *);
static merge_two_rmaps_t dl___cilkrts_internal_merge_two_rmaps = NULL;

cilkred_map *merge_two_rmaps(__cilkrts_worker *const ws,
                             cilkred_map *left,
                             cilkred_map *right) {
    START_DL_INTERPOSABLE(__cilkrts_internal_merge_two_rmaps,
                          merge_two_rmaps_t);

    return dl___cilkrts_internal_merge_two_rmaps(ws, left, right);
}
#endif // DL_INTERPOSE

cilkred_map *__cilkrts_internal_merge_two_rmaps(__cilkrts_worker *const ws,
                                                cilkred_map *left,
                                                cilkred_map *right) {
    CILK_ASSERT(ws, ws == __cilkrts_get_tls_worker());
    if (!left)
        return right;
    if (!right)
        return left;

    /* Special case, if left is leftmost, then always merge into it.
       For C reducers this forces lazy creation of the leftmost views. */
    if (cilkred_map_is_leftmost(left) ||
        cilkred_map_num_views(left) > cilkred_map_num_views(right)) {
        cilkred_map_merge(left, ws, right, MERGE_INTO_LEFT);
        return left;
    } else {
        cilkred_map_merge(right, ws, left, MERGE_INTO_RIGHT);
        return right;
    }
}
