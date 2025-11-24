#include "internal-malloc.h" /* only needed for new view allocation */
#include "local-hypertable.h"

#include "cilk/reducer"

#include "debug.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>

static void make_tombstone(uintptr_t *key) { *key = KEY_DELETED; }

// Constant used to determine the target maximum load factor.  The
// table will aim for a maximum load factor of
// 1 - (1 / LOAD_FACTOR_CONSTANT).
static const int32_t LOAD_FACTOR_CONSTANT = 16;
// Prevent integer overflow computing load factor
__attribute__((unused))
static const int32_t MAX_CAPACITY = 0x7fffffff / (LOAD_FACTOR_CONSTANT - 1);

static bool is_overloaded(int32_t occupancy, int32_t capacity) {
    // Set the upper load threshold to be 15/16ths of the capacity.
    return occupancy >
           (LOAD_FACTOR_CONSTANT - 1) * capacity / LOAD_FACTOR_CONSTANT;
}

static bool is_underloaded(int32_t occupancy, int32_t capacity) {
    // Set the lower load threshold to be 7/16ths of the capacity.
    return (capacity > MIN_CAPACITY) &&
           (occupancy <=
            ((LOAD_FACTOR_CONSTANT / 2) - 1) * capacity / LOAD_FACTOR_CONSTANT);
}

// After enough insertions and deletions have occurred, rebuild the
// table to fix up tombstones.
static const int32_t MIN_REBUILD_OP_COUNT = 8;
static bool time_to_rebuild(int32_t ins_rm_count, int32_t capacity) {
    return (ins_rm_count > MIN_REBUILD_OP_COUNT) &&
           (ins_rm_count > capacity / (4 * LOAD_FACTOR_CONSTANT));
}

struct bucket *hyper_table::bucket_array_create(int32_t array_size) {
    struct bucket *buckets = new bucket[array_size];
    if (array_size < MIN_HT_CAPACITY) {
        return buckets;
    }
    int32_t tombstone_idx = 0;
    for (int32_t i = 0; i < array_size; ++i) {
        // Graveyard hashing: Insert tombstones at regular intervals.
        // TODO: Check if it's bad for the insertions to rebuild a
        // table to use these tombstones.
        if (tombstone_idx == 2 * LOAD_FACTOR_CONSTANT) {
            make_tombstone(&buckets[i].key);
            tombstone_idx -= 2 * LOAD_FACTOR_CONSTANT;
        } else
            ++tombstone_idx;
    }
    return buckets;
}

hyper_table *__cilkrts_local_hyper_table_alloc(void) {
    return new hyper_table(MIN_CAPACITY);
}

void local_hyper_table_free(hyper_table *table) {
    delete table;
}

void hyper_table::rebuild(int32_t new_capacity) {
    struct bucket *old_buckets = buckets;
    int32_t old_capacity = capacity;
    int32_t old_occupancy = occupancy;

    assert(new_capacity <= MAX_CAPACITY);

    buckets = bucket_array_create(new_capacity);
    capacity = new_capacity;
    occupancy = 0;
    // Set count of insertions and removals to prevent insertions into
    // new table from triggering another rebuild.
    ins_rm_count = -old_occupancy;

    // Iterate through old table and insert each element into the new
    // table.
    for (int32_t i = 0; i < old_capacity; ++i) {
        if (is_valid(old_buckets[i].key)) {
            bool success = insert_hyperobject(this, old_buckets[i]);
            assert(success && "Failed to insert when resizing table.");
            (void)success;
        }
    }

    assert(occupancy == old_occupancy &&
           "Mismatched occupancy after resizing table.");

    free(old_buckets);
}

///////////////////////////////////////////////////////////////////////////
// Query, insert, and delete methods for the hash table.

struct bucket *__cilkrts_find_hyperobject_hash(hyper_table *table,
                                               uintptr_t key) {
    int32_t capacity = table->capacity;

    // Target hash
    const index_t tgt = get_table_entry(capacity, key);
    struct bucket *buckets = table->buckets;
    // Start the probe at the target hash
    index_t i = tgt;
    do {
        uintptr_t curr_key = buckets[i].key;
        // Found the key?  Return that bucket.
        // TODO: Consider moving this bucket to the front of the run.
        if (key == curr_key)
            return &buckets[i];

        // Found an empty entry?  The probe failed.
        if (is_empty(curr_key))
            return nullptr;

        // Found a tombstone?  Continue the probe.
        if (is_tombstone(curr_key)) {
            i = inc_index(i, capacity);
            continue;
        }

        // Otherwise, buckets[i] is another valid key that does not match.
        index_t curr_hash = buckets[i].hash;

        if (continue_probe(tgt, curr_hash, i)) {
            i = inc_index(i, capacity);
            continue;
        }

        // If none of the above cases match, then the probe failed to
        // find the key.
        return nullptr;
    } while (i != tgt);

    // The probe failed to find the key.
    return nullptr;
}

bool remove_hyperobject(hyper_table *table, uintptr_t key) noexcept {
    if (table->capacity < MIN_HT_CAPACITY) {
        // If the table is small enough, just scan the array.
        struct bucket *buckets = table->buckets;
        int32_t occupancy = table->occupancy;

        for (int32_t i = 0; i < occupancy; ++i) {
            if (buckets[i].key == key) {
                if (i == occupancy - 1)
                    // Set this entry's key to empty.  This code is here
                    // primarily to handle the case where occupancy == 1.
                    buckets[i].key = KEY_EMPTY;
                else
                    // Remove this entry by swapping it with the last entry.
                    buckets[i] = buckets[occupancy - 1];
                // Decrement the occupancy.
                --table->occupancy;
                return true;
            }
        }
        return false;
    }

    // Find the key in the table.
    struct bucket *entry = find_hyperobject(table, key);

    // If entry is NULL, the probe did not find the key.
    if (nullptr == entry)
        return false;

    // The probe found the key and returned a pointer to the entry.
    // Replace the entry with a tombstone and decrement the occupancy.
    make_tombstone(&entry->key);
    --table->occupancy;
    ++table->ins_rm_count;

    int32_t capacity = table->capacity;
    if (is_underloaded(table->occupancy, capacity))
        table->rebuild(capacity / 2);
    else if (time_to_rebuild(table->ins_rm_count, capacity))
        table->rebuild(capacity);

    return true;
}

bool insert_hyperobject(hyper_table *table, struct bucket b) noexcept {
    int32_t capacity = table->capacity;
    struct bucket *buckets = table->buckets;
    if (capacity < MIN_HT_CAPACITY) {
        // If the table is small enough, just scan the array.
        int32_t occupancy = table->occupancy;

        if (occupancy < capacity) {
            for (int32_t i = 0; i < occupancy; ++i) {
                if (buckets[i].key == b.key) {
                    // The key is already in the table.  Overwrite.
                    buckets[i] = b;
                    return true;
                }
            }

            // The key is not aleady in the table.  Append the bucket.
            buckets[occupancy] = b;
            ++table->occupancy;
            return true;
        }

        // The small table is full.  Increase its capacity, convert it
        // to a hash table, and fall through to insert the new bucket
        // into that hash table.
        capacity *= 2;
        table->rebuild(capacity);
        buckets = table->buckets;
    }

    // If the occupancy is already too high, rebuild the table.
    if (is_overloaded(table->occupancy, capacity)) {
        capacity *= 2;
        table->rebuild(capacity);
        buckets = table->buckets;
    } else if (time_to_rebuild(table->ins_rm_count, capacity)) {
        table->rebuild(capacity);
        buckets = table->buckets;
    }

    // Target hash
    const index_t tgt = get_table_entry(capacity, b.key);
    b.hash = tgt;

    // If we find an empty entry, insert the bucket there.
    if (is_empty(buckets[tgt].key)) {
        buckets[tgt] = b;
        ++table->occupancy;
        ++table->ins_rm_count;
        return true;
    }

    // Probe for the place to insert b.
    index_t i = tgt;

    const index_t probe_end = tgt;
    do {
        uintptr_t curr_key = buckets[i].key;
        // Found the key?  Overwrite that bucket.
        // TODO: Reconsider what to do in this case.
        if (b.key == curr_key) {
            buckets[i].data = b.data;
            return true;
        }

        // Found an empty entry?  Insert b there.
        if (is_empty(curr_key)) {
            buckets[i] = b;
            ++table->occupancy;
            ++table->ins_rm_count;
            return true;
        }

        // Found a tombstone?
        if (is_tombstone(curr_key)) {
            index_t current_tomb = i;
            // Scan consecutive tombstones from i.
            index_t next_i = inc_index(i, capacity);
            uintptr_t tomb_end = buckets[next_i].key;
            while (next_i != probe_end && is_tombstone(tomb_end)) {
                next_i = inc_index(next_i, capacity);
                tomb_end = buckets[next_i].key;
            }

            // If the next entry is empty, then the probe would stop.  It's
            // safe to insert the bucket at the tombstone at i.
            if (is_empty(tomb_end)) {
                buckets[current_tomb] = b;
                ++table->occupancy;
                ++table->ins_rm_count;
                return true;
            }

            // Check if the hash at the end of this run of tombstones would
            // terminate the probe or if the probe has traversed the whole
            // table.
            index_t tomb_end_hash = buckets[next_i].hash;
            if (next_i == probe_end ||
                !continue_probe(tgt, tomb_end_hash, next_i)) {
                // It's safe to insert b at the current tombstone.
                buckets[current_tomb] = b;
                ++table->occupancy;
                ++table->ins_rm_count;
                return true;
            }

            // None of the locations among these consecutive tombstones are
            // appropriate for this bucket.  Continue the probe.
            i = next_i;
            continue;
        }

        // Otherwise this entry contains another valid key that does
        // not match.  Compare the hashes to decide whether or not to
        // continue the probe.
        index_t curr_hash = buckets[i].hash;
        if (continue_probe(tgt, curr_hash, i)) {
            i = inc_index(i, capacity);
            continue;
        }

        // This is an appropriate location to insert the bucket.  Stop
        // the probe.
        break;
    } while (i != probe_end);

    index_t insert_tgt = i;
    // The probe found a place to insert the bucket, but it's occupied.  Insert
    // the bucket here and shift the subsequent entries.
    do {
        // If this entry is empty or a tombstone, insert the current bucket at
        // this location and terminate.
        if (!is_valid(buckets[i].key)) {
            buckets[i] = b;
            ++table->occupancy;
            ++table->ins_rm_count;
            return true;
        }

        // Swap b with the current bucket.
        struct bucket tmp = buckets[i];
        buckets[i] = b;
        b = tmp;

        // Continue onto the next index.
        i = inc_index(i, capacity);
    } while (i != insert_tgt);

    assert(i != insert_tgt && "Insertion failed.");
    return false;
}

__reducer_base *__cilkrts_insert_new_view_0(hyper_table *table,
                                            __reducer_base *key) {
    // Create a new view and initialize it with the identity function.
    size_t size = key->size();
    void *new_view = cilk_aligned_alloc(64, round_size_to_alignment(64, size));
    __reducer_base *base = key->identity(new_view);
    // Insert the new view into the local hypertable.
    struct bucket new_bucket = {
        .key = (uintptr_t)key,
        .data = { .view = new_view, .extra = base }
    };
    bool success = insert_hyperobject(table, new_bucket);
    assert(success);
    (void)success;
    // Return the base class subobject of the new view.
    return base;
}

void *__cilkrts_insert_new_view_1(hyper_table *table, uintptr_t key,
                                  const __reducer_callbacks &callbacks) {
    // Create a new view and initialize it with the identity function.
    void *new_view =
        cilk_aligned_alloc(64, round_size_to_alignment(64, callbacks.size));
    callbacks.identity(new_view);
    // Insert the new view into the local hypertable.
    struct bucket new_bucket = {
        .key = (uintptr_t)key,
        // XXX check lifetime
        .data = { .view = new_view, .extra = &callbacks.reduce }
    };
    bool success = insert_hyperobject(table, new_bucket);
    assert(success);
    (void)success;
    // Return the new view.
    return new_view;
}

void *__cilkrts_insert_new_view_2(hyper_table *table, uintptr_t key,
                                  size_t size,
                                  void (*identity)(void *),
                                  void (*reduce)(void *, void *)) {
    // Create a new view and initialize it with the identity function.
    void *new_view = cilk_aligned_alloc(64, round_size_to_alignment(64, size));
    identity(new_view);
    // Insert the new view into the local hypertable.
    struct bucket new_bucket = {
        .key = (uintptr_t)key,
        .data = { .view = new_view, .extra = reduce }
    };
    bool success = insert_hyperobject(table, new_bucket);
    assert(success);
    (void)success;
    // Return the new view.
    return new_view;
}

// Merge two hypertables, left and right.  Returns the merged hypertable and
// deletes the other.
hyper_table *merge_two_hts(hyper_table *__restrict left,
                           hyper_table *__restrict right) {
    // In the trivial case of an empty hyper_table, return the other
    // hyper_table.
    if (!left)
        return right;
    if (!right)
        return left;
    if (left->occupancy == 0) {
        local_hyper_table_free(left);
        return right;
    }
    if (right->occupancy == 0) {
        local_hyper_table_free(right);
        return left;
    }

    // Pick the smaller hyper_table to be the source, which we will iterate
    // over.
    bool left_dst;
    hyper_table *src, *dst;
    if (left->occupancy >= right->occupancy) {
        src = right;
        dst = left;
        left_dst = true;
    } else {
        src = left;
        dst = right;
        left_dst = false;
    }

    int32_t src_capacity =
        (src->capacity < MIN_HT_CAPACITY) ? src->occupancy : src->capacity;
    struct bucket *src_buckets = src->buckets;
    // Iterate over the contents of the source hyper_table.
    for (int32_t i = 0; i < src_capacity; ++i) {
        struct bucket b = src_buckets[i];
        if (!is_valid(b.key))
            continue;

        // For each valid key in the source table, lookup that key in the
        // destination table.
        struct bucket *dst_bucket = find_hyperobject(dst, b.key);

        if (nullptr == dst_bucket) {
            // The destination table does not contain this key.  Insert the
            // key-value pair from the source table into the destination.
            insert_hyperobject(dst, b);
        } else {
            // Merge the two views in the source and destination buckets, being
            // sure to preserve left-to-right ordering.  Free the right view
            // when done.
            if (left_dst) {
                bucket::reduce(dst_bucket, &b);
            } else {
                bucket::reduce(&b, dst_bucket);
                dst_bucket->data = b.data;
                b.data.extra = (__reducer_base *)nullptr;
                b.data.view = nullptr;
            }
        }
    }

    // Destroy the source hyper_table, and return the destination.
    local_hyper_table_free(src);

    return dst;
}

void bucket::reduce(bucket *left, bucket *right)
{
    assert(left->data.extra.index() == right->data.extra.index());
    void *left_view = left->data.view, *right_view = right->data.view;
    if (std::holds_alternative<__reducer_base *>(left->data.extra)) {
        __reducer_base *leftmost =
            static_cast<__reducer_base *>
            (reinterpret_cast<void *>(left->key));
        __reducer_base *left_r = std::get<__reducer_base *>(left->data.extra);
        __reducer_base *right_r = std::get<__reducer_base *>(right->data.extra);
        leftmost->reduce(left_r, right_r);
        right_r->~__reducer_base();
    } else if (std::holds_alternative<const __cilk_reduce_fn *>(left->data.extra)) {
        (*std::get<const __cilk_reduce_fn *>(left->data.extra))
            (left_view, right_view);
    } else {
        std::get<void (*)(void *, void *)>(left->data.extra)
            (left_view, right_view);
    }
    right->data.extra = (__reducer_base *)nullptr;
    right->data.view = nullptr;
    free(right_view);
}
