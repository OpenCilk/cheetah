#ifndef _CILK_INIT_H
#define _CILK_INIT_H

#include "cilk-internal.h"

int cilk_main(int argc, char *argv[]);
CHEETAH_INTERNAL global_state *__cilkrts_init(int argc, char *argv[]);
CHEETAH_INTERNAL void __cilkrts_cleanup(global_state *);
CHEETAH_INTERNAL_NORETURN void invoke_main();

#endif /* _CILK_INIT_H */
