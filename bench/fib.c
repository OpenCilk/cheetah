#include "ktiming.h"
#include <cilk/cilk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TIMING_COUNT
#define TIMING_COUNT 1
#endif

#ifdef SIMULATE_RACE
int race = 0;
#endif

static const int expected[] = {
    0, 1, 1, 2, 3, 5, 8, 13, 21, 34,
    /* 10 */
    55, 89, 144, 233, 377, 610, 987, 1597, 2584, 4181,
    /* 20 */
    6765, 10946, 17711, 28657, 46368, 75025, 121393, 196418, 317811, 514229,
    /* 30 */
    832040, 1346269, 2178309, 3524578, 5702887, 9227465, 14930352, 24157817,
    39088169, 63245986,
    /* 40 */
    102334155, 165580141, 267914296, 433494437, 701408733, 1134903170};

extern int fib(int);

/* Without the weak attribute llvm recognizes that the fib() always
   returns the same result for the same argument and moves the call
   outside of the timing loop. */
__attribute__((weak)) int fib(int n) {
#ifdef SIMULATE_RACE
    ++race;
#endif
    int x, y;

    if (n < 2) {
        return n;
    }

    x = cilk_spawn fib(n - 1);
    y = fib(n - 2);
    cilk_sync;
    return x + y;
}

int main(int argc, char *args[]) {
    int i;
    int n;
    uint64_t running_time[TIMING_COUNT];
    int res[TIMING_COUNT];
    _Bool check = 0;

    if (argc > 2 && !strcmp(args[1], "-c")) {
        ++args;
        --argc;
        check = 1;
    }

    if (argc != 2) {
        fprintf(stderr, "Usage: fib [<cilk-options>] <n>\n");
        exit(1);
    }

    n = atoi(args[1]);

    for (i = 0; i < TIMING_COUNT; i++) {
        clockmark_t begin = ktiming_getmark();
        res[i] = fib(n);
        clockmark_t end = ktiming_getmark();
        running_time[i] = ktiming_diff_nsec(&begin, &end);
    }

    char const *result = "(unchecked)";
    int status = 0;
    if (check && (size_t)n < sizeof expected / sizeof expected[0]) {
        for (int i = 0; i < TIMING_COUNT; ++i) {
            if (expected[n] != res[i]) {
                status = 1;
                break;
            }
        }
        result = status ? "(incorrect)" : "(correct)";
    }

    printf("Result: %d %s\n", res[0], result);

    if (TIMING_COUNT > 10)
        print_runtime_summary(running_time, TIMING_COUNT);
    else
        print_runtime(running_time, TIMING_COUNT);

    return status;
}
