#ifndef _CILK_API_H
#define _CILK_API_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
#define __CILKRTS_NOTHROW noexcept
extern "C" {
#else
#define __CILKRTS_NOTHROW
#endif

int __cilkrts_is_initialized(void);
int __cilkrts_atinit(void (*callback)(void));
int __cilkrts_atexit(void (*callback)(void));
unsigned __cilkrts_get_nworkers(void);
unsigned __cilkrts_get_worker_number(void) __attribute__((deprecated));
int __cilkrts_running_on_workers(void);

#include <inttypes.h>
typedef struct __cilkrts_pedigree {
    uint64_t rank;
    struct __cilkrts_pedigree *parent;
} __cilkrts_pedigree;
__cilkrts_pedigree __cilkrts_get_pedigree(void);
void __cilkrts_bump_worker_rank(void) __CILKRTS_NOTHROW;
void __cilkrts_dprand_set_seed(uint64_t seed);
void __cilkrts_init_dprng(void);
uint64_t __cilkrts_get_dprand(void);

typedef void (*__cilk_identity_fn)(void *);
typedef void (*__cilk_reduce_fn)(void *, void *);

/* void *__cilkrts_reducer_lookup(void *key); */

/* void *__cilkrts_reducer_lookup(void *key, size_t size, __cilk_identity_fn id, */
/*                                __cilk_reduce_fn reduce); */
void *__cilkrts_reducer_lookup(void *key, size_t size, void *id, void *reduce);
void __cilkrts_reducer_register(void *key, size_t size, __cilk_identity_fn id,
                                __cilk_reduce_fn reduce)
    __attribute__((deprecated));
void __cilkrts_reducer_unregister(void *key) __attribute__((deprecated));

#ifdef __cplusplus
}
#endif

#endif /* _CILK_API_H */
