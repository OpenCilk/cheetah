#ifndef _CILK_API_H
#define _CILK_API_H
#ifdef __cplusplus
extern "C" {
#endif

extern int __cilkrts_is_initialized(void);
extern int __cilkrts_atinit(void (*callback)(void));
extern int __cilkrts_atexit(void (*callback)(void));
extern unsigned __cilkrts_get_nworkers(void);
extern unsigned __cilkrts_get_worker_number(void) __attribute__((deprecated));
extern int __cilkrts_running_on_workers(void);

#if defined(__cilk_pedigrees__) || defined(ENABLE_CILKRTS_PEDIGREE)
#include <inttypes.h>
typedef struct __cilkrts_pedigree {
    uint64_t rank;
    struct __cilkrts_pedigree *parent;
} __cilkrts_pedigree;
extern __cilkrts_pedigree __cilkrts_get_pedigree(void);
extern void __cilkrts_bump_worker_rank(void);
extern uint64_t __cilkrts_get_dprand(void);
#endif // defined(__cilk_pedigrees__) || defined(ENABLE_CILKRTS_PEDIGREE)

#undef VISIBILITY

#ifdef __cplusplus
}
#endif

#endif /* _CILK_API_H */
