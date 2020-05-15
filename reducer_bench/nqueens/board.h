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

#ifndef _BOARD_H_
#define _BOARD_H_

#include <stdint.h>

#define N (8)

// Saves a queen in (row, col) in the (row * N + col)th bit.
// row, col must be 0-indexed.
typedef uint64_t board_t;

static inline board_t board_bitmask(int row, int col) {
  return ((board_t)1) << (row * N + col);
}

void print_board(board_t board);

struct BoardNode {
  board_t board;
  struct BoardNode *next;
};
typedef struct BoardNode BoardNode;

struct BoardList {
  BoardNode *head;
  BoardNode *tail;
  int size;
};
typedef struct BoardList BoardList;

void append_node(BoardList *board_list, board_t board);

void init_nodes(BoardList *board_list);

void delete_nodes(BoardList *board_list);

#endif // _BOARD_H_
