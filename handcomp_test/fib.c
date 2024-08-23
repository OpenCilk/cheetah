#include <stdio.h>
#include <stdlib.h>

#include "ktiming.h"

#define ENABLE_UNSAFE_C2CILK_LIBRARY 0xda179e12
#include <c2cilk/c2cilk.h>


#ifndef TIMING_COUNT 
#define TIMING_COUNT 1 
#endif

/* 
 * fib 39: 63245986
 * fib 40: 102334155
 * fib 41: 165580141 
 * fib 42: 267914296
   
int fib(int n) {
    int x, y, _tmp;

    if(n < 2) {
        return n;
    }
    
    x = cilk_spawn fib(n - 1);
    y = fib(n - 2);
    cilk_sync;

    return x+y;
}
*/

C2CILK_FUNC(int, fib, (int, n), {
    int x = 0, y;

    if(n < 2)
        return n;

    c2cilk_context(
        /* x = spawn fib(n-1) */
        c2cilk_spawn(&x, fib, n-1);
        y = fib(n - 2);

        // Implicit cilk_sync
    )

    return x + y;
})

int main(int argc, char * args[]) {
    int i;
    int n, res;
    clockmark_t begin, end; 
    uint64_t running_time[TIMING_COUNT];

    if(argc != 2) {
        fprintf(stderr, "Usage: fib [<cilk-options>] <n>\n");
        exit(1);
    }
    
    n = atoi(args[1]);

    for(i = 0; i < TIMING_COUNT; i++) {
        begin = ktiming_getmark();
        res = fib(n);
        end = ktiming_getmark();
        running_time[i] = ktiming_diff_nsec(&begin, &end);
    }
    printf("Result: %d\n", res);
    print_runtime(running_time, TIMING_COUNT); 

    return 0;
}
