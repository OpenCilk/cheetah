#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cilk-internal.h"
#include "debug.h"
#include "internal-malloc.h" /* only needed for new view allocation */
#include "local-hypertable.h"

static void reducer_base_init(reducer_base *rb) {
    rb->view = NULL;
    rb->reduce_fn = NULL;
}

static void make_tombstone(uintptr_t *key) { *key = KEY_DELETED; }

static void bucket_init(struct bucket *b) {
    b->key = KEY_EMPTY;
    reducer_base_init(&b->value);
}

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

static struct bucket *bucket_array_create(int32_t array_size) {
    struct bucket *buckets =
        (struct bucket *)calloc(array_size, sizeof(struct bucket));
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

hyper_table *__cilkrts_local_hyper_table_alloc(void) {
    hyper_table *table = malloc(sizeof(hyper_table));
    int32_t capacity = MIN_CAPACITY;
    table->capacity = capacity;
    table->occupancy = 0;
    table->ins_rm_count = 0;
    table->buckets = bucket_array_create(capacity);
    return table;
}

void local_hyper_table_free(hyper_table *table) {
    free(table->buckets);
    free(table);
}

static struct bucket *rebuild_table(hyper_table *table, int32_t new_capacity) {
    struct bucket *old_buckets = table->buckets;
    int32_t old_capacity = table->capacity;
    int32_t old_occupancy = table->occupancy;

    assert(new_capacity <= MAX_CAPACITY);

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
            (void)success;
        }
    }

    assert(table->occupancy == old_occupancy &&
           "Mismatched occupancy after resizing table.");

    free(old_buckets);
    return table->buckets;
}

///////////////////////////////////////////////////////////////////////////
// Query, insert, and delete methods for the hash table.

struct bucket *__cilkrts_find_hyperobject_hash(hyper_table *table,
                                               uintptr_t key) {
    int32_t capacity = table->capacity;

    // Target hash
    const index_t tgt = get_table_entry(capacity, key);
    struct bucket *buckets = table->buckets;
    // Start the search at the target hash
    index_t i = tgt;
    do {
        uintptr_t curr_key = buckets[i].key;
        // Found the key?  Return that bucket.
        // TODO: Consider moving this bucket to the front of the run.
        if (key == curr_key)
            return &buckets[i];

        // Found an empty entry?  The search failed.
        if (is_empty(curr_key))
            return NULL;

        // Found a tombstone?  Continue the search.
        if (is_tombstone(curr_key)) {
            i = inc_index(i, capacity);
            continue;
        }

        // Otherwise, buckets[i] is another valid key that does not match.
        index_t curr_hash = buckets[i].hash;

        if (continue_search(tgt, curr_hash, i)) {
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
    assert(b.key != KEY_EMPTY && b.key != KEY_DELETED);
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

    // If the occupancy is already too high, rebuild the table.
    if (is_overloaded(table->occupancy, capacity)) {
        capacity *= 2;
        buckets = rebuild_table(table, capacity);
    } else if (time_to_rebuild(table->ins_rm_count, capacity)) {
        buckets = rebuild_table(table, capacity);
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

    // Search for the place to insert b.
    index_t i = tgt;

    const index_t search_end = tgt;
    do {
        uintptr_t curr_key = buckets[i].key;
        // Found the key?  Overwrite that bucket.
        // TODO: Reconsider what to do in this case.
        if (b.key == curr_key) {
            buckets[i].value = b.value;
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
            while (is_tombstone(tomb_end)) {
                next_i = inc_index(next_i, capacity);
                tomb_end = buckets[next_i].key;
            }
            // If the next entry is empty, then the search would stop.  It's
            // safe to insert the bucket at the tombstone.
            if (is_empty(tomb_end)) {
                buckets[current_tomb] = b;
                ++table->occupancy;
                ++table->ins_rm_count;
                return true;
            }
            // Check if the hash at the end of this run of tombstones would
            // terminate the search.
            index_t tomb_end_hash = buckets[next_i].hash;
            if (stop_insert_scan(tgt, tomb_end_hash, next_i)) {
                // It's safe to insert b at the current tombstone.
                buckets[current_tomb] = b;
                ++table->occupancy;
                ++table->ins_rm_count;
                return true;
            }
            // None of the locations among these consecutive tombstones are
            // appropriate for this bucket.  Continue the search.
            i = inc_index(next_i, capacity);
            continue;
        }

        // Otherwise we have another valid key that does not match.
        // Compare the hashes to decide whether or not to continue the
        // search.
        index_t curr_hash = buckets[i].hash;
        if (continue_search(tgt, curr_hash, i)) {
            i = inc_index(i, capacity);
            continue;
        }

        // This is an appropriate location to insert the bucket.  Stop
        // the search.
        break;
    } while (i != search_end);

    index_t insert_tgt = i;
    // The search found a place to insert the bucket, but it's occupied.  Insert
    // the bucket here and shift the subsequent entries.
    do {
        // If this entry is empty, insert the current bucket at this location
        // and terminate.
        if (is_empty(buckets[i].key)) {
            buckets[i] = b;
            ++table->occupancy;
            ++table->ins_rm_count;
            return true;
        } else if (is_tombstone(buckets[i].key)) {
            // Check whether its safe to insert the bucket at this tombstone.
            index_t next_i = inc_index(i, capacity);
            uintptr_t next_key = buckets[next_i].key;
            if (is_valid(next_key)) {
                // Inserting at this tombstone could disrupt the search for
                // next_key.  If this is the case, swap this tombstone with
                // next_key.
                index_t next_hash = get_table_entry(capacity, next_key);
                // Check whether next_key is already displaced from its intended
                // location in the table.  If so, then it's safe to move it to
                // the previous table entry.
                if (next_hash != next_i &&
                    continue_search(next_hash, next_i, i)) {
                    struct bucket tmp = buckets[i];
                    buckets[i] = buckets[next_i];
                    buckets[next_i] = tmp;
                    i = next_i;
                    continue;
                }
            }
            // Either next_key is not valid or it should not be moved.  In
            // either case, insert the bucket at this tombstone.
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

void *__cilkrts_insert_new_view(hyper_table *table, uintptr_t key, size_t size,
                                __cilk_identity_fn identity,
                                __cilk_reduce_fn reduce) {
    // Create a new view and initialize it with the identity function.
    void *new_view = cilk_aligned_alloc(64, round_size_to_alignment(64, size));
    identity(new_view);
    // Insert the new view into the local hypertable.
    struct bucket new_bucket = {
        .key = (uintptr_t)key,
        .value = {.view = new_view, .reduce_fn = reduce}};
    bool success = insert_hyperobject(table, new_bucket);
    assert(success);
    (void)success;
    // Return the new view.
    return new_view;
}

// Merge two hypertables, left and right.  Returns the merged hypertable and
// deletes the other.
hyper_table *merge_two_hts(__cilkrts_worker *restrict w,
                           hyper_table *restrict left,
                           hyper_table *restrict right) {
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

        if (NULL == dst_bucket) {
            // The destination table does not contain this key.  Insert the
            // key-value pair from the source table into the destination.
            insert_hyperobject(dst, b);
        } else {
            // Merge the two views in the source and destination buckets, being
            // sure to preserve left-to-right ordering.  Free the right view
            // when done.
            reducer_base dst_rb = dst_bucket->value;
            if (left_dst) {
                dst_rb.reduce_fn(dst_rb.view, b.value.view);
                free(b.value.view);
            } else {
                dst_rb.reduce_fn(b.value.view, dst_rb.view);
                free(dst_rb.view);
                dst_bucket->value.view = b.value.view;
            }
        }
    }

    // Destroy the source hyper_table, and return the destination.
    local_hyper_table_free(src);

    return dst;
}
