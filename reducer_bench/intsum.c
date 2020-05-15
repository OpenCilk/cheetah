#include <cilk/cilk.h>
#include <cilk/reducer.h>
#include <stdio.h>
#include <stdlib.h>

#include "ktiming.h"

void identity_longsum(void *reducer, void *sum) { *((long *)sum) = 0; }

void reduce_longsum(void *reducer, void *left, void *right) {
    *((long *)left) += *((long *)right);
}

CILK_C_DECLARE_REDUCER(long)
my_int_sum_reducer = CILK_C_INIT_REDUCER(long, reduce_longsum, identity_longsum,
                                         __cilkrts_hyperobject_noop_destroy, 0);

void compute_sum(long limit, int scale) {
    for (long i = 0; i < limit; i++) {
        REDUCER_VIEW(my_int_sum_reducer) += scale;
    }
}

void test_reducer(long limit) {
    for (int t = 1; t < 100; ++t) {
        cilk_spawn compute_sum(limit, t);
    }
    compute_sum(limit, 100);
    cilk_sync;
}

int main(int argc, const char **args) {
    long i, n;
    int res = 0;
    clockmark_t begin, end;
    uint64_t running_time[TIMING_COUNT];

    if (argc != 2) {
        fprintf(stderr, "Usage: ilist_dac [<cilk-options>] <n>\n");
        exit(1);
    }

    n = atol(args[1]);

    const long scale = 100 * 101 / 2;

    for (i = 0; i < TIMING_COUNT; i++) {
        begin = ktiming_getmark();
        CILK_C_REGISTER_REDUCER(my_int_sum_reducer);
        *(&REDUCER_VIEW(my_int_sum_reducer)) = 0;
        test_reducer(n);
        long sum = REDUCER_VIEW(my_int_sum_reducer);
        res += (sum == scale * n) ? 1 : 0;
        CILK_C_UNREGISTER_REDUCER(my_int_sum_reducer);
        end = ktiming_getmark();
        // prlongf("The final sum is %d\n", sum);
        running_time[i] = ktiming_diff_nsec(&begin, &end);
    }
    printf("Result: %d/%d successes!\n", res, TIMING_COUNT);
    print_runtime(running_time, TIMING_COUNT);

    return res != TIMING_COUNT;
}
