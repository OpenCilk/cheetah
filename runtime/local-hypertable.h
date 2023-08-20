#ifndef _LOCAL_HYPERTABLE_H
#define _LOCAL_HYPERTABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "hyperobject_base.h"
#include "rts-config.h"
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
static inline bool is_valid(uintptr_t key) {
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

hyper_table *__cilkrts_local_hyper_table_alloc();
CHEETAH_INTERNAL
void local_hyper_table_free(hyper_table *table);

CHEETAH_INTERNAL
bool remove_hyperobject(hyper_table *table, uintptr_t key);
CHEETAH_INTERNAL
bool insert_hyperobject(hyper_table *table, struct bucket b);

CHEETAH_INTERNAL
hyper_table *merge_two_hts(__cilkrts_worker *restrict w,
                           hyper_table *restrict left,
                           hyper_table *restrict right);

// Data type for indexing the hash table.  This type is used for
// hashes as well as the table's capacity.
static const int32_t MIN_CAPACITY = 4;
static const int32_t MIN_HT_CAPACITY = 8;

// Some random numbers for the hash.
/* uint64_t seed = 0xe803e76341ed6d51UL; */
static const uint64_t salt = 0x96b9af4f6a40de92UL;

static inline index_t hash(uintptr_t key_in) {
    uint64_t x = key_in ^ salt;
    // mix64 from SplitMix.
    x = (x ^ (x >> 33)) * 0xff51afd7ed558ccdUL;
    x = (x ^ (x >> 33)) * 0xc4ceb9fe1a85ec53UL;
    return x;
}

static inline index_t get_table_entry(int32_t capacity, uintptr_t key) {
    // Assumes capacity is a power of 2.
    return hash(key) & (capacity - 1);
}

static inline index_t inc_index(index_t i, index_t capacity) {
    ++i;
    if (i == capacity)
        i = 0;
    return i;
}

static inline bool continue_search(index_t tgt, index_t hash,
                                   index_t init_hash) {
    // NOTE: index_t must be unsigned for this check to work.
    index_t norm_tgt = tgt - init_hash;
    index_t norm_hash = hash - init_hash;
    return norm_tgt >= norm_hash;
}

static inline bool stop_insert_scan(index_t tgt, index_t hash,
                                    index_t init_hash) {
    // NOTE: index_t must be unsigned for this check to work.
    index_t norm_tgt = tgt - init_hash;
    index_t norm_hash = hash - init_hash;
    return norm_tgt <= norm_hash;
}

static inline struct bucket *find_hyperobject_linear(hyper_table *table,
                                                     uintptr_t key) {
    // If the table is small enough, just scan the array.
    struct bucket *buckets = table->buckets;
    int32_t occupancy = table->occupancy;

    // Scan the array backwards, since inserts add new entries to
    // the end of the array, and we anticipate that the program
    // will exhibit locality of reference.
    for (int32_t i = occupancy - 1; i >= 0; --i)
        if (buckets[i].key == key)
            return &buckets[i];

    return NULL;
}

struct bucket *__cilkrts_find_hyperobject_hash(hyper_table *table,
                                               uintptr_t key);

static inline struct bucket *find_hyperobject(hyper_table *table,
                                              uintptr_t key) {
    if (table->capacity < MIN_HT_CAPACITY) {
        return find_hyperobject_linear(table, key);
    } else {
        return __cilkrts_find_hyperobject_hash(table, key);
    }
}

void *__cilkrts_insert_new_view(hyper_table *table, uintptr_t key, size_t size,
                                __cilk_identity_fn identity,
                                __cilk_reduce_fn reduce);

#endif // _LOCAL_HYPERTABLE_H
