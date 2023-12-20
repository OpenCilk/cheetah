#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TRACE 0

// Dummy implementation of __cilkrts_get_worker_number.
unsigned __cilkrts_get_worker_number(void) { return 0; }

#define CHEETAH_INTERNAL
#include "../runtime/local-hypertable.h"

// Print alert message
void ALERT(const char *fmt, ...) {
    va_list l;
    va_start(l, fmt);
    vfprintf(stderr, fmt, l);
    va_end(l);
}

// Print additional trace information if TRACE == 1.
void PRINT_TRACE(const char *fmt, ...) {
#if TRACE
    va_list l;
    va_start(l, fmt);
    vfprintf(stderr, fmt, l);
    va_end(l);
#endif
}

// Structures for specifying simple table commands for tests.
enum table_command_type {
    TABLE_INSERT,
    TABLE_LOOKUP,
    TABLE_DELETE
};
typedef struct table_command {
    enum table_command_type type;
    uintptr_t key;
} table_command;

// Verify the entries of a hyper_table to verify that key appears exactly
// expected_count times.  Optionally print the entries of hyper_table.
void verify_hypertable(hyper_table *table, uintptr_t key, int32_t expected_count) {
    int32_t key_count = 0;
    int32_t capacity = table->capacity;
    struct bucket *buckets = table->buckets;
    PRINT_TRACE("table(%p): cap %d, occ %d, ins_rm %d\n", buckets, capacity,
                table->occupancy, table->ins_rm_count);
    if (capacity < MIN_HT_CAPACITY) {
        int32_t occupancy = table->occupancy;
        for (int32_t i = 0; i < occupancy; ++i) {
            PRINT_TRACE("table(%p)[%d] = { 0x%lx, %p }\n", buckets, i,
                        buckets[i].key, buckets[i].value.view);
            if (is_valid(key) && buckets[i].key == key)
                key_count++;
        }
        if (key_count != expected_count)
            PRINT_TRACE("ERROR: Unexpected count (%d != %d) for key 0x%lx!\n",
                        key_count, expected_count, key);
        assert(key_count == expected_count);
        return;
    }

    for (int32_t i = 0; i < capacity; ++i) {
        PRINT_TRACE("table(%p)[%d] = { 0x%lx, %d, %p }\n", buckets, i,
                    buckets[i].key, buckets[i].hash,
                    is_valid(buckets[i].key) ? buckets[i].value.view : NULL);
        if (is_valid(key) && buckets[i].key == key)
            key_count++;
    }
    if (key_count != expected_count)
        PRINT_TRACE("ERROR: Unexpected count (%d != %d) for key 0x%lx!\n",
                    key_count, expected_count, key);
    assert(key_count == expected_count);
}

// A weaker form of verify_hypertable, returns whether the specified key shows
// up the expected number of times.
bool check_hypertable(hyper_table *table, uintptr_t key,
                      int32_t expected_count) {
    int32_t key_count = 0;
    int32_t capacity = table->capacity;
    struct bucket *buckets = table->buckets;
    if (capacity < MIN_HT_CAPACITY) {
        int32_t occupancy = table->occupancy;
        for (int32_t i = 0; i < occupancy; ++i) {
            if (is_valid(key) && buckets[i].key == key)
                key_count++;
        }
        return key_count == expected_count;
    }

    for (int32_t i = 0; i < capacity; ++i) {
        if (is_valid(key) && buckets[i].key == key)
            key_count++;
    }
    return key_count == expected_count;
}

// Parse and execute a table_command on a hyper_table.
void do_table_command(hyper_table *table, table_command cmd) {
    switch (cmd.type) {
    case TABLE_INSERT: {
        PRINT_TRACE("INSERT 0x%lx\n", cmd.key);
        /* if (!check_hypertable(table, cmd.key, 0)) { */
        /*     ALERT("INSERT Unsupported! 0x%lx already in table\n", cmd.key); */
        /*     break; */
        /* } */
        bool success = insert_hyperobject(
            table, (struct bucket){
                       .key = cmd.key,
                       .value = {.view = (void *)cmd.key, .reduce_fn = NULL}});
        assert(success && "insert_hyperobject failed");
        verify_hypertable(table, cmd.key, 1);
        break;
    }
    case TABLE_LOOKUP: {
        PRINT_TRACE("LOOKUP 0x%lx\n", cmd.key);
        struct bucket *b = find_hyperobject(table, cmd.key);
        verify_hypertable(table, cmd.key, NULL != b);
        break;
    }
    case TABLE_DELETE: {
        PRINT_TRACE("DELETE 0x%lx\n", cmd.key);
        /* if (!check_hypertable(table, cmd.key, 1)) { */
        /*     ALERT("DELETE Unsupported! 0x%lx not in table\n", cmd.key); */
        /*     break; */
        /* } */
        bool success = remove_hyperobject(table, cmd.key);
        assert(success && "remove_hyperobject failed");
        verify_hypertable(table, cmd.key, 0);
        break;
    }
    }
}

// Simple test routine to insert and remove elements from a hyper_table,
// according to the given list of table_commands.
void test_insert_remove(const table_command *commands, int num_commands) {
    hyper_table *table = __cilkrts_local_hyper_table_alloc();
    for (int i = 0; i < num_commands; ++i) {
        do_table_command(table, commands[i]);
    }
    local_hyper_table_free(table);
}

void test_set_insert_remove(const uintptr_t *keys, int num_keys,
                            const table_command *commands, int num_commands) {
    assert((num_keys & -num_keys) == num_keys &&
           "Must use a power-of-2 number of keys.");
    hyper_table *table = __cilkrts_local_hyper_table_alloc();

    // Add enough temporary keys to make the table the correct size.
    for (int i = 0; i < num_keys / 2 + 1; ++i) {
        table_command tmpInsert = {TABLE_INSERT, i + 1};
        do_table_command(table, tmpInsert);
    }

    // Set the keys to their intended values.
    int num_valid = 0;
    int num_tomb = 0;
    for (int i = 0; i < num_keys; ++i) {
        num_valid += is_valid(keys[i]);
        num_tomb += is_tombstone(keys[i]);
        table->buckets[i].key = keys[i];
        table->buckets[i].hash = hash(keys[i]) % num_keys;
    }
    table->occupancy = num_valid;
    table->ins_rm_count = num_tomb;
    verify_hypertable(table, 1, 0);

    // Run test.
    for (int i = 0; i < num_commands; ++i) {
        do_table_command(table, commands[i]);
    }
    local_hyper_table_free(table);
}
