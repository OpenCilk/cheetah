#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define ENABLE_CILKRTS_PEDIGREE
#include <cilk/cilk_api.h> 

__cilkrts_pedigree cilkrts_root_pedigree_node;
uint64_t DPRNG_PRIME = (uint64_t)(-59);
uint64_t* dprng_m_array;
uint64_t dprng_m_X = 0;

uint64_t __cilkrts_dprng_swap_halves(uint64_t x) {
  return (x >> (4 * sizeof(uint64_t))) | (x << (4 * sizeof(uint64_t)));
}

uint64_t __cilkrts_dprng_mix(uint64_t x) {
  for (int i = 0; i < 4; i++) {
      x = x * (2*x+1);
      x = __cilkrts_dprng_swap_halves(x);
  } 
  return x;
}

uint64_t __cilkrts_dprng_mix_mod_p(uint64_t x) {
  x = __cilkrts_dprng_mix(x);
  return x -  (DPRNG_PRIME & -(x >= DPRNG_PRIME));
}

uint64_t __cilkrts_dprng_sum_mod_p(uint64_t a, uint64_t b) {
  uint64_t z = a+b;
  if ((z < a) || (z >= DPRNG_PRIME)) {
      z -= DPRNG_PRIME;
  }
  return z;
}

void __cilkrts_init_dprng(void) {
    dprng_m_array = (uint64_t*) malloc(sizeof(uint64_t*) * 4096);
    for (int i = 0; i < 4096; i++) {
      dprng_m_array[i] = __cilkrts_dprng_mix_mod_p(0x8c679c168e6bf733ul + i);
    }
    dprng_m_X = __cilkrts_dprng_mix_mod_p(0x8c679c168e6bf733ul + 4096);
}

