#include <assert.h>
#include <stdint.h>
#include <stdio.h>

// Dummy implementation of __cilkrts_get_worker_number.
unsigned __cilkrts_get_worker_number(void) { return 0; }

#define CHEETAH_INTERNAL
#include "../runtime/local-hypertable.h"

#define TRACE 0

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

// Check the entries of a hyper_table to verify that key appears exactly
// expected_count times.  Optionally print the entries of hyper_table.
void check_hypertable(hyper_table *table, uintptr_t key, int32_t expected_count) {
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
        PRINT_TRACE("table(%p)[%d] = { 0x%lx, %p }\n", buckets, i,
                    buckets[i].key,
                    is_valid(buckets[i].key) ? buckets[i].value.view : NULL);
        if (is_valid(key) && buckets[i].key == key)
            key_count++;
    }
    if (key_count != expected_count)
        PRINT_TRACE("ERROR: Unexpected count (%d != %d) for key 0x%lx!\n",
                    key_count, expected_count, key);
    assert(key_count == expected_count);
}

// Parse and execute a table_command on a hyper_table.
void do_table_command(hyper_table *table, table_command cmd) {
    switch (cmd.type) {
    case TABLE_INSERT: {
        PRINT_TRACE("INSERT 0x%lx\n", cmd.key);
        bool success = insert_hyperobject(
            table, (struct bucket){
                       .key = cmd.key,
                       .value = {.view = (void *)cmd.key, .reduce_fn = NULL}});
        assert(success && "insert_hyperobject failed");
        check_hypertable(table, cmd.key, 1);
        break;
    }
    case TABLE_LOOKUP: {
        // TODO: Implement this.
        break;
    }
    case TABLE_DELETE: {
        PRINT_TRACE("DELETE 0x%lx\n", cmd.key);
        bool success = remove_hyperobject(table, cmd.key);
        assert(success && "remove_hyperobject failed");
        check_hypertable(table, cmd.key, 0);
        break;
    }
    }
}

// Simple test routine to insert and remove elements from a hyper_table,
// according to the given list of table_commands.
void test_insert_remove(const table_command *commands, int num_commands) {
    hyper_table *table = (hyper_table *)malloc(sizeof(hyper_table));
    local_hyper_table_init(table);
    for (int i = 0; i < num_commands; ++i) {
        do_table_command(table, commands[i]);
    }
    local_hyper_table_destroy(table);
    free(table);
}

int main(int argc, char *argv[]) {
    // Simple test case
    table_command test1[] = {
        {TABLE_INSERT, 0x1},
        {TABLE_INSERT, 0x2},
        {TABLE_INSERT, 0x3},
        {TABLE_INSERT, 0x4},
        {TABLE_INSERT, 0x5},
        {TABLE_INSERT, 0x6},
        {TABLE_INSERT, 0x7},
        {TABLE_INSERT, 0x8},
        {TABLE_INSERT, 0x9},
        {TABLE_INSERT, 0xa},
        {TABLE_INSERT, 0xb},
        {TABLE_INSERT, 0xc},
        {TABLE_INSERT, 0xd},
        {TABLE_INSERT, 0xe},
        {TABLE_INSERT, 0xf},

        {TABLE_DELETE, 0x1},
        {TABLE_INSERT, 0x1},

        {TABLE_DELETE, 0x1},
        {TABLE_DELETE, 0x2},
        {TABLE_DELETE, 0x3},
        {TABLE_DELETE, 0x4},
        {TABLE_DELETE, 0x5},
        {TABLE_DELETE, 0x6},
        {TABLE_DELETE, 0x7},
        {TABLE_DELETE, 0x8},
        {TABLE_DELETE, 0x9},
        {TABLE_DELETE, 0xa},
        {TABLE_DELETE, 0xb},
        {TABLE_DELETE, 0xc},
        {TABLE_DELETE, 0xd},
        {TABLE_DELETE, 0xe},
        {TABLE_DELETE, 0xf},
    };
    test_insert_remove(test1, sizeof(test1)/sizeof(table_command));

    // Test case derived from trace that led to errors.
    table_command test2[] = {
        {TABLE_INSERT, 0x7f2a10bfe050},
        {TABLE_INSERT, 0x7f2a10bff968},
        {TABLE_INSERT, 0x7f2a10bfe8a8},
        {TABLE_INSERT, 0x7f2a10bfece0},
        {TABLE_INSERT, 0x7f2a10bff538},
        {TABLE_INSERT, 0x7f2a10bff108},
        {TABLE_INSERT, 0x7f2a10bff540},
        {TABLE_INSERT, 0x7f2a10bff970},
        {TABLE_INSERT, 0x7f2a10bfe8b0},
        {TABLE_INSERT, 0x7f2a10bfe478},
        {TABLE_INSERT, 0x7f2a10bfe480},
        {TABLE_INSERT, 0x7f2a10bff110},
        {TABLE_INSERT, 0x7f2a10bffda0},
        {TABLE_INSERT, 0x562edc97d0c0},
        {TABLE_INSERT, 0x7f2a10bfe048},

        {TABLE_INSERT, 0x7f2a10bfe478},
        {TABLE_INSERT, 0x7f2a10bff110},

        {TABLE_DELETE, 0x7f2a10bfe048},
        {TABLE_DELETE, 0x7f2a10bfe050},
        {TABLE_DELETE, 0x7f2a10bfe478},
        {TABLE_DELETE, 0x7f2a10bfe480},
        {TABLE_DELETE, 0x7f2a10bfe8a8},
        {TABLE_DELETE, 0x7f2a10bfe8b0},

        {TABLE_INSERT, 0x7f2a10bfe8b0},
        {TABLE_INSERT, 0x7f2a10bfe8a8},
        {TABLE_INSERT, 0x7f2a10bfe480},
        {TABLE_INSERT, 0x7f2a10bfe478},
        {TABLE_INSERT, 0x7f2a10bfe050},
        {TABLE_INSERT, 0x7f2a10bfe048},

        {TABLE_DELETE, 0x7f2a10bfe048},
        {TABLE_DELETE, 0x7f2a10bfe050},
        {TABLE_DELETE, 0x7f2a10bfe478},
        {TABLE_DELETE, 0x7f2a10bfe480},

        {TABLE_INSERT, 0x7f2a10bfe480},
        {TABLE_INSERT, 0x7f2a10bfe478},

        {TABLE_INSERT, 0x7f2a10bfe480},
    };
    test_insert_remove(test2, sizeof(test2)/sizeof(table_command));
    return 0;
}
