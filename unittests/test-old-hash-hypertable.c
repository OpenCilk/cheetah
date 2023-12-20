#include "test-hypertable-common.h"

// Test cases derived from traces that led to errors.

void test0(void) {
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

void test1(void) {
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

        {TABLE_DELETE, 0xfffff4e82bb0},

        // Check that insert succeeds when an element has a larger hash than
        // any elements currently in the table.
        {TABLE_INSERT, 0xfffff4e827e0},
    };
    test_insert_remove(test, sizeof(test)/sizeof(table_command));
}

void test2(void) {
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
        {TABLE_LOOKUP, 0x7f84b33fee30},

        {TABLE_INSERT, 0x7f84b33fec50},
        {TABLE_LOOKUP, 0x7f84b33fec50},

        {TABLE_INSERT, 0x3},
        {TABLE_LOOKUP, 0x7f84b33fee30},

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
    return 0;
}
