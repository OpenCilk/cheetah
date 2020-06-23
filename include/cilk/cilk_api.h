#ifndef _CILK_API_H
#define _CILK_API_H
#ifdef __cplusplus
extern "C" {
#endif

extern int __cilkrts_atinit(void (*callback)(void));
extern int __cilkrts_atexit(void (*callback)(void));
extern unsigned __cilkrts_get_nworkers(void);
extern unsigned __cilkrts_get_worker_number(void) __attribute__((deprecated));
struct __cilkrts_worker *__cilkrts_get_tls_worker(void);

#undef VISIBILITY

#ifdef __cplusplus
}
#endif

#endif /* _CILK_API_H */
