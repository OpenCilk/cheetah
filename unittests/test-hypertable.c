#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define TRACE 1

#if TRACE
#include <stdarg.h>
#endif

// Dummy implementation of __cilkrts_get_worker_number.
unsigned __cilkrts_get_worker_number(void) { return 0; }

#define CHEETAH_INTERNAL
#include "../runtime/local-hypertable.h"

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
        PRINT_TRACE("LOOKUP 0x%lx\n", cmd.key);
        struct bucket *b = find_hyperobject(table, cmd.key);
        check_hypertable(table, cmd.key, NULL != b);
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
    }
    table->occupancy = num_valid;
    table->ins_rm_count = num_tomb;
    check_hypertable(table, 1, 0);

    // Run test.
    for (int i = 0; i < num_commands; ++i) {
        do_table_command(table, commands[i]);
    }
    local_hyper_table_free(table);
}

void test0(void) {
    // Basic test case
    table_command test[] = {
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
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test1(void) {
    // Test case derived from trace that led to errors.
    table_command test[] = {
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
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test2(void) {
    // Test case derived from trace that led to errors.
    table_command test[] = {
        {TABLE_INSERT, 0xfffff4e82ed0},
        {TABLE_INSERT, 0xfffff4e82d40},
        {TABLE_INSERT, 0xfffff4e82bb0},
        {TABLE_INSERT, 0xfffff4e82a90},
        {TABLE_INSERT, 0xfffff4e82900},
        {TABLE_INSERT, 0xfffff4e82770},
        {TABLE_INSERT, 0xfffff4e82650},
        {TABLE_INSERT, 0xfffff4e82530},

        {TABLE_DELETE, 0xfffff4e82530},
        {TABLE_DELETE, 0xfffff4e82650},
        {TABLE_DELETE, 0xfffff4e82770},

        // Check that insert succeeds when an element has a larger hash than
        // any elements currently in the table.
        {TABLE_INSERT, 0xfffff4e827e0},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test3(void) {
    // Test case derived from trace that led to errors.
    uintptr_t keys[] = {
        0x7f84b33fef40,
        0x7f84b33fed90,
        KEY_DELETED,
        KEY_DELETED,
        0x7f84b33fee30,
        0x7f84b33fecf0,
        KEY_DELETED,
        KEY_DELETED,
    };
    table_command test[] = {
        {TABLE_INSERT, 0x7f84b33fec50},
        {TABLE_INSERT, 0x3},
        {TABLE_INSERT, 0x4},
        {TABLE_INSERT, 0x1},

        {TABLE_LOOKUP, 0x7f84b33fee30},
        {TABLE_LOOKUP, 0x7f84b33fecf0},
        {TABLE_LOOKUP, 0x7f84b33fec50},
        {TABLE_LOOKUP, 0x7f84b33fef40},
        {TABLE_LOOKUP, 0x7f84b33fed90},
        {TABLE_LOOKUP, 0x3},
        {TABLE_LOOKUP, 0x4},
        {TABLE_LOOKUP, 0x1},
    };
    test_set_insert_remove(keys, sizeof(keys) / sizeof(uintptr_t), test,
                           sizeof(test) / sizeof(table_command));
}

void test4(void) {
    // Test WS+NR and WS+WR inserts
    table_command test[] = {
        {TABLE_INSERT, 0x4},
        {TABLE_INSERT, 0x1},
        {TABLE_INSERT, 0x2},
        {TABLE_INSERT, 0x3},
        {TABLE_INSERT, 0x5},

        {TABLE_INSERT, 0x6},
        {TABLE_DELETE, 0x2},
        {TABLE_INSERT, 0x7},
        {TABLE_DELETE, 0x3},
        {TABLE_INSERT, 0x8},
        {TABLE_DELETE, 0x1},
        {TABLE_DELETE, 0x8},

        {TABLE_INSERT, 0x15}, // NS+NR insert, move 0x7 to wrap
        {TABLE_INSERT, 0x25}, // NS+NR insert, move 0x6 to wrap

        {TABLE_INSERT, 0x2}, // Direct insert into empty slot
        {TABLE_DELETE, 0x2}, // Tombstone left behind
        {TABLE_INSERT, 0x3}, // Direct insert into empty slot
        {TABLE_DELETE, 0x3}, // Tombstone left behind

        {TABLE_DELETE, 0x7}, // Tombstone left behind
        {TABLE_INSERT, 0x7}, // Insert wraps, stops in WS+NR case

        {TABLE_DELETE, 0x6}, // Create tombstone after wrap
        {TABLE_INSERT, 0x8}, // WS+WR insert, must be inserted after 0x7

        {TABLE_LOOKUP, 0x7},
        {TABLE_LOOKUP, 0x8},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test5(void) {
    // Test NS+WR and WS+WR inserts
    table_command test[] = {
        {TABLE_INSERT, 0x4},
        {TABLE_INSERT, 0x1},
        {TABLE_INSERT, 0x2},
        {TABLE_INSERT, 0x3},
        {TABLE_INSERT, 0x5},

        {TABLE_INSERT, 0x6},
        {TABLE_DELETE, 0x2},
        {TABLE_INSERT, 0x7},
        {TABLE_DELETE, 0x3},
        {TABLE_INSERT, 0x8},
        {TABLE_DELETE, 0x1},
        {TABLE_DELETE, 0x8},

        {TABLE_INSERT, 0x15}, // NS+NR insert, move 0x7 to wrap
        {TABLE_INSERT, 0x25}, // NS+NR insert, move 0x6 to wrap
        {TABLE_DELETE, 0x15},
        {TABLE_DELETE, 0x25},

        {TABLE_INSERT, 0x1}, // NS+WR insert, must be inserted after 0x7

        {TABLE_DELETE, 0x7},
        {TABLE_INSERT, 0x3},
        {TABLE_DELETE, 0x6},
        {TABLE_INSERT, 0x7}, // NS+NR insert, searching wrapped tombstones
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test6(void) {
    // WS+WR search, wrapping around whole table
    table_command test[] = {
        {TABLE_INSERT, 0x5},
        {TABLE_INSERT, 0x15},
        {TABLE_INSERT, 0x25},
        {TABLE_INSERT, 0x35},
        {TABLE_INSERT, 0x45},

        {TABLE_INSERT, 0x6},
        {TABLE_INSERT, 0x16},
        {TABLE_INSERT, 0x26},

        {TABLE_DELETE, 0x15},
        {TABLE_LOOKUP, 0x26},

        {TABLE_INSERT, 0x36}, // Scans whole table to insert after 0x26
        {TABLE_LOOKUP, 0x26},
        {TABLE_LOOKUP, 0x35},
        {TABLE_LOOKUP, 0x36},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test7(void) {
    // WS+WR search, wrapping around whole table
    table_command test[] = {
        {TABLE_INSERT, 0x5},
        {TABLE_INSERT, 0x15},
        {TABLE_INSERT, 0x25},
        {TABLE_INSERT, 0x35},
        {TABLE_INSERT, 0x45},
        {TABLE_INSERT, 0x55},
        {TABLE_INSERT, 0x65},
        {TABLE_INSERT, 0x75},

        {TABLE_DELETE, 0x15},

        {TABLE_INSERT, 0x7}, // Scans whole table to insert at index 5
        {TABLE_LOOKUP, 0x35},
        {TABLE_LOOKUP, 0x7},

        {TABLE_DELETE, 0x7},
        {TABLE_DELETE, 0x5},

        {TABLE_INSERT, 0x7}, // Scans whole table to insert at index 5
        {TABLE_INSERT, 0x17},

        {TABLE_DELETE, 0x7},
        {TABLE_DELETE, 0x17},
        {TABLE_INSERT, 0x5},
        {TABLE_DELETE, 0x45},
        {TABLE_DELETE, 0x55},
        {TABLE_DELETE, 0x65},

        {TABLE_INSERT, 0x7},  // Scans whole table to insert at index 5
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

int main(int argc, char *argv[]) {
    int to_run = -1;
    if (argc > 1)
        to_run = atoi(argv[1]);

    if (to_run < 0 || to_run == 0) {
        test0();
        printf("test0 PASSED\n");
    }
    if (to_run < 0 || to_run == 1) {
        test1();
        printf("test1 PASSED\n");
    }
    if (to_run < 0 || to_run == 2) {
        test2();
        printf("test2 PASSED\n");
    }
    if (to_run < 0 || to_run == 3) {
        test3();
        printf("test3 PASSED\n");
    }
    if (to_run < 0 || to_run == 4) {
        test4();
        printf("test4 PASSED\n");
    }
    if (to_run < 0 || to_run == 5) {
        test5();
        printf("test5 PASSED\n");
    }
    if (to_run < 0 || to_run == 6) {
        test6();
        printf("test6 PASSED\n");
    }
    if (to_run < 0 || to_run == 7) {
        test7();
        printf("test7 PASSED\n");
    }
    return 0;
}
