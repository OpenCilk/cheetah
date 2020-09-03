#ifndef _CILKRED_MAP_H
#define _CILKRED_MAP_H

#include "cilk-internal.h"
#include "debug.h"
#include <cilk/hyperobject_base.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t hyper_id_t; /* must match cilk/hyperobject_base.h */
#define HYPER_ID_VALID 0x80000000

enum merge_kind {
    MERGE_UNORDERED, ///< Assertion fails
    MERGE_INTO_LEFT, ///< Merges the argument from the right into the left
    MERGE_INTO_RIGHT ///< Merges the argument from the left into the right
};
typedef enum merge_kind merge_kind;

typedef struct view_info {
    void *val; // pointer to the actual view for the reducer
    // pointer to the hyperbase object for a given reducer
    __cilkrts_hyperobject_base *key;
} ViewInfo;

/**
 * Class that implements the map for reducers so we can find the
 * view for a strand.
 */
struct cilkred_map {
    hyper_id_t spa_cap;
    hyper_id_t num_of_vinfo; // max is spa_cap
    hyper_id_t num_of_logs;  // max is spa_cap / 2
    /** Set true if merging (for debugging purposes) */
    bool merging;
    hyper_id_t *log;
    // SPA structure, may be reallocated when entries are added or removed
    ViewInfo *vinfo;
};
typedef struct cilkred_map cilkred_map;

CHEETAH_INTERNAL
void cilkred_map_log_id(__cilkrts_worker *const w, cilkred_map *this_map,
                        hyper_id_t id);
CHEETAH_INTERNAL
void cilkred_map_unlog_id(__cilkrts_worker *const w, cilkred_map *this_map,

                          hyper_id_t id);

/* Calling this function potentially invalidates any older ViewInfo pointers
   from the same map. */
CHEETAH_INTERNAL
ViewInfo *cilkred_map_lookup(cilkred_map *this_map,
                             __cilkrts_hyperobject_base *key);
/**
 * Construct an empty reducer map from the memory pool associated with the
 * given worker.  This reducer map must be destroyed before the worker's
 * associated global context is destroyed.
 *
 * @param w __cilkrts_worker the cilkred_map is being created for.
 *
 * @return Pointer to the initialized cilkred_map.
 */
CHEETAH_INTERNAL
cilkred_map *cilkred_map_make_map(__cilkrts_worker *w, size_t size);

/**
 * Destroy a reducer map.  The map must have been allocated from the worker's
 * global context and should have been allocated from the same worker.
 *
 * @param w __cilkrts_worker the cilkred_map was created for.
 * @param h The cilkred_map to be deallocated.
 */
CHEETAH_INTERNAL
void cilkred_map_destroy_map(__cilkrts_worker *w, cilkred_map *h);

/**
 * Merge other_map into this_map and destroy other_map.
 */
CHEETAH_INTERNAL
void cilkred_map_merge(cilkred_map *this_map, __cilkrts_worker *w,
                       cilkred_map *other_map, merge_kind kind);

/** @brief Test whether the cilkred_map is empty */
CHEETAH_INTERNAL
bool cilkred_map_is_empty(cilkred_map *this_map);

/** @brief Get number of views in the cilkred_map */
CHEETAH_INTERNAL
size_t cilkred_map_num_views(cilkred_map *this_map);

/** @brief Is the cilkred_map leftmost */
CHEETAH_INTERNAL
bool cilkred_map_is_leftmost(cilkred_map *this_map);

#endif
