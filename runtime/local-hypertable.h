#ifndef _LOCAL_HYPERTABLE_H
#define _LOCAL_HYPERTABLE_H

#include <stdbool.h>
#include <stdint.h>

// #include <cilk/cilk_api.h>
#include "hyperobject_base.h"
#include "types.h"

// An entry in the hash table.
struct bucket {
    uintptr_t key; /* EMPTY, DELETED, or a user-provided pointer. */
    reducer_base value;
};

// Helper methods for testing and setting keys.
static const uintptr_t KEY_EMPTY = 0UL;
static const uintptr_t KEY_DELETED = ~0UL;

static bool is_empty(uintptr_t key) { return key == KEY_EMPTY; }
static bool is_tombstone(uintptr_t key) { return key == KEY_DELETED; }
static bool is_valid(uintptr_t key) {
    return !is_empty(key) && !is_tombstone(key);
}

typedef uint32_t index_t;

// Hash table of reducers.  We don't need any locking or support for
// concurrent updates, since the hypertable is local.
typedef struct local_hyper_table {
    index_t capacity;
    int32_t occupancy;
    int32_t ins_rm_count;
    struct bucket *buckets;
} hyper_table;

void local_hyper_table_init(hyper_table *table);
void local_hyper_table_destroy(hyper_table *table);

struct bucket *find_hyperobject(hyper_table *table, uintptr_t key);
bool remove_hyperobject(hyper_table *table, uintptr_t key);
bool insert_hyperobject(hyper_table *table, struct bucket b);

hyper_table *merge_two_hts(__cilkrts_worker *restrict w,
                           hyper_table *restrict left,
                           hyper_table *restrict right);

#endif // _LOCAL_HYPERTABLE_H
