#ifndef _FIBER_HEADER_H
#define _FIBER_HEADER_H

#include <stdio.h>

#include "rts-config.h"

struct __cilkrts_worker;
struct __cilkrts_stack_frame;

// Structure inserted at the top of a fiber, to implement fiber-local storage.
struct fiber_header {
    // Worker currently executing on the fiber.
    struct __cilkrts_worker *worker;
    // Current stack frame executing on the fiber.
    struct __cilkrts_stack_frame *current_stack_frame;

    // NOTE: The current hyper_table can be stored in the fiber header, but we
    // don't currently observe any performance advantage or disadvantage to
    // storing the hyper_table here.

} __attribute__((aligned(CILK_CACHE_LINE)));

#endif // _FIBER_HEADER_H
