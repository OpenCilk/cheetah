#ifndef BIN_H_
#define BIN_H_


#include <stdint.h>
#include <stdlib.h>

#include "./vec.h"
#include "./line.h"
#include "./collision_world.h"
#include "./intersection_event_list.h"
#include "./intersection_detection.h"


#ifndef X_CELLS
#define X_CELLS 64
#endif

#ifndef Y_CELLS
#define Y_CELLS 64
#endif

// Type for bin coordinates
typedef int16_t cell_t;

// Bin coordinates
typedef struct {
  cell_t xcell;
  cell_t ycell;
} bin_t;

// Bin AABB
typedef struct {
  bin_t min;
  bin_t max;
} bin_range_t;

// Data for each bin
typedef struct {
  unsigned int start;
  unsigned int end;
  cell_t top;
  cell_t right;
} bin_loc_t;

// Maps bin info onto CollisionWorld
typedef struct {
  bin_loc_t * bins;
  int size;
} bin_manager_t;

// Compare two cells
// Do I really need this
int compCell(cell_t c1, cell_t c2);

// Compare two bins
int compBin(bin_t b1, bin_t b2);

// Get l's bin
bin_t getBin(Line * l, CollisionWorld * cw);

// Get bin version of l's AABB
bin_range_t getRange(Line * l, CollisionWorld * cw);

// Standard new
bin_manager_t * new_bin_manager();

// Standard free
void free_bin_manager(bin_manager_t * man);

// Convert cell's to an index
int getBin_ind(cell_t x, cell_t y);

// Do I even use this?
unsigned int getBin_start(cell_t x, cell_t y, bin_manager_t * man);

// Do I even use this?
unsigned int getBin_end(cell_t x, cell_t y, bin_manager_t * man);

// Initialize man with data from cw->lines
// Only works if cw->lines is sorted
void setBins(CollisionWorld * cw, bin_manager_t * man);

// Insertion sort on cw->lines
// Assuming velocities are low, amortized very fast
void sortLines(CollisionWorld * cw);

// Find lines in man->bins[ind1:ind2] that cw->lines[i] collides with
// Only checks lines after cw->lines[i]
unsigned int intersect_range(unsigned int i, int ind1, int ind2, bin_manager_t * man, IntersectionEventList * iel, CollisionWorld * cw);

// Find lines in man->bins[ind] that hit the wall
unsigned int wallCollision_range(int ind, bin_manager_t * man, CollisionWorld * cw);

// Get colliding lines
unsigned int bin_intersect(bin_manager_t * man, IntersectionEventList * iel, CollisionWorld * cw);

// Resolve Wall collisions
unsigned int bin_wallCollision(bin_manager_t * man, CollisionWorld * cw);

// Detect IF lines l1 and l2 will intersect between now and the next time step
// Encodes additional info to aid intersectDecode
uint8_t intersectFast(Line *l1, Line *l2, double time);

// Detect HOW lines l1 and l2 will intersect between now and the next time step
// Uses info in res
IntersectionType intersectDecode(uint8_t res, Line *l1, Line *l2, double time);

#endif
