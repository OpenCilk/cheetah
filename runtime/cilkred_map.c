#include "cilkred_map.h"

#include <stdatomic.h>

// =================================================================
// small helper functions
// =================================================================

static inline void swap_views(ViewInfo *v1, ViewInfo *v2) {
    ViewInfo tmp = *v1;
    *v1 = *v2;
    *v2 = tmp;
}

static inline void swap_vals(ViewInfo *v1, ViewInfo *v2) {
    void *val = v1->val;
    v1->val = v2->val;
    v2->val = val;
}

static inline void clear_view(ViewInfo *view) {
    __cilkrts_hyperobject_base *key = view->key;

    if (key != NULL) {
        cilk_destroy_fn_t destroy = key->__c_monoid.destroy_fn;
        if (destroy) {
            key->__c_monoid.destroy_fn(key, view->val); // calls destructor
        }
        key->__c_monoid.deallocate_fn(key, view->val); // free the memory
    }
    view->key = NULL;
    view->val = NULL;
}

// =================================================================
// helper functions that operate on a SPA map
// =================================================================

void cilkred_map_log_id(__cilkrts_worker *const w, cilkred_map *this_map,
                        hyper_id_t id) {
    CILK_ASSERT(w, this_map->num_of_logs <= ((this_map->spa_cap / 2) + 1));
    CILK_ASSERT(w, this_map->num_of_vinfo <= this_map->spa_cap);

    if (this_map->num_of_vinfo == this_map->spa_cap) {
        cilkrts_bug(w, "SPA resize not supported yet! (vinfo = spa_cap = %lu)",
                    (unsigned long)this_map->spa_cap);
    }

    if (this_map->num_of_logs < (this_map->spa_cap / 2)) {
        this_map->log[this_map->num_of_logs++] = id;
    } else if (this_map->num_of_logs == (this_map->spa_cap / 2)) {
        this_map->num_of_logs++; // invalidate the log
    }

    this_map->num_of_vinfo++;
}

void cilkred_map_unlog_id(__cilkrts_worker *const w, cilkred_map *this_map,
                          hyper_id_t id) {
    CILK_ASSERT(w, this_map->num_of_logs <= ((this_map->spa_cap / 2) + 1));
    CILK_ASSERT(w, this_map->num_of_vinfo <= this_map->spa_cap);
    CILK_ASSERT(w, id < this_map->spa_cap);

    this_map->vinfo[id].key = NULL;
    this_map->vinfo[id].val = NULL;

    this_map->num_of_vinfo--;
    if (this_map->num_of_vinfo == 0) {
        this_map->num_of_logs = 0; // now we can reset the log
    }
}

/** @brief Return element mapped to 'key' or null if not found. */
ViewInfo *cilkred_map_lookup(cilkred_map *this_map,
                             __cilkrts_hyperobject_base *key) {
    hyper_id_t id = key->__id_num;
    if (__builtin_expect(!(id & HYPER_ID_VALID), 0)) {
        return NULL;
    }
    id &= ~HYPER_ID_VALID;
    if (id >= this_map->spa_cap) {
        return NULL; /* TODO: grow map */
    }
    ViewInfo *ret = this_map->vinfo + id;
    if (ret->key == NULL && ret->val == NULL) {
        return NULL;
    }

    return ret;
}

/**
 * Construct an empty reducer map from the memory pool associated with the
 * given worker.  This reducer map must be destroyed before the worker's
 * associated global context is destroyed.
 *
 * @param w __cilkrts_worker the cilkred_map is being created for.
 *
 * @return Pointer to the initialized cilkred_map.
 */
cilkred_map *cilkred_map_make_map(__cilkrts_worker *w, size_t size) {
    CILK_ASSERT_G(w);
    CILK_ASSERT(w, size > 0 && (hyper_id_t)size == size);

    cilkred_map *h =
        (cilkred_map *)cilk_internal_malloc(w, sizeof(*h), IM_REDUCER_MAP);

    // MAK: w is not NULL
    h->spa_cap = size;
    h->num_of_vinfo = 0;
    h->num_of_logs = 0;
    h->merging = false;
    h->vinfo = (ViewInfo *)calloc(size, sizeof(ViewInfo));
    h->log = (hyper_id_t *)calloc(size / 2, sizeof(hyper_id_t));

    cilkrts_alert(REDUCE, w, "created reducer map size %zu %p", size,
                  (void *)h);

    return h;
}

/**
 * Destroy a reducer map.  The map must have been allocated from the worker's
 * global context and should have been allocated from the same worker.
 *
 * @param w __cilkrts_worker the cilkred_map was created for.
 * @param h The cilkred_map to be deallocated.
 */
void cilkred_map_destroy_map(__cilkrts_worker *w, cilkred_map *h) {
    if (!h) {
        return;
    }
    if (DEBUG_ENABLED(REDUCER)) {
        for (hyper_id_t i = 0; i < h->spa_cap; ++i)
            CILK_ASSERT(w, !h->vinfo[i].val);
    }
    free(h->vinfo);
    h->vinfo = NULL;
    free(h->log);
    h->log = NULL;
    cilk_internal_free(w, h, sizeof(*h), IM_REDUCER_MAP);

    cilkrts_alert(REDUCE, w, "freed reducer map %p", (void *)h);
}

/* This function is responsible for freeing other_map. */
void cilkred_map_merge(cilkred_map *this_map, __cilkrts_worker *w,
                       cilkred_map *other_map, merge_kind kind) {
    cilkrts_alert(REDUCE, w, "merging reducer map %p into %p, order %d",
                  (void *)other_map, (void *)this_map, kind);
    // Remember the current stack frame.
    // __cilkrts_stack_frame *current_sf = w->current_stack_frame;
    this_map->merging = true;
    other_map->merging = true;

    // Merging to the leftmost view is a special case because every leftmost
    // element must be initialized before the merge.
    // CILK_ASSERT(w, !other_map->is_leftmost /* || kind == MERGE_UNORDERED */);
    // bool merge_to_leftmost = (this_map->is_leftmost);

    if (other_map->num_of_vinfo == 0) {
        cilkred_map_destroy_map(w, other_map);
        return;
    }

    if (other_map->num_of_logs <= (other_map->spa_cap / 2)) {
        hyper_id_t i;

        for (i = 0; i < other_map->num_of_logs; i++) {
            hyper_id_t vindex = other_map->log[i];
            __cilkrts_hyperobject_base *key = other_map->vinfo[vindex].key;

            if (this_map->vinfo[vindex].key != NULL) {
                CILK_ASSERT(w, key == this_map->vinfo[vindex].key);
                if (kind == MERGE_INTO_RIGHT) { // other_map is the left val
                    swap_vals(&other_map->vinfo[vindex],
                              &this_map->vinfo[vindex]);
                }
                // updated val is stored back into the left
                key->__c_monoid.reduce_fn(key, this_map->vinfo[vindex].val,
                                          other_map->vinfo[vindex].val);
                clear_view(&other_map->vinfo[vindex]);
            } else {
                CILK_ASSERT(w, this_map->vinfo[vindex].val == NULL);
                swap_views(&other_map->vinfo[vindex], &this_map->vinfo[vindex]);
                cilkred_map_log_id(w, this_map, vindex);
            }
        }

    } else {
        hyper_id_t i;
        for (i = 0; i < other_map->spa_cap; i++) {
            if (other_map->vinfo[i].key != NULL) {
                __cilkrts_hyperobject_base *key = other_map->vinfo[i].key;

                if (this_map->vinfo[i].key != NULL) {
                    CILK_ASSERT(w, key == this_map->vinfo[i].key);
                    if (kind == MERGE_INTO_RIGHT) { // other_map is the left val
                        swap_vals(&other_map->vinfo[i], &this_map->vinfo[i]);
                    }
                    // updated val is stored back into the left
                    key->__c_monoid.reduce_fn(key, this_map->vinfo[i].val,
                                              other_map->vinfo[i].val);
                    clear_view(&other_map->vinfo[i]);
                } else { // the 'this_map' page does not contain view
                    CILK_ASSERT(w, this_map->vinfo[i].val == NULL);
                    // transfer the key / val over
                    swap_views(&other_map->vinfo[i], &this_map->vinfo[i]);
                    cilkred_map_log_id(w, this_map, i);
                }
            }
        }
    }
    other_map->num_of_vinfo = 0;
    other_map->num_of_logs = 0;

    // this_map->is_leftmost = this_map->is_leftmost || other_map->is_leftmost;
    this_map->merging = false;
    other_map->merging = false;
    cilkred_map_destroy_map(w, other_map);
    return;
}

/** @brief Test whether the cilkred_map is empty */
bool cilkred_map_is_empty(cilkred_map *this_map) {
    return this_map->num_of_vinfo == 0;
}

/** @brief Get number of views in the cilkred_map */
size_t cilkred_map_num_views(cilkred_map *this_map) {
    return this_map->num_of_vinfo;
}

/** @brief Is the cilkred_map leftmost */
bool cilkred_map_is_leftmost(cilkred_map *this_map) { return false; }
