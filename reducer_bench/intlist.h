#ifndef _INTLIST_H
#define _INTLIST_H

#include <stdio.h>
#include <stdlib.h>

typedef struct _intListNode {
  int value;
  struct _intListNode* next;
} IntListNode;
typedef struct { IntListNode* head; IntListNode* tail; } IntList;

// Initialize a list to be empty
void IntList_init(IntList* list) {
  list->head = list->tail = NULL;
}

// Append an integer to the list
void IntList_append(IntList* list, int x)
{
  IntListNode* node = (IntListNode*) malloc(sizeof(IntListNode));
  node->value = x;
  node->next = NULL;
    
  if (list->tail) {
    list->tail->next = node;
  } else {
    list->head = node;
  }
  list->tail = node;
}

// Append the right list to the left list, and leave the right list
// empty
void IntList_concat(IntList* left, IntList* right)
{
  if (left->head) {
    left->tail->next = right->head;
    if (right->tail) left->tail = right->tail;
  }
  else {
    *left = *right;
  }
  IntList_init(right);
}

void identity_IntList(void* reducer, void* list)
{
  IntList_init((IntList*)list);
}

void reduce_IntList(void* reducer, void* left, void* right)
{
  IntList_concat((IntList*)left, (IntList*)right);
}

// Append an integer to the list
int IntList_check(IntList* list, int lo, int hi)
{
  IntListNode* node = list->head;
  int curr = lo;
  
  if (hi <= lo) return 0;
    
  while (node != NULL) {
    if (node->value != curr) return 0;
    node = node->next;
    curr++;
  }
  
  if (curr != hi) return 0;
  
  return 1;
}

#endif
