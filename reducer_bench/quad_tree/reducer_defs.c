#include "./reducer_defs.h"

// Evaluates *left = *left OPERATOR *right
void IEL_merge(IntersectionEventList * left, IntersectionEventList * right) {
  if (right->head == NULL) return;
  if (left->head == NULL) left->head = right->head;
  else left->tail->next = right->head;
  left->tail = right->tail;
  left->size += right->size;
}

// Evaluates *left = *left OPERATOR *right
void IEL_list_reduce(void * key, void * left, void * right) {
  IEL_merge((IntersectionEventList *)left, (IntersectionEventList *)right);
}

// Sets *value to the the identity value.
void IEL_list_identity(void* key, void* value) {
  IntersectionEventList * v = (IntersectionEventList *)value;
  v->head = NULL;
  v->tail = NULL;
  v->size = 0;
}
