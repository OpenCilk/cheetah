#include <stdio.h>
#include <stdlib.h>
#include <cilk/cilk.h>
#include <cilk/reducer_opadd.h>

extern "C" {
#include "ktiming.h"
}

int test_reducer(int limit) {

  cilk::reducer_opadd<int> radd(0);
  
  cilk_for(int i = 0; i < limit; i++) {
    *radd += 1;
  }

  int sum = radd.get_value();

  return sum;
}

int main(int argc, const char** args) {
  int i;
  int n, res = 0;
  clockmark_t begin, end; 
  uint64_t running_time[TIMING_COUNT];

  if(argc != 2) {
    fprintf(stderr, "Usage: ilist_dac [<cilk-options>] <n>\n");
    exit(1);
  }
    
  n = atoi(args[1]);

  for(i = 0; i < TIMING_COUNT; i++) {
    begin = ktiming_getmark();
    int sum = test_reducer(n);
    res += (sum == n) ? 1 : 0;
    end = ktiming_getmark();
    // printf("The final sum is %d\n", sum);
    running_time[i] = ktiming_diff_nsec(&begin, &end);
  }
  printf("Result: %d/%d successes!\n", res, TIMING_COUNT);
  print_runtime(running_time, TIMING_COUNT); 

  return 0;
}
