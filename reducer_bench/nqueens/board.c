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

#include "./board.h"

#include <stdio.h>
#include <stdlib.h>

void print_board(board_t board) {
  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      printf("%s ", (board & board_bitmask(row, col)) != 0 ? "Q" : ".");
    }
    printf("\n");
  }
  printf("\n");
}

void append_node(BoardList *board_list, board_t board) {
  BoardNode *new_node = malloc(sizeof(BoardNode));
  if (new_node == NULL) {
    return;
  }
  new_node->board = board;
  new_node->next = NULL;
  if (board_list->head == NULL) {
    board_list->head = new_node;
  } else {
    board_list->tail->next = new_node;
  }
  board_list->tail = new_node;
  board_list->size++;
}

void init_nodes(BoardList *board_list) {
  board_list->head = NULL;
  board_list->tail = NULL;
  board_list->size = 0;
}

void delete_nodes(BoardList *board_list) {
  BoardNode *cur_node = board_list->head;
  BoardNode *next_node = NULL;
  while (cur_node != NULL) {
    next_node = cur_node->next;
    free(cur_node);
    cur_node = next_node;
  }
  board_list->head = NULL;
  board_list->tail = NULL;
  board_list->size = 0;
}
