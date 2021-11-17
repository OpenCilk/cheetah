#ifndef _CILK_API_H
#define _CILK_API_H

#include <stddef.h> /* size_t */

#ifdef __cplusplus
extern "C" {
#endif

extern int __cilkrts_is_initialized(void);
extern int __cilkrts_atinit(void (*callback)(void));
extern int __cilkrts_atexit(void (*callback)(void));
extern unsigned __cilkrts_get_nworkers(void);
extern unsigned __cilkrts_get_worker_number(void) __attribute__((deprecated));
extern int __cilkrts_running_on_workers(void);

#include <inttypes.h>
typedef struct __cilkrts_pedigree {
    uint64_t rank;
    struct __cilkrts_pedigree *parent;
} __cilkrts_pedigree;
extern __cilkrts_pedigree __cilkrts_get_pedigree(void);
extern void __cilkrts_bump_worker_rank(void);
extern void __cilkrts_dprand_set_seed(uint64_t seed);
extern void __cilkrts_init_dprng(void);
extern uint64_t __cilkrts_get_dprand(void);

typedef void (*__cilk_identity_fn)(void *);
typedef void (*__cilk_reduce_fn)(void *, void *);
typedef void (*__cilk_destroy_fn)(void *);

extern void *__cilkrts_reducer_lookup(void *key);
extern void __cilkrts_reducer_register(void *key, size_t size,
                                       __cilk_identity_fn id,
                                       __cilk_reduce_fn reduce,
                                       __cilk_destroy_fn destroy)
  __attribute__((deprecated));
extern void __cilkrts_reducer_unregister(void *key)
  __attribute__((deprecated));

#ifdef __cplusplus
}
#endif

#endif /* _CILK_API_H */
