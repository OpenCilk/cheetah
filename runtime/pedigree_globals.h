#include <stdint.h>
#if ENABLE_CILKRTS_PEDIGREE
#include <cilk/cilk_api.h> 

extern __cilkrts_pedigree __cilkrts_root_pedigree_node;
extern uint64_t __cilkrts_dprng_m_X;
uint64_t __cilkrts_dprng_mix_mod_p(uint64_t x);
uint64_t __cilkrts_dprng_sum_index_mod_p(uint64_t a, unsigned index);
void __cilkrts_init_dprng(void);
#endif
