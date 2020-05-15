#ifdef REDUCER_MODULE
#include "reducer_impl.h"
#endif
#include "cilk/hyperobject_base.h"
#include "mutex.h"

#ifdef REDUCER_MODULE
// =================================================================
// ID managers for reducers
// =================================================================

/* This structure may need to exist before Cilk is started.
 */
typedef struct reducer_id_manager {
    pthread_mutex_t mutex; // enfore mutual exclusion on access to this desc
    int mutex_owner;       // worker id who holds the mutex
    hyper_id_t spa_cap;
    hyper_id_t curr_id;
} reducer_id_manager;

static reducer_id_manager id_manager = {PTHREAD_MUTEX_INITIALIZER, NOBODY, 1000,
                                        0};

static void reducer_id_manager_assert_ownership(__cilkrts_worker *const ws) {
    CILK_ASSERT(ws, !ws || id_manager.mutex_owner == ws->self);
}

static inline void reducer_id_manager_lock(__cilkrts_worker *const w) {
    int error = pthread_mutex_lock(&id_manager.mutex);
    if (error == 0) {
        id_manager.mutex_owner = w ? w->self : NOBODY;
    } else {
        cilkrts_bug(w, "unable to lock reducer ID manager");
    }
}

static void reducer_id_manager_unlock(__cilkrts_worker *const ws) {
    reducer_id_manager_assert_ownership(ws);
    id_manager.mutex_owner = NOBODY;
    pthread_mutex_unlock(&id_manager.mutex);
}

static void init_reducer_id_manager(global_state *const g) {
    /* Currently nothing to do */
}

static void free_reducer_id_manager(global_state *const g) {
    id_manager.spa_cap = 1000;
    id_manager.curr_id = 0;
}

static hyper_id_t reducer_id_get(__cilkrts_worker *ws) {
    reducer_id_manager_lock(ws);
    hyper_id_t id = id_manager.curr_id++;
    if (id >= id_manager.spa_cap) {
        cilkrts_bug(ws, "SPA resize not supported yet!");
    }
    reducer_id_manager_unlock(ws);
    return id;
}

static void reducer_id_free(__cilkrts_worker *const ws, hyper_id_t id) {
    reducer_id_manager_lock(ws);
    /* Return the ID to the system to be reallocated.
       TODO: This only works if reducer lifetimes are nested. */
    if (id == id_manager.curr_id - 1) {
        id_manager.curr_id = id;
    }
    reducer_id_manager_unlock(ws);
}

// =================================================================
// Init / deinit functions
// =================================================================

void reducers_init(global_state *g) {
    cilkrts_alert(ALERT_BOOT, NULL, "(reducers_init) Initializing reducers");
    init_reducer_id_manager(g);
}

void reducers_deinit(global_state *g) {
    cilkrts_alert(ALERT_BOOT, NULL, "(reducers_deinit) Cleaning up reducers");
    free_reducer_id_manager(g);
}

// =================================================================
// Helper functions for interacting with reducer library and
// managing hyperobjects
// =================================================================

static cilkred_map *install_new_reducer_map(__cilkrts_worker *w) {
    cilkred_map *h;
    // MAK: w.out worker mem pools, need to reexamine
    h = cilkred_map_make_map(w, id_manager.spa_cap);
    w->reducer_map = h;

    cilkrts_alert(ALERT_REDUCE, w,
                  "(install_new_reducer_map) installed reducer_map %p", h);
    return h;
}

#if 0 /* unused? */
// Given a __cilkrts_hyperobject_base, return the key to that hyperobject in
// the reducer map.
void *get_hyperobject_val(__cilkrts_hyperobject_base *hb) {
    // The current implementation uses the address of the lefmost view as the
    // key.
    return ((char *)hb) + hb->__view_offset;
}
#endif

/* remove the reducer from the current reducer map.  If the reducer
   exists in maps other than the current one, the behavior is
   undefined. */
void __cilkrts_hyper_destroy(__cilkrts_hyperobject_base *key) {

    __cilkrts_worker *w = __cilkrts_get_tls_worker();

    hyper_id_t id = key->__id_num;
    if (!__builtin_expect(id & HYPER_ID_VALID, HYPER_ID_VALID)) {
        cilkrts_bug(w, "unregistering unregistered hyperobject");
        return;
    }
    id &= ~HYPER_ID_VALID;
    key->__id_num = id;

    if (!w) {
        reducer_id_free(w, id);
        return;
    }

    const char *UNSYNCED_REDUCER_MSG =
        "Destroying a reducer while it is visible to unsynced child tasks, or\n"
        "calling CILK_C_UNREGISTER_REDUCER() on an unregistered reducer.\n"
        "Did you forget a _Cilk_sync or CILK_C_REGISTER_REDUCER()?";

    cilkred_map *h = w->reducer_map;
    if (NULL == h)
        cilkrts_bug(w, UNSYNCED_REDUCER_MSG); // Does not return

    if (h->merging) {
        cilkrts_bug(w, "User error: hyperobject used by another hyperobject");
    }

    cilkred_map_unlog_id(w, h, id);
    reducer_id_free(w, id);
}

void __cilkrts_hyper_create(__cilkrts_hyperobject_base *key) {
    // This function registers the specified hyperobject in the current
    // reducer map and registers the initial value of the hyperobject as the
    // leftmost view of the reducer.
    __cilkrts_worker *w = __cilkrts_get_tls_worker();

    hyper_id_t id = reducer_id_get(w);
    key->__id_num = id | HYPER_ID_VALID;

    if (!w)
        return;

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
    vinfo->val = (char *)key + key->__view_offset; // init with left most view
    cilkred_map_log_id(w, h, id);

    static_assert(sizeof(__cilkrts_hyperobject_base) <= __CILKRTS_CACHE_LINE__,
                  "hyperobject base is too large");
}

void *__cilkrts_hyper_lookup(__cilkrts_hyperobject_base *key) {
    // MAK: Requires runtime started
    __cilkrts_worker *w = __cilkrts_get_tls_worker();

    hyper_id_t id = key->__id_num;

    if (!__builtin_expect(id & HYPER_ID_VALID, HYPER_ID_VALID)) {
        cilkrts_bug(w, "User error: reference to unregistered hyperobject");
    }
    id &= ~HYPER_ID_VALID;

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

        void *val = key->__c_monoid.allocate_fn(key, key->__view_size);
        key->__c_monoid.identity_fn(key, val);
        CILK_ASSERT(w, vinfo->key == NULL && vinfo->val == NULL);

        // allocate space for the val and initialize it to identity
        vinfo->key = key;
        vinfo->val = val;
        cilkred_map_log_id(w, h, id);
    }
    return vinfo->val;
}

void *__cilkrts_hyperobject_alloc(void *ignore, size_t bytes) {
    return malloc(bytes);
}

void __cilkrts_hyperobject_dealloc(void *ignore, void *view) { free(view); }

/* No-op destroy function */
void __cilkrts_hyperobject_noop_destroy(void *ignore, void *ignore2) {}

// =================================================================
// Helper function for the scheduler
// =================================================================

cilkred_map *merge_two_rmaps(__cilkrts_worker *const ws, cilkred_map *left,
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
#endif
