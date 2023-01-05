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

void local_hyper_table_init(hyper_table *table);
void local_hyper_table_destroy(hyper_table *table);

/* struct bucket *find_hyperobject(hyper_table *table, uintptr_t key); */
bool remove_hyperobject(hyper_table *table, uintptr_t key);
bool insert_hyperobject(hyper_table *table, struct bucket b);

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
    /* uint64_t x = (uint32_t)(key_in ^ salt) | (((key_in ^ seed) >> 32) << 32);
     */
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

static inline bool continue_search(index_t tgt, index_t hash, index_t i,
                                   uintptr_t tgt_key, uintptr_t key) {
    // Continue the search if the current hash is smaller than the
    // target or if the hashes match and the current key is smaller
    // than the target key.
    //
    // It's possible that the current hash is larger than the target
    // because it belongs to a run that wraps from the end of the
    // table to the beginning.  We want to treat such hashes as
    // smaller than the target, unless the target itself is part of
    // such a wrapping run.  To detect such cases, check that the
    // target is smaller than the current index i --- meaning the
    // search has not wrapped --- and the current hash is larger than
    // i --- meaning the current hash is part of a wrapped run.
    return hash <= tgt || // (hash == tgt && key < tgt_key) ||
           (tgt < i && hash > i);
}

static inline struct bucket *find_hyperobject(hyper_table *table,
                                              uintptr_t key) {
    int32_t capacity = table->capacity;
    if (capacity < MIN_HT_CAPACITY) {
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

    // Target hash
    index_t tgt = get_table_entry(capacity, key);
    struct bucket *buckets = table->buckets;
    // Start the search at the target hash
    index_t i = tgt;
    do {
        uintptr_t curr_key = buckets[i].key;
        // If we find the key, return that bucket.
        // TODO: Consider moving this bucket to the front of the run.
        if (key == curr_key)
            return &buckets[i];

        // If we find an empty entry, the search failed.
        if (is_empty(curr_key))
            return NULL;

        // If we find a tombstone, continue the search.
        if (is_tombstone(curr_key)) {
            i = inc_index(i, capacity);
            continue;
        }

        // Otherwise we have another valid key that does not match.
        // Compare the hashes to decide whether or not to continue the
        // search.
        index_t curr_hash = get_table_entry(capacity, curr_key);
        if (continue_search(tgt, curr_hash, i, key, curr_key)) {
            i = inc_index(i, capacity);
            continue;
        }

        // If none of the above cases match, then the search failed to
        // find the key.
        return NULL;
    } while (i != tgt);

    // The search failed to find the key.
    return NULL;
}

#endif // _LOCAL_HYPERTABLE_H
