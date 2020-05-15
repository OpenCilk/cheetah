#ifndef QTREE_H_
#define QTREE_H_

#ifndef MAX_SIZE
#define MAX_SIZE 3
#endif

#ifndef MAX_DEPTH
#define MAX_DEPTH 3000000
#endif


#include <stdint.h>
#include <stdlib.h>

#include "./vec.h"
#include "./line.h"
#include "./collision_world.h"
#include "./intersection_event_list.h"

// The quadtree data structure
struct qtree {
  box_dimension xmin;
  box_dimension xmax;
  box_dimension ymin;
  box_dimension ymax;

  int depth;
  
  // Info for the lines stored at this node
  ssize_t cap;
  ssize_t sz;
  ssize_t * dat;

  struct qtree * ne;
  struct qtree * nw;
  struct qtree * se;
  struct qtree * sw;

  // struct qtree * parent;
};
typedef struct qtree qtree;

// allocate a new qtree
qtree * qtree_new(box_dimension xmin, box_dimension xmax, box_dimension ymin, box_dimension ymax, int depth);

// free a new qtree
void qtree_free(qtree * q);

// See if p1 through p4 are within q's bounds.  We treat p1 through p4 as a parallelogram
uint8_t qtree_in(qtree * q, Vec p1, Vec p2, Vec p3, Vec p4);

// Add cw->lines[ind] to q
void qtree_add(qtree * q, ssize_t ind, CollisionWorld * cw);

// Create the children of q and place q's lines in the children
void qtree_split(qtree * q, CollisionWorld * cw);

// Find collisions in q; store them in iel.  dats is used in the recursion to store lines from parents of q.
unsigned int qtree_intersect(qtree * q, ssize_t * dats, ssize_t len, IntersectionEventList * iel, CollisionWorld * cw);

#endif
