// #define LIVE
/** 
 * collision_world.c -- detect and handle line segment intersections
 * Copyright (c) 2012 the Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. 
 **/

#include "./collision_world.h"

#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>

#include "./intersection_detection.h"
#include "./intersection_event_list.h"
#include "./line.h"

#include "./bin.h"

#include <cilk/cilk.h>
#include "./reducer_defs.h"

// ssize_t * dats = NULL;
bin_manager_t * man = NULL;

CollisionWorld* CollisionWorld_new(const unsigned int capacity) {
  assert(capacity > 0);

  CollisionWorld* collisionWorld = malloc(sizeof(CollisionWorld));
  if (collisionWorld == NULL) {
    return NULL;
  }

  collisionWorld->numLineWallCollisions = 0;
  collisionWorld->numLineLineCollisions = 0;
  collisionWorld->timeStep = 0.5;
  collisionWorld->lines = malloc(capacity * sizeof(Line*));
  collisionWorld->numOfLines = 0;
  
  
  // dats = (ssize_t *)malloc(capacity * sizeof(ssize_t));
  man = new_bin_manager();
  
  return collisionWorld;
}

void CollisionWorld_delete(CollisionWorld* collisionWorld) {
  for (int i = 0; i < collisionWorld->numOfLines; i++) {
    free(collisionWorld->lines[i]);
  }
  free(collisionWorld->lines);
  free(collisionWorld);
  
  // free(dats);
  free_bin_manager(man);
}

unsigned int CollisionWorld_getNumOfLines(CollisionWorld* collisionWorld) {
  return collisionWorld->numOfLines;
}

void CollisionWorld_addLine(CollisionWorld* collisionWorld, Line *line) {
  collisionWorld->lines[collisionWorld->numOfLines] = line;
  collisionWorld->numOfLines++;
}

Line* CollisionWorld_getLine(CollisionWorld* collisionWorld,
                             const unsigned int index) {
  if (index >= collisionWorld->numOfLines) {
    return NULL;
  }
  return collisionWorld->lines[index];
}

void CollisionWorld_updateLines(CollisionWorld* collisionWorld) {
  CollisionWorld_detectIntersection(collisionWorld);
  CollisionWorld_updatePosition(collisionWorld);
  CollisionWorld_lineWallCollision(collisionWorld);
}

void lineUpdate(Line * line, double t) {
  Vec v = Vec_multiply(line->velocity, t);
  line->p1 = Vec_add(line->p1, v);
  line->p2 = Vec_add(line->p2, v);
}

void CollisionWorld_updatePosition(CollisionWorld* collisionWorld) {
  double t = collisionWorld->timeStep;
  unsigned int n = collisionWorld->numOfLines;

  // Unrolled loop
  for (int i = 0; i + 7 < n; i += 8) {
    lineUpdate(collisionWorld->lines[i], t);
    lineUpdate(collisionWorld->lines[i + 1], t);
    lineUpdate(collisionWorld->lines[i + 2], t);
    lineUpdate(collisionWorld->lines[i + 3], t);
    lineUpdate(collisionWorld->lines[i + 4], t);
    lineUpdate(collisionWorld->lines[i + 5], t);
    lineUpdate(collisionWorld->lines[i + 6], t);
    lineUpdate(collisionWorld->lines[i + 7], t);
  }

  for (int i = n - (n & 7); i < collisionWorld->numOfLines; i++) {
    lineUpdate(collisionWorld->lines[i], t);
  }
}

bool wallLine(Line * line) {
    bool collide = false;

    // Right side
    if ((line->p1.x > BOX_XMAX || line->p2.x > BOX_XMAX)
        && (line->velocity.x > 0)) {
      line->velocity.x = -line->velocity.x;
      collide = true;
    }
    // Left side
    if ((line->p1.x < BOX_XMIN || line->p2.x < BOX_XMIN)
        && (line->velocity.x < 0)) {
      line->velocity.x = -line->velocity.x;
      collide = true;
    }
    // Top side
    if ((line->p1.y > BOX_YMAX || line->p2.y > BOX_YMAX)
        && (line->velocity.y > 0)) {
      line->velocity.y = -line->velocity.y;
      collide = true;
    }
    // Bottom side
    if ((line->p1.y < BOX_YMIN || line->p2.y < BOX_YMIN)
        && (line->velocity.y < 0)) {
      line->velocity.y = -line->velocity.y;
      collide = true;
    }
    return collide;
}

// Parallelizing this causes an 0.02 to 0.09 s slowdown
void CollisionWorld_lineWallCollision_orig(CollisionWorld* collisionWorld) {
  unsigned int n = collisionWorld->numOfLines;
  //Unrolled loop
  for (int i = 0; i + 7 < n; i += 8) {
    Line * line0 = collisionWorld->lines[i];
    Line * line1 = collisionWorld->lines[i + 1];
    Line * line2 = collisionWorld->lines[i + 2];
    Line * line3 = collisionWorld->lines[i + 3];
    Line * line4 = collisionWorld->lines[i + 4];
    Line * line5 = collisionWorld->lines[i + 5];
    Line * line6 = collisionWorld->lines[i + 6];
    Line * line7 = collisionWorld->lines[i + 7];

    uint8_t res = 0;

    res += (wallLine(line0)) ? 1 : 0;
    res += (wallLine(line1)) ? 1 : 0;
    res += (wallLine(line2)) ? 1 : 0;
    res += (wallLine(line3)) ? 1 : 0;
    res += (wallLine(line4)) ? 1 : 0;
    res += (wallLine(line5)) ? 1 : 0;
    res += (wallLine(line6)) ? 1 : 0;
    res += (wallLine(line7)) ? 1 : 0;

    // Update total number of collisions.
    collisionWorld->numLineWallCollisions += res;
  }

  for (int i = n - (n & 7); i < collisionWorld->numOfLines; i++) {
    Line *line = collisionWorld->lines[i];
    bool collide = wallLine(line);
    // Update total number of collisions.
    if (collide == true) {
      collisionWorld->numLineWallCollisions++;
    }
  }
}

void CollisionWorld_lineWallCollision(CollisionWorld* collisionWorld) {
  CollisionWorld_lineWallCollision_orig(collisionWorld);
}

// That godawful bubblesort, separated out for readability.
void IntersectionEventList_sort(IntersectionEventList * iel) {
  // Sort the intersection event list.
  IntersectionEventNode* startNode = iel->head;
  while (startNode != NULL) {
    IntersectionEventNode* minNode = startNode;
    IntersectionEventNode* curNode = startNode->next;
    while (curNode != NULL) {
      if (IntersectionEventNode_compareData(curNode, minNode) < 0) {
        minNode = curNode;
      }
      curNode = curNode->next;
    }
    if (minNode != startNode) {
      IntersectionEventNode_swapData(minNode, startNode);
    }
    startNode = startNode->next;
  }
}

// The original collision detection
unsigned int CollisionWorld_IntersectionEventList_orig(IntersectionEventList * iel, CollisionWorld* collisionWorld) {
  unsigned int numLineLineCollisions = 0;
  // Test all line-line pairs to see if they will intersect before the
  // next time step.
  for (int i = 0; i < collisionWorld->numOfLines; i++) {

    for (int j = i + 1; j < collisionWorld->numOfLines; j++) {
      Line *l1 = collisionWorld->lines[i];  // NOTE: I moved this in, as per https://piazza.com/class/ism98km5jrx2fl?cid=290
      Line *l2 = collisionWorld->lines[j];

      // intersect expects compareLines(l1, l2) < 0 to be true.
      // Swap l1 and l2, if necessary.
      if (compareLines(l1, l2) >= 0) {
        Line *temp = l1;
        l1 = l2;
        l2 = temp;
      }

      IntersectionType intersectionType =
          intersect(l1, l2, collisionWorld->timeStep);
      if (intersectionType != NO_INTERSECTION) {
        IntersectionEventList_appendNode(iel, l1, l2,
                                         intersectionType);
        numLineLineCollisions++;
      }
    }
  }
  
  IntersectionEventList_sort(iel);
  
  return numLineLineCollisions;
}

unsigned int CollisionWorld_IntersectionEventList_bin(IntersectionEventList * iel, CollisionWorld* collisionWorld) {
  unsigned int numLineLineCollisions = 0;

  // (Re) sort the lines
  sortLines(collisionWorld);

  // Build the bin
  // bin_manager_t * man = new_bin_manager();
  setBins(collisionWorld, man);

  numLineLineCollisions = bin_intersect(man, iel, collisionWorld);

  // free_bin_manager(man);

  IntersectionEventList_sort(iel);

  return numLineLineCollisions;
}

void CollisionWorld_detectIntersection(CollisionWorld* collisionWorld) {
  // Uncomment out lines for live testing
  IntersectionEventList intersectionEventList1 = IntersectionEventList_make();
#ifdef LIVE
  IntersectionEventList intersectionEventList2 = IntersectionEventList_make();
#endif

  CollisionWorld_IntersectionEventList_bin(&intersectionEventList1, collisionWorld);
  unsigned int col1 = intersectionEventList1.size;
#ifdef LIVE
  unsigned int col2 = CollisionWorld_IntersectionEventList_orig(&intersectionEventList2, collisionWorld);

  assert(col1 == col2);
#endif
  
  collisionWorld->numLineLineCollisions += col1;
  // Call the collision solver for each intersection event.
  IntersectionEventNode* curNode = intersectionEventList1.head;
#ifdef LIVE
  curNode = intersectionEventList2.head;
  IntersectionEventNode* curNode2 = intersectionEventList1.head;
#endif

  while (curNode != NULL) {
#ifdef LIVE
    assert(curNode->l1 == curNode2->l1);
    assert(curNode->l2 == curNode2->l2);
    assert(curNode->intersectionType == curNode2->intersectionType);
#endif
    CollisionWorld_collisionSolver(collisionWorld, curNode->l1, curNode->l2,
                                   curNode->intersectionType);
    curNode = curNode->next;
#ifdef LIVE
    curNode2 = curNode2->next;
#endif
  }

  IntersectionEventList_deleteNodes(&intersectionEventList1);
#ifdef LIVE
  IntersectionEventList_deleteNodes(&intersectionEventList2);
#endif
}

unsigned int CollisionWorld_getNumLineWallCollisions(
    CollisionWorld* collisionWorld) {
  return collisionWorld->numLineWallCollisions;
}

unsigned int CollisionWorld_getNumLineLineCollisions(
    CollisionWorld* collisionWorld) {
  return collisionWorld->numLineLineCollisions;
}

void CollisionWorld_collisionSolver(CollisionWorld* collisionWorld,
                                    Line *l1, Line *l2,
                                    IntersectionType intersectionType) {
  assert(compareLines(l1, l2) < 0);
  assert(intersectionType == L1_WITH_L2
         || intersectionType == L2_WITH_L1
         || intersectionType == ALREADY_INTERSECTED);

  // Despite our efforts to determine whether lines will intersect ahead
  // of time (and to modify their velocities appropriately), our
  // simplified model can sometimes cause lines to intersect.  In such a
  // case, we compute velocities so that the two lines can get unstuck in
  // the fastest possible way, while still conserving momentum and kinetic
  // energy.
  if (intersectionType == ALREADY_INTERSECTED) {
    Vec p = getIntersectionPoint(l1->p1, l1->p2, l2->p1, l2->p2);
    Vec l_p1_p = Vec_subtract(l1->p1, p);
    Vec l_p2_p = Vec_subtract(l1->p2, p);
    
    // Simplified control flow; replaced ifs with ?:s
    
    // bool lcomp = (Vec_length(l_p1_p) < Vec_length(l_p2_p));
    bool lcomp = (l_p1_p.x * l_p1_p.x + l_p1_p.y * l_p1_p.y < l_p2_p.x * l_p2_p.x + l_p2_p.y * l_p2_p.y);
    
    l1->velocity = Vec_multiply(Vec_normalize(lcomp ? l_p2_p : l_p1_p), Vec_length(l1->velocity));
    
    l_p1_p = Vec_subtract(l2->p1, p);
    l_p2_p = Vec_subtract(l2->p2, p);
    
    lcomp = (l_p1_p.x * l_p1_p.x + l_p1_p.y * l_p1_p.y < l_p2_p.x * l_p2_p.x + l_p2_p.y * l_p2_p.y);
    
    l2->velocity = Vec_multiply(Vec_normalize(lcomp ? l_p2_p : l_p1_p), Vec_length(l2->velocity));
    
    return;
  }

  // Simplified control flow; replaced ifs with ?:s
  // Compute the collision face/normal vectors.
  bool itype = (intersectionType == L1_WITH_L2);
  Line * l = itype ? l2 : l1;
  
  Vec v = Vec_makeFromLine(*l);
  Vec face = Vec_divide(v, Vec_length(v));
  Vec normal = Vec_orthogonal(face);

  // Obtain each line's velocity components with respect to the collision
  // face/normal vectors.
  double v1Face = Vec_dotProduct(l1->velocity, face);
  double v2Face = Vec_dotProduct(l2->velocity, face);
  double v1Normal = Vec_dotProduct(l1->velocity, normal);
  double v2Normal = Vec_dotProduct(l2->velocity, normal);

  // Compute the mass of each line (we simply use its length).
  double m1 = Vec_length(Vec_makeFromLine(*l1));
  double m2 = Vec_length(Vec_makeFromLine(*l2));

  // Perform the collision calculation (computes the new velocities along
  // the direction normal to the collision face such that momentum and
  // kinetic energy are conserved).
  double newV1Normal = ((m1 - m2) / (m1 + m2)) * v1Normal
      + (2 * m2 / (m1 + m2)) * v2Normal;
  double newV2Normal = (2 * m1 / (m1 + m2)) * v1Normal
      + ((m2 - m1) / (m2 + m1)) * v2Normal;

  // Combine the resulting velocities.
  l1->velocity = Vec_add(Vec_multiply(normal, newV1Normal),
                         Vec_multiply(face, v1Face));
  l2->velocity = Vec_add(Vec_multiply(normal, newV2Normal),
                         Vec_multiply(face, v2Face));

  return;
}
