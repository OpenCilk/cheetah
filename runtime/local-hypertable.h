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
    // mix based on abseil's low-level hash, to convert 64-bit integers into
    // 32-bit integers.
    const size_t half_bits = sizeof(uintptr_t) * 4;
    const uintptr_t low_mask = ((uintptr_t)(1) << half_bits) - 1;
    uintptr_t v = (x & low_mask) * (x >> half_bits);
    return (v & low_mask) ^ (v >> half_bits);
}
#else
#include MOCK_HASH
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

// For theoretical and practical efficiency, the hash table implements ordered
// linear probing --- consecutive hashes in the table are always stored in
// sorted order --- in a circular buffer.  Typically, the ordering optimization
// means any hash-table probe for a target T can stop when it encounters an
// element in the table whose hash is greater than T.
//
// However, the combination of ordering and a circular buffer leads to several
// tricky cases when probing for an element or its insertion point.  These cases
// depend on whether the probe wraps around the end of the buffer and whether
// the run --- the ordered sequence of hashes in the table --- wraps around the
// end of the buffer.
//
// Example case 1: no wrapping (common case)
//     Index:  ... | 3 | 4 | 5 | 6 | ...
//     Hashes: ... | 3 | 3 | 3 | 5 | ...
//     Target: 4
//   The probe starts at index 4 and scans increasing indices, stopping when it
//   sees hash = 5 at index 6.
//
// Example case 2: probe and run both wrap
//     Index:  | 0 | 1 | 2 | ... | 6 | 7 |
//     Hashes: | 6 | 7 | 0 | ... | 6 | 6 |
//     Target: 7
//   The run of 6's wraps around, as does the probe for 7.
//
// Example case 3: probe does not wrap, run does wrap
//     Index:  | 0 | 1 | 2 | ... | 6 | 7 |
//     Hashes: | 6 | 7 | 0 | ... | 6 | 6 |
//     Target: 0
//   The run of 6's and 7's wrap around.  The probe for 0 starts in the middle
//   of this wrapped run and must continue past it, even though the hashes in
//   the run are larger than the target.
//
// Example case 4: probe wraps, run does not wrap
//     Index:  | 0 | 1 | 2 | ... | 6 | 7 |
//     Hashes: | 6 | 0 | 1 | ... | 6 | 6 |
//     Target: 7
//   After the wrapped run of 6's is a run starting at 0, which does not wrap.
//   The probe for 7 wraps around before encountering the 0.  The probe should
//   stop at that point, even though 0 is smaller than 7.
//
// We characterize these four cases based on the following:
//
// - T: The target hash value being probed for.
// - i: The current index in the table being examined in the probe.
// - H[i]: The hash value of the key at index i, assuming that table entry is
//   occupied.
//
// We can identify cases where the probe or the run wraps around the end of the
// circular buffer by comparing i to T (for the probe) and i to H[i] (for the
// run).  A probe starts at i == T and proceeds to scan increasing values of i
// (mod table size).  Therefore, we typically expect i >= T and i >= H[i].  But
// when wrapping occurs, i will be smaller than the hash, that is, i < T when
// the probe wraps and i < H[i] when the run wraps.
//
// We can describe these four cases in terms of these variables as follows:
//   Normal Probe, Normal Run (NP+NR):   T <= i and H[i] <= i
//     The probe _terminates_ at i where T < H[i].
//   Wrapped Probe, Wrapped Run (WP+WR): T > i and H[i] > i
//     The probe _terminates_ at i where T < H[i].
//   Normal Probe, Wrapped Run (NP+WR):  T <= i and H[i] > i
//     The probe _must continue_ even though T < H[i].
//   Wrapped Probe, Normal Run (WP+NR):  T > i and H[i] <= i
//     The probe _must termiante_ even though T > H[i].
//
// The table uses the following bit trick to handle all of these cases simply:
//
//   Continue the probe if and only if i-T <= i-H[i], using an _unsigned
//   integer_ comparison.
//
// Intuitively, this trick makes the case of wrapping in the probe or run
// coincide with unsigned integer overflow, allowing the same comparison to be
// used for all cases.
//
// We can justify this bit trick in all caes:
//
//   NP+NR and WP+WR: The original termination condition, T < H[i], implies that
//   -T > -H[i].  Adding i to both sides does not affect the comparison.
//
//   NP+WR: The wrapped run, H[i] > i, implies that i-H[i] is negative, which
//   becomes are large positive unsigned integer.  Meanwhile, i-T is a small
//   positive unsigned integer, because i > T.  Hence, i-T < i-H[i], which
//   correctly implies that the probe must continue.
//
//   WP+NR: The wrapped probe, T > i, implies that i-T is negative, which
//   becomes a large positive unsigned integer.  Meanwhile, i >= H[i], implying
//   that i-H[i] is a small positive unsigned integer.  Hence, i-T > i-H[i],
//   which correctly implies that the probe should stop.
//
// Note: One can formulate this bit trick as T-i >= H[i]-i instead, preserving
// the direction of the inequality.  I formulate the trick this way simply
// because I prefer that the common case involve comparisons of small positive
// integers.

static inline bool continue_probe(index_t tgt, index_t hash, index_t idx) {
    // NOTE: index_t must be unsigned for this check to work.
    return (idx - tgt) <= (idx - hash);
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
