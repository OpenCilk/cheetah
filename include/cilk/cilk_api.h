#ifndef _CILK_API_H
#define _CILK_API_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
#define __CILKRTS_NOTHROW noexcept
extern "C" {
#else
#define __CILKRTS_NOTHROW __attribute__((nothrow))
#endif

int __cilkrts_is_initialized(void) __CILKRTS_NOTHROW;
int __cilkrts_atinit(void (*callback)(void)) __CILKRTS_NOTHROW;
int __cilkrts_atexit(void (*callback)(void)) __CILKRTS_NOTHROW;
unsigned __cilkrts_get_nworkers(void) __CILKRTS_NOTHROW;
unsigned __cilkrts_get_worker_number(void) __attribute__((deprecated));
int __cilkrts_running_on_workers(void) __CILKRTS_NOTHROW;

#include <inttypes.h>
typedef struct __cilkrts_pedigree {
    uint64_t rank;
    struct __cilkrts_pedigree *parent;
} __cilkrts_pedigree;
__cilkrts_pedigree __cilkrts_get_pedigree(void) __CILKRTS_NOTHROW;
void __cilkrts_bump_worker_rank(void) __CILKRTS_NOTHROW;
void __cilkrts_dprand_set_seed(uint64_t seed) __CILKRTS_NOTHROW;
void __cilkrts_init_dprng(void) __CILKRTS_NOTHROW;
uint64_t __cilkrts_get_dprand(void) __CILKRTS_NOTHROW;

void __cilkrts_reducer_register_2(void *key, void (*reduce)(void *, void *))
  __CILKRTS_NOTHROW;

__attribute__((deprecated))
void __cilkrts_reducer_unregister(void *key) __CILKRTS_NOTHROW;

#ifdef __cplusplus
}
#endif

#endif /* _CILK_API_H */
