#include <cilk/cilk.h>
#include <cilk/reducer.h>
#include <stdio.h>
#include <stdlib.h>

#include "intlist.h"
#include "ktiming.h"

#ifndef TIMING_COUNT
#define TIMING_COUNT 1
#endif

CILK_C_DECLARE_REDUCER(IntList)
my_int_list_reducer =
    CILK_C_INIT_REDUCER(IntList, reduce_IntList, identity_IntList,
                        __cilkrts_hyperobject_noop_destroy, {0});
// Initial value omitted //

void ilist_dac(int lo, int hi, int base) {
    int mid, ctr;

    if (hi - lo < base) {
        for (ctr = lo; ctr < hi; ctr++)
            IntList_append(&REDUCER_VIEW(my_int_list_reducer), ctr);
        return;
    }

    mid = (lo + hi) / 2;

    cilk_spawn ilist_dac(lo, mid, base);

    ilist_dac(mid, hi, base);

    cilk_sync;

    return;
}

int main(int argc, char *args[]) {
    int i;
    int n, res = 0, b = 4;
    clockmark_t begin, end;
    uint64_t running_time[TIMING_COUNT];

    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage: ilist_dac [<cilk-options>] <n> [<b>]\n");
        exit(1);
    }

    n = atoi(args[1]);

    if (argc == 3) {
        b = atoi(args[2]);
    }

    for (i = 0; i < TIMING_COUNT; i++) {
        begin = ktiming_getmark();
        CILK_C_REGISTER_REDUCER(my_int_list_reducer);
        IntList_init(&REDUCER_VIEW(my_int_list_reducer));
        ilist_dac(0, n, b);
        IntList result = REDUCER_VIEW(my_int_list_reducer);
        res += IntList_check(&result, 0, n);
        CILK_C_UNREGISTER_REDUCER(my_int_list_reducer);
        end = ktiming_getmark();
        running_time[i] = ktiming_diff_nsec(&begin, &end);
    }
    printf("Result: %d/%d successes!\n", res, TIMING_COUNT);
    print_runtime(running_time, TIMING_COUNT);

    return 0;
}
