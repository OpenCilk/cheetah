#ifndef _PEDIGREE_INTERNAL_H
#define _PEDIGREE_INTERNAL_H

#include <stdlib.h>
#include <cilk/cilk_api.h>

#include "cilk-internal.h"

static const uint64_t DPRNG_PRIME = (uint64_t)(-59);
extern uint64_t *__pedigree_dprng_m_array;
extern uint64_t __pedigree_dprng_seed;

typedef struct __pedigree_frame {
    __cilkrts_pedigree pedigree; // Fields for pedigrees.
    int64_t rank;
    uint64_t dprng_dotproduct;
    int64_t dprng_depth;
} __pedigree_frame;

typedef struct __pedigree_frame_storage_t {
    size_t next_pedigree_frame;
    __pedigree_frame* frames;
} __pedigree_frame_storage_t;


///////////////////////////////////////////////////////////////////////////
// Helper methods

static inline __attribute__((malloc)) __pedigree_frame *
push_pedigree_frame(__cilkrts_worker *w) {
    return __cilkrts_push_ext_stack(w, sizeof(__pedigree_frame));
}

static inline void pop_pedigree_frame(__cilkrts_worker *w) {
    __cilkrts_pop_ext_stack(w, sizeof(__pedigree_frame));
}

static inline uint64_t __cilkrts_dprng_swap_halves(uint64_t x) {
  return (x >> (4 * sizeof(uint64_t))) | (x << (4 * sizeof(uint64_t)));
}

static inline uint64_t __cilkrts_dprng_mix(uint64_t x) {
  for (int i = 0; i < 4; i++) {
      x = x * (2*x+1);
      x = __cilkrts_dprng_swap_halves(x);
  }
  return x;
}

static inline uint64_t __cilkrts_dprng_mix_mod_p(uint64_t x) {
  x = __cilkrts_dprng_mix(x);
  return x - (DPRNG_PRIME & -(x >= DPRNG_PRIME));
}

static inline uint64_t __cilkrts_dprng_sum_mod_p(uint64_t a, uint64_t b) {
    uint64_t z = a + b;
    if ((z < a) || (z >= DPRNG_PRIME)) {
        z -= DPRNG_PRIME;
    }
    return z;
}

// Helper method to advance the pedigree and dprng states.
static inline __attribute__((always_inline)) __pedigree_frame *
bump_worker_rank(void) {
    __pedigree_frame *frame = (__pedigree_frame *)(__cilkrts_get_extension());
    frame->rank++;
    frame->dprng_dotproduct = __cilkrts_dprng_sum_mod_p(
        frame->dprng_dotproduct, __pedigree_dprng_m_array[frame->dprng_depth]);
    return frame;
}

#endif // _PEDIGREE_INTERNAL_H
