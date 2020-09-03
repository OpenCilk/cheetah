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

#include "board.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
// #include "fasttime.h"
#include "ktiming.h"

// board.h defines N, board_t, and helper functions.

#ifndef TIMING_COUNT
#define TIMING_COUNT 1
#endif

// Feel free to make this 0.
#define TO_PRINT (3)  // number of sample solutions to print
#define BITMASK (255) // 8 "1"s

void merge_lists(BoardList *list1, BoardList *list2) {
  if (list2->head == NULL)
    return; // Nothing to do...

  if (list1->head == NULL) {
    // Set list1 to list2
    list1->head = list2->head;
    list1->size = list2->size;
  } else {
    // Append list2 to list1
    list1->tail->next = list2->head;
    list1->size += list2->size;
  }

  list1->tail = list2->tail;

  // Set list2 to empty
  list2->head = NULL;
  list2->tail = NULL;
  list2->size = 0;
}

// Evaluates *left = *left OPERATOR *right.
void board_list_reduce(void *key, void *left, void *right) {
  BoardList *a = (BoardList *)left;
  BoardList *b = (BoardList *)right;
  merge_lists(a, b);
}

// Sets *value to the the identity value.
void board_list_identity(void *key, void *value) {
  BoardList *a = (BoardList *)value;
  a->head = NULL;
  a->tail = NULL;
  a->size = 0;
}

// Destroys any dynamically allocated memory. Hint: delete_nodes.
void board_list_destroy(void *key, void *value) {
  BoardList *a = (BoardList *)value;
  delete_nodes(a);
}

typedef CILK_C_DECLARE_REDUCER(BoardList) BoardListReducer;

BoardListReducer X = CILK_C_INIT_REDUCER(
    BoardList,                                                  // type
    board_list_reduce, board_list_identity, board_list_destroy, // functions
    (BoardList){.head = NULL, .tail = NULL, .size = 0});        // initial value

void queens(board_t cur_board, int row, int down, int left, int right) {
  if (row == N) {
    // A solution to 8 queens!
    append_node(&REDUCER_VIEW(X), cur_board);
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
  init_nodes(&REDUCER_VIEW(X));
  queens((board_t)0, 0, 0, 0, 0);
  BoardList board_list = REDUCER_VIEW(X);

  int num_solutions = board_list.size;

  if (verbose) {
    // Print the first few solutions to check correctness.
    BoardNode *cur_node = board_list.head;
    int num_printed = 0;
    while (cur_node != NULL && num_printed < TO_PRINT) {
      printf("Solution # %d / %d\n", num_printed + 1, num_solutions);
      print_board(cur_node->board);
      cur_node = cur_node->next;
      num_printed++;
    }
  }
  CILK_C_UNREGISTER_REDUCER(X);
  delete_nodes(&board_list);
  return num_solutions;
}

int main(int argc, char *argv[]) {
  clockmark_t begin, end;
  uint64_t running_time[TIMING_COUNT];

  int count = argc > 1 ? atoi(argv[1]) : -1;
  if (count <= 0)
    count = TIMING_COUNT;

  int num_solutions = 92, res = 0;

  for (int i = 0; i < count; i++) {
    begin = ktiming_getmark();
    int run_solutions = run_queens(false);

    res += (num_solutions == run_solutions) ? 1 : 0;
    end = ktiming_getmark();
    running_time[i] = ktiming_diff_usec(&begin, &end);
  }
  if (res == count) {
    printf("Success\n");
  } else {
    printf("Result: %d/%d successes (%d failures)\n", res, count, count - res);
  }
  if (count > 0) {
    print_runtime(running_time, TIMING_COUNT);
  }

  return res != TIMING_COUNT;
}
