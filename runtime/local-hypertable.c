#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "debug.h"
#include "local-hypertable.h"

static void reducer_base_init(reducer_base *rb) {
    rb->view = NULL;
    // rb->view_size = 0;
    rb->reduce_fn = NULL;
    // rb->identity_fn = NULL;
}

static void make_tombstone(uintptr_t *key) { *key = KEY_DELETED; }

static void bucket_init(struct bucket *b) {
    b->key = KEY_EMPTY;
    reducer_base_init(&b->value);
}

// Data type for indexing the hash table.  This type is used for
// hashes as well as the table's capacity.
const int32_t MIN_CAPACITY = 4;
const int32_t MIN_HT_CAPACITY = 8;

// Constant used to determine the target maximum load factor.  The
// table will aim for a maximum load factor of
// 1 - (1 / LOAD_FACTOR_CONSTANT).
const int32_t LOAD_FACTOR_CONSTANT = 16;

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
const int32_t MIN_REBUILD_OP_COUNT = 8;
static bool time_to_rebuild(int32_t ins_rm_count, int32_t capacity) {
    return (ins_rm_count > MIN_REBUILD_OP_COUNT) &&
           (ins_rm_count > capacity / (4 * LOAD_FACTOR_CONSTANT));
}

static struct bucket *bucket_array_create(int32_t array_size) {
    struct bucket *buckets =
        (struct bucket *)malloc(sizeof(struct bucket) * array_size);
    if (array_size < MIN_HT_CAPACITY) {
        for (int32_t i = 0; i < array_size; ++i) {
            bucket_init(&buckets[i]);
        }
        return buckets;
    }
    int32_t tombstone_idx = 0;
    for (int32_t i = 0; i < array_size; ++i) {
        bucket_init(&buckets[i]);
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

// Some random numbers for the hash.
uint64_t seed = 0xe803e76341ed6d51UL;
const uint64_t salt = 0x96b9af4f6a40de92UL;

static index_t hash(uintptr_t key_in) {
    /* uint64_t x = (uint32_t)(key_in ^ salt) | (((key_in ^ seed) >> 32) << 32);
     */
    uint64_t x = key_in ^ salt;
    // mix64 from SplitMix.
    x = (x ^ (x >> 33)) * 0xff51afd7ed558ccdUL;
    x = (x ^ (x >> 33)) * 0xc4ceb9fe1a85ec53UL;
    return x;
}

static index_t get_table_entry(hyper_table *table, uintptr_t key) {
    // TODO: Replace capacity with lg_capacity and replace this
    // operation with a shift instead of a divide.
    return hash(key) % table->capacity;
}

void local_hyper_table_init(hyper_table *table) {
    int32_t capacity = MIN_CAPACITY;
    table->capacity = capacity;
    table->occupancy = 0;
    table->ins_rm_count = 0;
    table->buckets = bucket_array_create(capacity);
}

void local_hyper_table_destroy(hyper_table *table) { free(table->buckets); }

static struct bucket *rebuild_table(hyper_table *table, int32_t new_capacity) {
    struct bucket *old_buckets = table->buckets;
    int32_t old_capacity = table->capacity;
    int32_t old_occupancy = table->occupancy;

    table->buckets = bucket_array_create(new_capacity);
    table->capacity = new_capacity;
    table->occupancy = 0;
    // Set count of insertions and removals to prevent insertions into
    // new table from triggering another rebuild.
    table->ins_rm_count = -old_occupancy;

    // Iterate through old table and insert each element into the new
    // table.
    for (int32_t i = 0; i < old_capacity; ++i) {
        if (is_valid(old_buckets[i].key)) {
            bool success = insert_hyperobject(table, old_buckets[i]);
            assert(success && "Failed to insert when resizing table.");
        }
    }

    assert(table->occupancy == old_occupancy &&
           "Mismatched occupancy after resizing table.");

    free(old_buckets);
    return table->buckets;
}

///////////////////////////////////////////////////////////////////////////
// Query, insert, and delete methods for the hash table.

static index_t inc_index(index_t i, index_t capacity) {
    ++i;
    if (i == capacity)
        i = 0;
    return i;
}

static bool continue_search(index_t tgt, index_t hash, index_t i,
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

struct bucket *find_hyperobject(hyper_table *table, uintptr_t key) {
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
    index_t tgt = get_table_entry(table, key);
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
        index_t curr_hash = get_table_entry(table, curr_key);
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

bool remove_hyperobject(hyper_table *table, uintptr_t key) {
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

    // If entry is NULL, the search did not find the key.
    if (NULL == entry)
        return false;

    // The search found the key and returned a pointer to the entry.
    // Replace the entry with a tombstone and decrement the occupancy.
    make_tombstone(&entry->key);
    --table->occupancy;
    ++table->ins_rm_count;

    int32_t capacity = table->capacity;
    if (is_underloaded(table->occupancy, capacity))
        rebuild_table(table, capacity / 2);
    else if (time_to_rebuild(table->ins_rm_count, capacity))
        rebuild_table(table, capacity);

    return true;
}

bool insert_hyperobject(hyper_table *table, struct bucket b) {
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
        buckets = rebuild_table(table, capacity);
    }

    // Target hash
    index_t tgt = get_table_entry(table, b.key);

    // If the occupancy is already too high, rebuild the table.
    if (is_overloaded(table->occupancy, capacity)) {
        capacity *= 2;
        buckets = rebuild_table(table, capacity);
    } else if (time_to_rebuild(table->ins_rm_count, capacity)) {
        buckets = rebuild_table(table, capacity);
    }

    // If we find an empty entry, insert the bucket there.
    if (is_empty(buckets[tgt].key)) {
        buckets[tgt] = b;
        ++table->occupancy;
        ++table->ins_rm_count;
        return true;
    }

    // Search for the place to insert b.
    index_t i = tgt;
    do {
        uintptr_t curr_key = buckets[i].key;
        // If we find the key, overwrite that bucket.
        // TODO: Reconsider what we do in this case.
        if (b.key == curr_key) {
            buckets[i].value = b.value;
            return true;
        }

        // If we find an empty entry, insert b there.
        if (is_empty(curr_key)) {
            buckets[i] = b;
            ++table->occupancy;
            ++table->ins_rm_count;
            return true;
        }

        // If we find a tombstone, check whether to insert b here, and
        // finish the insert if so.
        if (is_tombstone(curr_key)) {
            // Check that the search would not continue through the next
            // index.
            index_t next_i = inc_index(i, capacity);
            index_t next_key = buckets[next_i].key;
            if (is_empty(next_key)) {
                // If the next entry is empty, then the search would
                // stop.  Go ahead and insert the bucket.
                buckets[i] = b;
                ++table->occupancy;
                ++table->ins_rm_count;
                return true;
            } else if (is_tombstone(next_key)) {
                // If the next entry is a tombstone, then the search
                // would continue.
                i = next_i;
                continue;
            }
            index_t next_hash = get_table_entry(table, next_key);
            if (!continue_search(tgt, next_hash, next_i, b.key, next_key)) {
                // This location is appropriate for inserting the bucket.
                buckets[i] = b;
                ++table->occupancy;
                ++table->ins_rm_count;
                return true;
            }
            // This location is not appropriate for this bucket.
            // Continue the search.
            i = next_i;
            continue;
        }

        // Otherwise we have another valid key that does not match.
        // Compare the hashes to decide whether or not to continue the
        // search.
        index_t curr_hash = get_table_entry(table, curr_key);
        if (continue_search(tgt, curr_hash, i, b.key, curr_key)) {
            i = inc_index(i, capacity);
            continue;
        }

        // This is an appropriate location to insert the bucket.  Stop
        // the search.
        break;
    } while (i != tgt);

    // The search found a place to insert the bucket, but it's
    // occupied.  Insert the bucket here and shift the subsequent
    // entries.
    do {
        // If this entry is empty or a tombstone, insert the current
        // bucket at this location and terminate.
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
    } while (i != tgt);

    assert(i != tgt && "Insertion failed.");
    return false;
}

hyper_table *merge_two_hts(__cilkrts_worker *restrict w,
                           hyper_table *restrict left,
                           hyper_table *restrict right) {
    if (!left)
        return right;
    if (!right)
        return left;

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

    int32_t src_capacity = src->capacity;
    struct bucket *src_buckets = src->buckets;
    for (int32_t i = 0; i < src_capacity; ++i) {
        struct bucket b = src_buckets[i];
        if (!is_valid(b.key))
            continue;

        struct bucket *dst_bucket = find_hyperobject(dst, b.key);

        if (NULL == dst_bucket) {
            insert_hyperobject(dst, b);
        } else {
            reducer_base dst_rb = dst_bucket->value;
            if (left_dst) {
                dst_rb.reduce_fn(dst_rb.view, b.value.view);
                free(b.value.view);
            } else {
                dst_rb.reduce_fn(b.value.view, dst_rb.view);
                free(dst_rb.view);
                dst_rb.view = b.value.view;
            }
        }
    }

    local_hyper_table_destroy(src);
    free(src);

    return dst;
}