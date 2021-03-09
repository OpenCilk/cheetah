#include <stdio.h>
#include <stdlib.h>

#include "../runtime/cilk2c.h"
#include "../runtime/cilk2c_inlined.c"
#include "ktiming.h"


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

extern size_t ZERO;
void __attribute__((weak)) dummy(void *p) { return; }

static void __attribute__ ((noinline)) fib_spawn_helper(int *x, int n); 

int fib(int n) {
    int x, y, _tmp;

    if(n < 2)
        return n;

    dummy(alloca(ZERO));
    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame(&sf);

    /* x = spawn fib(n-1) */
    if (!__cilk_prepare_spawn(&sf)) {
      fib_spawn_helper(&x, n-1);
    }

    y = fib(n - 2);

    /* cilk_sync */
    __cilk_sync_nothrow(&sf);
    _tmp = x + y;

    __cilk_parent_epilogue(&sf);

    return _tmp;
}

static void __attribute__ ((noinline)) fib_spawn_helper(int *x, int n) {

    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame_helper(&sf);
    __cilkrts_detach(&sf);
    *x = fib(n);
    __cilk_helper_epilogue(&sf);
}

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
