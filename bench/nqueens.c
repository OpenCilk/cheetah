#ifndef TIMING_COUNT
#define TIMING_COUNT 0
#endif

#if TIMING_COUNT
#include "ktiming.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cilk/cilk.h>

// int * count;

/*
 * nqueen  4 = 2
 * nqueen  5 = 10
 * nqueen  6 = 4
 * nqueen  7 = 40
 * nqueen  8 = 92
 * nqueen  9 = 352
 * nqueen 10 = 724
 * nqueen 11 = 2680
 * nqueen 12 = 14200
 * nqueen 13 = 73712
 * nqueen 14 = 365596
 * nqueen 15 = 2279184
 */

/*
 * <a> contains array of <n> queen positions.  Returns 1
 * if none of the queens conflict, and returns 0 otherwise.
 */
static int ok(int n, char *a) {

    int i, j;
    char p, q;

    for (i = 0; i < n; i++) {
        p = a[i];
        for (j = i + 1; j < n; j++) {
            q = a[j];
            if (q == p || q == p - (j - i) || q == p + (j - i))
                return 0;
        }
    }

    return 1;
}

static int nqueens(int n, int j, char *a) {

    char *b;
    int i;
    int *count;
    int solNum = 0;

    if (n == j) {
        return 1;
    }

    count = (int *)alloca(n * sizeof(int));
    (void)memset(count, 0, n * sizeof(int));

    for (i = 0; i < n; i++) {

        /***
         * Strictly speaking, this (alloca after spawn) is frowned
         * up on, but in this case, this is ok, because b returned by
         * alloca is only used in this iteration; later spawns don't
         * need to be able to access copies of b from previous iterations
         ***/
        b = (char *)alloca((j + 1) * sizeof(char));
        memcpy(b, a, j * sizeof(char));
        b[j] = i;

        if (ok(j + 1, b)) {
            count[i] = cilk_spawn nqueens(n, j + 1, b);
        }
    }
    cilk_sync;

    for (i = 0; i < n; i++) {
        solNum += count[i];
    }

    return solNum;
}

int main(int argc, char *argv[]) {

    const char *prog = argv[0];

    int n = 13;

    if (argc > 1 && !strcmp(argv[1], "-c")) {
        --argc;
        ++argv;
    }

    switch (argc) {
    case 1:
        break;
    case 2:
        n = atoi(argv[1]);
        if (n <= 0 || n > 100) {
            fprintf(stderr, "Invalid board size %s\n", argv[1]);
            exit(1);
        }
        printf("Running %s with n = %d.\n", prog, n);
        break;
    default:
        fprintf(stderr, "Usage: %s <n>\n", prog);
        fprintf(stderr, "Use default board size, n = 13.\n");
        exit(1);
    }

    int res = 0;
    char a[n];

#if TIMING_COUNT
    clockmark_t begin, end;
    uint64_t elapsed[TIMING_COUNT];

    for (int i = 0; i < TIMING_COUNT; i++) {
        begin = ktiming_getmark();
        res = nqueens(n, 0, a);
        end = ktiming_getmark();
        elapsed[i] = ktiming_diff_nsec(&begin, &end);
    }
    print_runtime(elapsed, TIMING_COUNT);
#else
    res = nqueens(n, 0, a);
#endif

    if (res == 0) {
        printf("No solution found.\n");
        exit(1);
    } else {
        printf("Total number of solutions : %d\n", res);
    }

    return 0;
}
