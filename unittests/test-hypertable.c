#include "test-hypertable-common.h"

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
    // Test WP+NR and WP+WR inserts
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

        {TABLE_INSERT, 0x15}, // NP+NR insert, move 0x7 to wrap
        {TABLE_INSERT, 0x25}, // NP+NR insert, move 0x6 to wrap

        {TABLE_INSERT, 0x2}, // Direct insert into empty slot
        {TABLE_DELETE, 0x2}, // Tombstone left behind
        {TABLE_INSERT, 0x3}, // Direct insert into empty slot
        {TABLE_DELETE, 0x3}, // Tombstone left behind

        {TABLE_DELETE, 0x7}, // Tombstone left behind
        {TABLE_INSERT, 0x7}, // Insert wraps, stops in WP+NR case

        {TABLE_DELETE, 0x6}, // Create tombstone after wrap
        {TABLE_INSERT, 0x8}, // WP+WR insert, must be inserted after 0x7

        {TABLE_LOOKUP, 0x7},
        {TABLE_LOOKUP, 0x8},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test2(void) {
    // Test NS+WR and WP+WR inserts
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

        {TABLE_INSERT, 0x15}, // NP+NR insert, move 0x7 to wrap
        {TABLE_INSERT, 0x25}, // NP+NR insert, move 0x6 to wrap
        {TABLE_DELETE, 0x15},
        {TABLE_DELETE, 0x25},

        {TABLE_INSERT, 0x1}, // NP+WR insert, must be inserted after 0x7
        {TABLE_LOOKUP, 0x1},

        {TABLE_DELETE, 0x7},
        {TABLE_INSERT, 0x3},
        {TABLE_DELETE, 0x6},
        {TABLE_INSERT, 0x7}, // NP+NR insert, searching wrapped tombstones

        {TABLE_LOOKUP, 0x7},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test3(void) {
    // WP+WR search, wrapping around whole table
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

        {TABLE_INSERT, 0x37}, // Scans ~whole table to insert

        {TABLE_LOOKUP, 0x5},
        {TABLE_LOOKUP, 0x25},
        {TABLE_LOOKUP, 0x35},
        {TABLE_LOOKUP, 0x45},
        {TABLE_LOOKUP, 0x6},
        {TABLE_LOOKUP, 0x16},
        {TABLE_LOOKUP, 0x26},
        {TABLE_LOOKUP, 0x37},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test4(void) {
    // WP+WR search, wrapping around whole table
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
        {TABLE_LOOKUP, 0x35},
        {TABLE_DELETE, 0x5},

        {TABLE_INSERT, 0x7}, // Scans ~whole table to insert
        {TABLE_INSERT, 0x17}, // Trigger table rebuild

        {TABLE_DELETE, 0x7},
        {TABLE_DELETE, 0x17},

        {TABLE_INSERT, 0x5},
        {TABLE_LOOKUP, 0x5},
        {TABLE_DELETE, 0x65},
        {TABLE_DELETE, 0x75},
        {TABLE_DELETE, 0x35},

        {TABLE_INSERT, 0x7}, // Scans whole table to insert after hash = 5 run

        {TABLE_LOOKUP, 0x7},
        {TABLE_LOOKUP, 0x5},
        {TABLE_LOOKUP, 0x25},
        {TABLE_LOOKUP, 0x45},
        {TABLE_LOOKUP, 0x55},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test5(void) {
    // WP+WR and NP+NR tests, wrapping around whole table with min elements.
    // Test inserting into into the middle of a run of tombstones, where insert
    // scan would wrap around the whole table.
    table_command test[] = {
        {TABLE_INSERT, 0x8},
        {TABLE_INSERT, 0x18},
        {TABLE_INSERT, 0x28},
        {TABLE_INSERT, 0x38},
        {TABLE_INSERT, 0x7},
        {TABLE_INSERT, 0x17},
        {TABLE_INSERT, 0x27},
        {TABLE_INSERT, 0x37},

        {TABLE_DELETE, 0x37},
        {TABLE_DELETE, 0x27},
        {TABLE_DELETE, 0x17},
        {TABLE_DELETE, 0x7},

         // NP+NR insert in middle of tombstones, where all other elements have
         // smaller hashes.  Must insert after hash = 8 run.
        {TABLE_INSERT, 0x1},

        {TABLE_LOOKUP, 0x1},
        {TABLE_LOOKUP, 0x8},
        {TABLE_LOOKUP, 0x18},
        {TABLE_LOOKUP, 0x28},
        {TABLE_LOOKUP, 0x38},
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
    return 0;
}
