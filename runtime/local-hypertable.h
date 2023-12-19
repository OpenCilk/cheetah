#ifndef _LOCAL_HYPERTABLE_H
#define _LOCAL_HYPERTABLE_H

#include <stdbool.h>
#include <stdint.h>

#include "hyperobject_base.h"
#include "rts-config.h"
#include "types.h"

typedef uint32_t index_t;

// An entry in the hash table.
struct bucket {
    uintptr_t key; /* EMPTY, DELETED, or a user-provided pointer. */
    index_t hash;  /* hash of the key when inserted into the table. */
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

// Hash table of reducers.  We don't need any locking or support for
// concurrent updates, since the hypertable is local.
typedef struct local_hyper_table {
    index_t capacity;
    int32_t occupancy;
    int32_t ins_rm_count;
    struct bucket *buckets;
} hyper_table;

hyper_table *__cilkrts_local_hyper_table_alloc(void);
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

#ifndef MOCK_HASH
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
#else
#include "mock-local-hypertable-hash.h"
#endif

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

// Searching for an element (or its insertion point) requires handling four
// conditions based on:
//
// - tgt: the target index of the item being inserted
// - i: the current index in the hash table being examined in the search
// - hash: the target index of the item at index i.
//
// Generally speaking, items that hash to the same index appear next to each
// other in the table, and items that hash to adjacent indices (modulo the
// table's capacity) appear next to each other in sorted order based on the
// indices they hash to.  These invariants hold with the exception that
// tombstones can exist between items in the table that would otherwise be
// adjacent.  Let a _run_ be a sequence of hash values for consecutive valid
// entries in the table (modulo the table's capacity).
//
// The search starts with i == tgt and gradually increases i (mod capacity).
// The search must handle the following 4 conditions:
//
// - Non-wrapped search (NS): tgt <= i
// - Wrapped search (WS):     tgt > i
// - Non-wrapped run (NR):    hash <= i
// - Wrapped run (WR):        hash > i
//
// These conditions lead to 4 cases:
//
// - NS+NR: hash <= tgt <= i:
//   Common case.  Search terminates when hash > tgt.
// - WS+WR: i < hash <= tgt:
//   Like NS+NR, search terminates when hash > tgt.
// - NS+WR: tgt <= i < hash:
//   The search needs to continue, meaning it needs to treat tgt as larger than
//   hash.
// - WS+NR: hash <= i < tgt:
//   The search needs to stop, meaning it needs to treat hash as larger than
//   tgt.
//
// Consider i-tgt and i-hash using unsigned arithmetic.
// - In NS+NR and WS+WR cases, search terminates when i-tgt > i-hash.
// - In NS+WR case, i-hash wraps, so i-tgt < i-hash ~> search continues.
// - In WS+NR case, i-tgt wraps, so i-tgt > i-hash ~> search terminates.

// NOTE: I prefer to think about i-tgt and i-hash, because these will be
// small positive values in the common case.

static inline bool continue_search(index_t tgt, index_t hash,
                                   index_t idx) {
    // NOTE: index_t must be unsigned for this check to work.
    return (idx - tgt) <= (idx - hash);
}

// Insert scans stop under slightly different conditions from !continue_search,
// specifically, when tgt == hash.
static inline bool stop_insert_scan(index_t tgt, index_t hash,
                                    index_t idx) {
    // NOTE: index_t must be unsigned for this check to work.
    return (idx - tgt) >= (idx - hash);
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
