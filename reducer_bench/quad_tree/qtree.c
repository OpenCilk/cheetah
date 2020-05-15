#include "./qtree.h"


#include "./vec.h"
#include "./line.h"
#include "./collision_world.h"
#include "./intersection_event_list.h"
#include "./intersection_detection.h"

qtree * qtree_new(box_dimension xmin, box_dimension xmax, box_dimension ymin, box_dimension ymax, int depth) {
  qtree * q = (qtree *)malloc(sizeof(qtree));
  q->xmin = xmin;
  q->xmax = xmax;
  q->ymin = ymin;
  q->ymax = ymax;

  q->depth = depth;
  q->cap = 0;
  q->sz = 0;

  q->dat = NULL;

  q->ne = NULL;
  q->nw = NULL;
  q->se = NULL;
  q->sw = NULL;

  // q->parent = NULL;

  return q;
}

void qtree_free(qtree * q) {
  if (q == NULL) return;

  free(q->dat);

  qtree_free(q->ne);
  qtree_free(q->nw);
  qtree_free(q->se);
  qtree_free(q->sw);

  // qtree_free(q->parent);

  free(q);
}


uint8_t qtree_in(qtree * q, Vec p1, Vec p2, Vec p3, Vec p4) {
  // We use < to deal with degenerate parallelograms lying on box boundaries
  uint8_t ret = q->xmin < p1.x && p1.x < q->xmax && q->ymin < p1.y && p1.y < q->ymax &&
                q->xmin < p2.x && p2.x < q->xmax && q->ymin < p2.y && p2.y < q->ymax &&
                q->xmin < p3.x && p3.x < q->xmax && q->ymin < p3.y && p3.y < q->ymax &&
                q->xmin < p4.x && p4.x < q->xmax && q->ymin < p4.y && p4.y < q->ymax;
  return ret;
}

void qtree_add(qtree * q, ssize_t ind, CollisionWorld * cw) {
  if(q->depth < MAX_DEPTH && q->ne == NULL && q->sz >= MAX_SIZE) qtree_split(q, cw);

  // Get the displacement parallelogram
  Line * l = cw->lines[ind];
  Vec p1 = l->p1;
  Vec p2 = l->p2;
  Vec p3 = Vec_add(p1, Vec_multiply(l->velocity, cw->timeStep));
  Vec p4 = Vec_add(p2, Vec_multiply(l->velocity, cw->timeStep));

  // Test variable for which quadrant it's in
  uint8_t ne = (q->ne != NULL && qtree_in(q->ne, p1, p2, p3, p4));
  uint8_t nw = (q->nw != NULL && qtree_in(q->nw, p1, p2, p3, p4));
  uint8_t se = (q->se != NULL && qtree_in(q->se, p1, p2, p3, p4));
  uint8_t sw = (q->sw != NULL && qtree_in(q->sw, p1, p2, p3, p4));
  uint8_t test = (ne << 3) | (nw << 2) | (se << 1) | sw;

  switch(test) {
  case 8:
    qtree_add(q->ne, ind, cw);
    break;
  case 4:
    qtree_add(q->nw, ind, cw);
    break;
  case 2:
    qtree_add(q->se, ind, cw);
    break;
  case 1:
    qtree_add(q->sw, ind, cw);
    break;
  default:
    // Add to our list of lines
    if (q->cap == q->sz) {
      q->cap = (q->cap == 0) ? MAX_SIZE : 1 + 2 * q->cap;
      q->dat = (ssize_t *)realloc(q->dat, q->cap * sizeof(ssize_t));
    }
    q->dat[q->sz] = ind;
    q->sz++;
    break;
  }
}

void qtree_split(qtree * q, CollisionWorld * cw) {
  box_dimension xmid = (q->xmin + q->xmax) / 2;
  box_dimension ymid = (q->ymin + q->ymax) / 2;
  int d = q->depth + 1;
 
  // Assuming q->ne et al are NULL
  q->ne = qtree_new(xmid, q->xmax, ymid, q->ymax, d);
  // q->ne->parent = q;
  q->nw = qtree_new(xmid, q->xmax, q->ymin, ymid, d);
  // q->nw->parent = q;
  q->se = qtree_new(q->xmin, xmid, ymid, q->ymax, d);
  // q->se->parent = q;
  q->sw = qtree_new(q->xmin, xmid, q->ymin, ymid, d);
  // q->sw->parent = q;

  // Iterate through lines in q; try to move them to q->ne et al
  for(ssize_t i = q->sz - 1; i >= 0; i--) {
    ssize_t ind = q->dat[i];

    Line * l = cw->lines[ind];
    Vec p1 = l->p1;
    Vec p2 = l->p2;
    Vec p3 = Vec_add(p1, Vec_multiply(l->velocity, cw->timeStep));
    Vec p4 = Vec_add(p2, Vec_multiply(l->velocity, cw->timeStep));

    uint8_t ne = (q->ne != NULL && qtree_in(q->ne, p1, p2, p3, p4));
    uint8_t nw = (q->nw != NULL && qtree_in(q->nw, p1, p2, p3, p4));
    uint8_t se = (q->se != NULL && qtree_in(q->se, p1, p2, p3, p4));
    uint8_t sw = (q->sw != NULL && qtree_in(q->sw, p1, p2, p3, p4));
    uint8_t test = (ne << 3) | (nw << 2) | (se << 1) | sw;

    switch(test) {
    case 8:
      qtree_add(q->ne, ind, cw);
      // Swap the element to be removed with the last element
      q->dat[i] ^= q->dat[q->sz - 1]; 
      q->dat[q->sz - 1] ^= q->dat[i];
      q->dat[i] ^= q->dat[q->sz - 1];
      // Make the new last element junk
      q->sz--;
      break;
    case 4:
      qtree_add(q->nw, ind, cw);
      q->dat[i] ^= q->dat[q->sz - 1]; 
      q->dat[q->sz - 1] ^= q->dat[i];
      q->dat[i] ^= q->dat[q->sz - 1];
      q->sz--;
      break;
    case 2:
      qtree_add(q->se, ind, cw);
      q->dat[i] ^= q->dat[q->sz - 1]; 
      q->dat[q->sz - 1] ^= q->dat[i];
      q->dat[i] ^= q->dat[q->sz - 1];
      q->sz--;
      break;
    case 1:
      qtree_add(q->sw, ind, cw);
      q->dat[i] ^= q->dat[q->sz - 1]; 
      q->dat[q->sz - 1] ^= q->dat[i];
      q->dat[i] ^= q->dat[q->sz - 1];
      q->sz--;
      break;
    default:
      break;
    }
  }

  // if q->dat is too large, free some space
  if (q->sz < q->cap / 3 && q->cap > MAX_SIZE) {
    q->cap = 1 + 2 * q->sz;
    q->dat = (ssize_t *)realloc(q->dat, q->cap * sizeof(ssize_t));
  }
}

unsigned int qtree_intersect(qtree * q, ssize_t * dats, ssize_t len, IntersectionEventList * iel, CollisionWorld * cw) {
  unsigned int numLineLineCollisions = 0;

  // Test info in dats vs q->dat lines
  for (ssize_t i = 0; i < len; i++) {
    for (ssize_t j = 0; j < q->sz; j++) {
      Line * l1 = cw->lines[dats[i]];
      Line * l2 = cw->lines[q->dat[j]];

      if (compareLines(l1, l2) >= 0) {
        Line *temp = l1;
        l1 = l2;
        l2 = temp;
      }

      IntersectionType intersectionType =
          intersect(l1, l2, cw->timeStep);
      if (intersectionType != NO_INTERSECTION) {
        IntersectionEventList_appendNode(iel, l1, l2,
                                         intersectionType);
        numLineLineCollisions++;
      }
    }
  }

  // Test within q->dat for collisions
  for (ssize_t i = 0; i < q->sz; i++) {
    for (ssize_t j = i + 1; j < q->sz; j++) {
      Line * l1 = cw->lines[q->dat[i]];
      Line * l2 = cw->lines[q->dat[j]];

      if (compareLines(l1, l2) >= 0) {
        Line *temp = l1;
        l1 = l2;
        l2 = temp;
      }

      IntersectionType intersectionType =
          intersect(l1, l2, cw->timeStep);
      if (intersectionType != NO_INTERSECTION) {
        IntersectionEventList_appendNode(iel, l1, l2,
                                         intersectionType);
        numLineLineCollisions++;
      }
    }
  }

  for (ssize_t i = 0; i < q->sz; i++) {
    dats[len + i] = q->dat[i];
  }

  // Recursions
  if (q->ne != NULL) numLineLineCollisions += qtree_intersect(q->ne, dats, len + q->sz, iel, cw);
  if (q->nw != NULL) numLineLineCollisions += qtree_intersect(q->nw, dats, len + q->sz, iel, cw);
  if (q->se != NULL) numLineLineCollisions += qtree_intersect(q->se, dats, len + q->sz, iel, cw);
  if (q->sw != NULL) numLineLineCollisions += qtree_intersect(q->sw, dats, len + q->sz, iel, cw);

  return numLineLineCollisions;
}
