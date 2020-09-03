#ifndef TIMING_COUNT
#define TIMING_COUNT 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/cilk2c.h"
#include "../runtime/cilk2c_inlined.c"
#include "ktiming.h"

extern size_t ZERO;
void __attribute__((weak)) dummy(void *p) { return; }

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
static int ok (int n, char *a) {

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

static void __attribute__ ((noinline))
nqueens_spawn_helper(int *count, int n, int j, char *a); 

static int nqueens(int n, int j, char *a) {

    char *b;
    int i;
    int *count;
    int solNum = 0;

    if (n == j) {
        return 1;
    }

    count = (int *) alloca(n * sizeof(int));
    (void) memset(count, 0, n * sizeof (int));

    dummy(alloca(ZERO));
    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame(&sf);

    for (i = 0; i < n; i++) {

        /***
         * Strictly speaking, this (alloca after spawn) is frowned 
         * up on, but in this case, this is ok, because b returned by 
         * alloca is only used in this iteration; later spawns don't 
         * need to be able to access copies of b from previous iterations 
         ***/
        b = (char *) alloca((j + 1) * sizeof (char));
        memcpy(b, a, j * sizeof (char));
        b[j] = i;

        if(ok (j + 1, b)) {

            /* count[i] = cilk_spawn nqueens(n, j + 1, b); */
            __cilkrts_save_fp_ctrl_state(&sf);
            if(!__builtin_setjmp(sf.ctx)) {
                nqueens_spawn_helper(&(count[i]), n, j+1, b);
            }
        }
    }
    /* cilk_sync */
    if(sf.flags & CILK_FRAME_UNSYNCHED) {
      __cilkrts_save_fp_ctrl_state(&sf);
      if(!__builtin_setjmp(sf.ctx)) {
        __cilkrts_sync(&sf);
      }
    }

    for(i = 0; i < n; i++) {
        solNum += count[i];
    }

    __cilkrts_pop_frame(&sf);
    if (0 != sf.flags)
        __cilkrts_leave_frame(&sf);

    return solNum;
}

static void __attribute__ ((noinline)) 
nqueens_spawn_helper(int *count, int n, int j, char *a) {

    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame_fast(&sf);
    __cilkrts_detach(&sf);
    *count = nqueens(n, j, a);
    __cilkrts_pop_frame(&sf);
    __cilkrts_leave_frame(&sf); 
}

int main(int argc, char *argv[]) {

  int n = 13;
  char *a;
  int res;

  if(argc < 2) {
      fprintf (stderr, "Usage: %s <n>\n", argv[0]);
      fprintf (stderr, "Use default board size, n = 13.\n");
      exit(0);
  } else {
      n = atoi (argv[1]);
      printf ("Running %s with n = %d.\n", argv[0], n);
  }

  a = (char *) alloca (n * sizeof (char));
  res = 0;

#if TIMING_COUNT
  clockmark_t begin, end;
  uint64_t elapsed[TIMING_COUNT];

  for(int i=0; i < TIMING_COUNT; i++) {
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
  } else {
      printf("Total number of solutions : %d\n", res);
  }

  return 0;
}
