#include "./bin.h"
#include <assert.h>
#include "./reducer_defs.h"

// Compare two cells
// Do I really need this
int compCell(cell_t c1, cell_t c2) {
  int pl = (c1 < c2);
  int mi = (c1 > c2);
  return pl - mi;

  // if (c1 == c2) return 0;
  // if (c1 > c2) return -1;
  // if (c1 < c2) return 1;
}

// Compare two bins
int compBin(bin_t b1, bin_t b2) {
  int ret = compCell(b1.xcell, b2.xcell);
  ret = (ret == 0) ? compCell(b1.ycell, b2.ycell) : ret;
  return ret;
}

// Get l's bin
bin_t getBin(Line * l, CollisionWorld * cw) {
  bin_t ret;

  // Get the displacement parallelogram
  Vec p1 = l->p1;
  Vec p2 = l->p2;
  Vec p3 = Vec_add(p1, Vec_multiply(l->velocity, cw->timeStep));
  Vec p4 = Vec_add(p2, Vec_multiply(l->velocity, cw->timeStep));

  // Get lower-left corner of AABB
  double xmin = p1.x;
  double ymin = p1.y;
  xmin = (p2.x < xmin) ? p2.x : xmin;
  ymin = (p2.y < ymin) ? p2.y : ymin;
  xmin = (p3.x < xmin) ? p3.x : xmin;
  ymin = (p3.y < ymin) ? p3.y : ymin;
  xmin = (p4.x < xmin) ? p4.x : xmin;
  ymin = (p4.y < ymin) ? p4.y : ymin;

  xmin = 2 * xmin - 1;
  ymin = 2 * ymin - 1;

  // Convert to bin
  ret.xcell = (cell_t) (xmin * X_CELLS);
  ret.xcell = (-1 > ret.xcell) ? -1 : ret.xcell;
  ret.xcell = (ret.xcell > X_CELLS) ? X_CELLS : ret.xcell;

  ret.ycell = (cell_t) (ymin * Y_CELLS);
  ret.ycell = (-1 > ret.ycell) ? -1 : ret.ycell;
  ret.ycell = (ret.ycell > Y_CELLS) ? Y_CELLS : ret.ycell;

  return ret;
}

// Get bin version of l's AABB
bin_range_t getRange(Line * l, CollisionWorld * cw) {
  bin_range_t ret;

  // Get the displacement parallelogram
  Vec p1 = l->p1;
  Vec p2 = l->p2;
  Vec p3 = Vec_add(p1, Vec_multiply(l->velocity, cw->timeStep));
  Vec p4 = Vec_add(p2, Vec_multiply(l->velocity, cw->timeStep));

  // Get AABB
  double xmin = p1.x;
  double ymin = p1.y;
  double xmax = p1.x;
  double ymax = p1.y;

  xmin = (p2.x < xmin) ? p2.x : xmin;
  ymin = (p2.y < ymin) ? p2.y : ymin;
  xmax = (p2.x > xmax) ? p2.x : xmax;
  ymax = (p2.y > ymax) ? p2.y : ymax;

  xmin = (p3.x < xmin) ? p3.x : xmin;
  ymin = (p3.y < ymin) ? p3.y : ymin;
  xmax = (p3.x > xmax) ? p3.x : xmax;
  ymax = (p3.y > ymax) ? p3.y : ymax;

  xmin = (p4.x < xmin) ? p4.x : xmin;
  ymin = (p4.y < ymin) ? p4.y : ymin;
  xmax = (p4.x > xmax) ? p4.x : xmax;
  ymax = (p4.y > ymax) ? p4.y : ymax;

  xmin = 2 * xmin - 1;
  ymin = 2 * ymin - 1;
  xmax = 2 * xmax - 1;
  ymax = 2 * ymax - 1;

  // Convert to bin
  cell_t xcell_min = (cell_t) (xmin * X_CELLS);
  xcell_min = (-1 > xcell_min) ? -1 : xcell_min;
  xcell_min = (xcell_min > X_CELLS) ? X_CELLS : xcell_min;

  cell_t ycell_min = (cell_t) (ymin * Y_CELLS);
  ycell_min = (-1 > ycell_min) ? -1 : ycell_min;
  ycell_min = (ycell_min > Y_CELLS) ? Y_CELLS : ycell_min;

  cell_t xcell_max = (cell_t) (xmax * X_CELLS);
  xcell_max = (-1 > xcell_max) ? -1 : xcell_max;
  xcell_max = (xcell_max > X_CELLS) ? X_CELLS : xcell_max;

  cell_t ycell_max = (cell_t) (ymax * Y_CELLS);
  ycell_max = (-1 > ycell_max) ? -1 : ycell_max;
  ycell_max = (ycell_max > Y_CELLS) ? Y_CELLS : ycell_max;

  ret.min.xcell = xcell_min;
  ret.min.ycell = ycell_min;
  ret.max.xcell = xcell_max;
  ret.max.ycell = ycell_max;

  return ret;
}

// Standard new
bin_manager_t * new_bin_manager() {
  bin_manager_t * ret = (bin_manager_t *) malloc(sizeof(bin_manager_t));

  int size = (X_CELLS + 2) * (Y_CELLS + 2) ;
  bin_loc_t * bins = (bin_loc_t *) malloc(sizeof(bin_loc_t) * size);

  ret->bins = bins;
  ret->size = size;

  return ret;
}

// Standard free
void free_bin_manager(bin_manager_t * man) {
  free(man->bins);
  free(man);
}

// Convert cell's to an index
int getBin_ind(cell_t x, cell_t y) {
  x = (-1 > x) ? -1 : x;
  x = (x > X_CELLS) ? X_CELLS : x;
  y = (-1 > y) ? -1 : y;
  y = (y > Y_CELLS) ? Y_CELLS : y;
  int ind = (x + 1) * (Y_CELLS + 2) + y + 1;
  return ind;
}

// Do I even use this?
unsigned int getBin_start(cell_t x, cell_t y, bin_manager_t * man) {
  x = (-1 > x) ? -1 : x;
  x = (x > X_CELLS) ? X_CELLS : x;
  y = (-1 > y) ? -1 : y;
  y = (y > Y_CELLS) ? Y_CELLS : y;
  int ind = (x + 1) * (Y_CELLS + 2) + y + 1;
  return man->bins[ind].start;
}

// Do I even use this?
unsigned int getBin_end(cell_t x, cell_t y, bin_manager_t * man) {
  x = (-1 > x) ? -1 : x;
  x = (x > X_CELLS) ? X_CELLS : x;
  y = (-1 > y) ? -1 : y;
  y = (y > Y_CELLS) ? Y_CELLS : y;
  int ind = (x + 1) * (Y_CELLS + 2) + y + 1;
  return man->bins[ind].end;
}

// Initialize man with data from cw->lines
// Only works if cw->lines is sorted
void setBins(CollisionWorld * cw, bin_manager_t * man) {
  unsigned int n = cw->numOfLines;
  int ind = 0;
  int oldind = 0;

  // Starting data for first bin
  man->bins[0].start = 0;
  man->bins[0].top = -2;
  man->bins[0].right = -2;

  // Iterate over lines
  for (unsigned int i = 0; i < n; i++) {
    // Get AABB
    bin_range_t bin = getRange(cw->lines[i], cw);
    ind = getBin_ind(bin.min.xcell, bin.min.ycell);

    // We moved to a new bin
    if (ind > oldind) {
      man->bins[oldind].end = i;

      // Populate intermediary bins
      for (int j = oldind + 1; j < ind; j++) {
        man->bins[j].start = i;
        man->bins[j].end = i;
        man->bins[j].top = -2;
        man->bins[j].right = -2;
      }

      // Start news bins
      man->bins[ind].start = i;
      man->bins[ind].top = -2;
      man->bins[ind].right = -2;
      oldind = ind;
    }

    // Update range
    man->bins[ind].top = (bin.max.ycell > man->bins[ind].top) ? bin.max.ycell : man->bins[ind].top;
    man->bins[ind].right = (bin.max.xcell > man->bins[ind].right) ? bin.max.xcell : man->bins[ind].right;
  }

  // Fill trailing bins
  man->bins[ind].end = n;
  for (int j = ind + 1; j < man->size; j++) {
    man->bins[j].start = n;
    man->bins[j].end = n;
    man->bins[j].top = -2;
    man->bins[j].right = -2;
  }
}

// Insertion sort on cw->lines
// Assuming velocities are low, amortized very fast
void sortLines(CollisionWorld * cw) {
  unsigned int n = cw->numOfLines;

  // Need i, j signed, so make them ssize_t
  for (ssize_t i = 0; i < n; i++) {
    // cw->lines[i]->id = i;
    bin_t bin = getBin(cw->lines[i], cw);
    int ind = getBin_ind(bin.xcell, bin.ycell);
    cw->lines[i]->ind = ind;
    for (ssize_t j = i - 1; j >= 0; j--) {
      // bin_t binj = getBin(cw->lines[j], cw);
      if (cw->lines[j]->ind > ind) {  // (compBin(binj, bin) < 0) {
        Line * temp = cw->lines[j + 1];
        cw->lines[j + 1] = cw->lines[j];
        cw->lines[j] = temp;
        // cw->lines[j + 1]->id++;
        // cw->lines[j]->id--;
      } else {
        break;
      }
    }
  }
}

// Find lines in man->bins[ind1:ind2] that cw->lines[i] collides with
// Only checks lines after cw->lines[i]
unsigned int intersect_range(unsigned int i, int ind1, int ind2, bin_manager_t * man, IntersectionEventList * iel, CollisionWorld * cw) {

  unsigned int start = man->bins[ind1].start;
  unsigned int end = man->bins[ind2].end;

  for (unsigned int j = (start > i) ? start : i + 1; j < end; j++) {
    Line * l1 = cw->lines[i];
    Line * l2 = cw->lines[j];

    // intersect expects compareLines(l1, l2) < 0 to be true.
    // Swap l1 and l2, if necessary.
    if (compareLines(l1, l2) >= 0) {
      Line *temp = l1;
      l1 = l2;
      l2 = temp;
    }
    uint8_t res = intersectFast(l1, l2, cw->timeStep);
    if (res) {
      IntersectionType intersectionType = intersectDecode(res, l1, l2, cw->timeStep);
      // IntersectionType intersectionType2 = intersect(l1, l2, cw->timeStep);
      // assert(intersectionType == intersectionType2);
      IntersectionEventList_appendNode(iel, l1, l2, intersectionType);
    }
  }

  return 0;
}

// Reducer version
void intersect_range_red(unsigned int i, int ind1, int ind2, bin_manager_t * man, IELReducer * red, CollisionWorld * cw) {

  unsigned int start = man->bins[ind1].start;
  start = (start > i) ? start : i + 1;
  unsigned int end = man->bins[ind2].end;

  if (end - start < 32) {
    IntersectionEventList * iel = &REDVAL(*red);
    for (unsigned int j = start; j < end; j++) {
      Line * l1 = cw->lines[i];
      Line * l2 = cw->lines[j];

      // intersect expects compareLines(l1, l2) < 0 to be true.
      // Swap l1 and l2, if necessary.
      if (compareLines(l1, l2) >= 0) {
        Line *temp = l1;
        l1 = l2;
        l2 = temp;
      }
      uint8_t res = intersectFast(l1, l2, cw->timeStep);
      if (res) {
        IntersectionType intersectionType = intersectDecode(res, l1, l2, cw->timeStep);
        // IntersectionType intersectionType2 = intersect(l1, l2, cw->timeStep);
        // assert(intersectionType == intersectionType2);
        IntersectionEventList_appendNode(iel, l1, l2, intersectionType);
      }
    }
  } else {
    cilk_for (unsigned int j = start; j < end; j++) {
      Line * l1 = cw->lines[i];
      Line * l2 = cw->lines[j];

      // intersect expects compareLines(l1, l2) < 0 to be true.
      // Swap l1 and l2, if necessary.
      if (compareLines(l1, l2) >= 0) {
        Line *temp = l1;
        l1 = l2;
        l2 = temp;
      }
      uint8_t res = intersectFast(l1, l2, cw->timeStep);
      if (res) {
        IntersectionType intersectionType = intersectDecode(res, l1, l2, cw->timeStep);
        // IntersectionType intersectionType2 = intersect(l1, l2, cw->timeStep);
        // assert(intersectionType == intersectionType2);
        IntersectionEventList_appendNode(&REDVAL(*red), l1, l2, intersectionType);
      }
    }
  }
}

// Find lines in man->bins[ind] that hit the wall
unsigned int wallCollision_range(int ind, bin_manager_t * man, CollisionWorld * cw) {
  unsigned int numLineWallCollisions = 0;

  unsigned int start = man->bins[ind].start;
  unsigned int end = man->bins[ind].end;
  for (unsigned int j = start; j < end; j++) {
    Line *line = cw->lines[j];
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
    // Update total number of collisions.
    if (collide == true) {
      numLineWallCollisions++;
    }
  }

  return numLineWallCollisions;
}

// Get colliding lines
unsigned int bin_intersect(bin_manager_t * man, IntersectionEventList * iel, CollisionWorld * cw) {
  unsigned int n = cw->numOfLines;

  IELReducer red = IEL_RED;
  REGRED(red);

  // Iterate through all lines
  cilk_for (unsigned int i = 0; i < n; i++) {
    Line * l1 = cw->lines[i];
    bin_range_t r = getRange(l1, cw);

    // Test bins [r.min.xcell, r.min.ycell:r.max.ycell]
    int ind1 = (r.min.xcell + 1) * (Y_CELLS + 2) + r.min.ycell + 1;
    int ind2 = (r.min.xcell + 1) * (Y_CELLS + 2) + r.max.ycell + 1;
    intersect_range_red(i, ind1, ind2, man, &red, cw);

    for (cell_t x = r.min.xcell + 1; x <= r.max.xcell; x++) {
      // Test bins [x, -1:r.min.ycell]
      for (cell_t y = -1; y < r.min.ycell; y++) {
        int ind = (x + 1) * (Y_CELLS + 2) + y + 1;
        // only test if bin's potential rang intersects r
        if (man->bins[ind].top >= r.min.ycell) intersect_range_red(i, ind, ind, man, &red, cw);
      }

      // Test bins [x, r.min.ycell:r.max.ycell]
      ind1 = (x + 1) * (Y_CELLS + 2) + r.min.ycell + 1;
      ind2 = (x + 1) * (Y_CELLS + 2) + r.max.ycell + 1;
      intersect_range_red(i, ind1, ind2, man, &red, cw);
    }

  }
  // numLineLineCollisions += REDVAL(red).size;
  IEL_merge(iel, &REDVAL(red));
  KILLRED(red);

  return 0;
}

// Resolve Wall collisions
unsigned int bin_wallCollision(bin_manager_t * man, CollisionWorld * cw) {
  unsigned int numLineWallCollisions = 0;

  // Iterate through all bins
  for (cell_t x = -1; x <= X_CELLS; x++) {
    for (cell_t y = -1; y <= Y_CELLS; y++) {
      int ind = (x + 1) * (Y_CELLS + 2) + y + 1;
      // only test if bin's potential range intersects border
      if (x == -1 || x == X_CELLS || y == -1 || y == Y_CELLS || man->bins[ind].top == Y_CELLS || man->bins[ind].right == X_CELLS) {
        numLineWallCollisions += wallCollision_range(ind, man, cw);
      }
    }
  }

  return numLineWallCollisions;
}

// Detect IF lines l1 and l2 will intersect between now and the next time step
// Encodes additional info to aid intersectDecode
uint8_t intersectFast(Line *l1, Line *l2, double time) {
  assert(compareLines(l1, l2) < 0);

  Vec disp;
  Vec p1;
  Vec p2;

  // Get relative velocity.
  disp = Vec_multiply(Vec_subtract(l2->velocity, l1->velocity), time);

  // Get the parallelogram.
  p1 = Vec_add(l2->p1, disp);
  p2 = Vec_add(l2->p2, disp);

  uint8_t res = 0;

  // Encode additional info
  res = (intersectLines(l1->p1, l1->p2, l2->p1, l2->p2)) ? 32 : 0;
  res += (intersectLines(l1->p1, l1->p2, p1, p2)) ? 1 : 0;
  res += (intersectLines(l1->p1, l1->p2, p1, l2->p1)) ? 9 : 0;
  res += (intersectLines(l1->p1, l1->p2, p2, l2->p2)) ? 5 : 0;
  if ((res >> 5) == 0 && (res & 3) != 2) {
    res += (pointInParallelogram(l1->p1, l2->p1, l2->p2, p1, p2) && pointInParallelogram(l1->p2, l2->p1, l2->p2, p1, p2)) ? 16 : 0;
  }
  return res;
}


// Detect HOW lines l1 and l2 will intersect between now and the next time step
// Uses info in res
IntersectionType intersectDecode(uint8_t res, Line *l1, Line *l2, double time) {
  assert(compareLines(l1, l2) < 0);

  Vec disp;
  Vec p1;
  Vec p2;

  // Get relative velocity.
  disp = Vec_multiply(Vec_subtract(l2->velocity, l1->velocity), time);

  // Get the parallelogram.
  p1 = Vec_add(l2->p1, disp);
  p2 = Vec_add(l2->p2, disp);

  int num_line_intersections = res & 3;
  bool bottom_intersected = (res >> 2) & 1;
  bool top_intersected = (res >> 3) & 1;

  if (res >> 5) {
    return ALREADY_INTERSECTED;
  }

  if (num_line_intersections == 2) {
    return L2_WITH_L1;
  }

  if ((res >> 4) & 1) {
    return L1_WITH_L2;
  }

  if (num_line_intersections == 0) {
    return NO_INTERSECTION;
  }

  Vec v1 = Vec_makeFromLine(*l1);
  Vec v2 = Vec_makeFromLine(*l2);
  double angle = Vec_angle(v1, v2);

  if (top_intersected) {
    if (angle < 0) {
      return L2_WITH_L1;
    } else {
      return L1_WITH_L2;
    }
  }

  if (bottom_intersected) {
    if (angle > 0) {
      return L2_WITH_L1;
    } else {
      return L1_WITH_L2;
    }
  }

  return L1_WITH_L2;
}

