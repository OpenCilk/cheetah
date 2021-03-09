#ifndef _RSCHED_H
#define _RSCHED_H

#include "cilk-internal.h"
#include "closure.h"

#define SYNC_READY 0
#define SYNC_NOT_READY 1

#define EXCEPTION_INFINITY (__cilkrts_stack_frame **)(-1LL)

CHEETAH_INTERNAL void do_what_it_says_boss(__cilkrts_worker *w, Closure *t);

CHEETAH_INTERNAL void __cilkrts_init_tls_variables();
CHEETAH_INTERNAL void __cilkrts_set_tls_worker(__cilkrts_worker *w);

CHEETAH_INTERNAL int Cilk_sync(__cilkrts_worker *const ws,
                               __cilkrts_stack_frame *frame);

void Cilk_set_return(__cilkrts_worker *const ws);
void Cilk_exception_handler(char *exn);

CHEETAH_INTERNAL_NORETURN void longjmp_to_runtime(__cilkrts_worker *w);
CHEETAH_INTERNAL void worker_scheduler(__cilkrts_worker *ws);
CHEETAH_INTERNAL void *scheduler_thread_proc(void *arg);

CHEETAH_INTERNAL void promote_own_deque(__cilkrts_worker *w);

#endif
