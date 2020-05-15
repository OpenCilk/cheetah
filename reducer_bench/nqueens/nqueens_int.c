/**
 * Copyright (c) 2013 MIT License by 6.172 Staff
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 **/

#include <cilk/cilk.h>
#include <cilk/reducer.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
// #include "board.h"
// #include "fasttime.h"
#include "ktiming.h"

// board.h defines N, board_t, and helper functions.

#ifndef TIMING_COUNT
#define TIMING_COUNT 1
#endif

// Feel free to make this 0.
#define TO_PRINT (3)  // number of sample solutions to print
#define BITMASK (255) // 8 "1"s
#define N (8)

// Saves a queen in (row, col) in the (row * N + col)th bit.
// row, col must be 0-indexed.
typedef uint64_t board_t;

static inline board_t board_bitmask(int row, int col) {
  return ((board_t)1) << (row * N + col);
}
typedef int BoardList;

void board_list_identity(void *reducer, void *sum) { *((int *)sum) = 0; }

void board_list_reduce(void *reducer, void *left, void *right) {
  *((int *)left) += *((int *)right);
}

typedef CILK_C_DECLARE_REDUCER(BoardList) BoardListReducer;

BoardListReducer X =
    CILK_C_INIT_REDUCER(BoardList, // type
                        board_list_reduce, board_list_identity,
                        __cilkrts_hyperobject_noop_destroy, // functions
                        (BoardList)0);                      // initial value

void queens(board_t cur_board, int row, int down, int left, int right) {
  if (row == N) {
    // A solution to 8 queens!
    REDUCER_VIEW(X) += 1;
  } else {
    int open_cols_bitmap = BITMASK & ~(down | left | right);

    while (open_cols_bitmap != 0) {
      int bit = -open_cols_bitmap & open_cols_bitmap;
      int col = log2(bit);
      open_cols_bitmap ^= bit;

      // Recurse! This can be parallelized.
      cilk_spawn queens(cur_board | board_bitmask(row, col), row + 1,
                        down | bit, (left | bit) << 1, (right | bit) >> 1);
    }
    cilk_sync;
  }
}

int run_queens(bool verbose) {
  CILK_C_REGISTER_REDUCER(X);
  *(&REDUCER_VIEW(X)) = 0;
  queens((board_t)0, 0, 0, 0, 0);
  BoardList board_list = REDUCER_VIEW(X);

  int num_solutions = board_list;

  CILK_C_UNREGISTER_REDUCER(X);
  return num_solutions;
}

int main(int argc, char *argv[]) {
  int i;
  clockmark_t begin, end;
  uint64_t running_time[TIMING_COUNT];

  int num_solutions = 92, res = 0;

  for (i = 0; i < TIMING_COUNT; i++) {
    begin = ktiming_getmark();
    int run_solutions = run_queens(false);

    res += (num_solutions == run_solutions) ? 1 : 0;
    end = ktiming_getmark();
    running_time[i] = ktiming_diff_usec(&begin, &end);
  }
  printf("Result: %d/%d successes!\n", res, TIMING_COUNT);
  print_runtime(running_time, TIMING_COUNT);

  return 0;
}
