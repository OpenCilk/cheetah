#ifndef _FIBER_HEADER_H
#define _FIBER_HEADER_H

#include "rts-config.h"

struct __cilkrts_worker;
struct __cilkrts_stack_frame;

// Structure inserted at the top of a fiber, to implement fiber-local storage.
// The stack begins just below this structure.  See sysdep_get_stack_start().
struct cilk_fiber {
    // Worker currently executing on the fiber.
    struct __cilkrts_worker *worker;
    // Current stack frame executing on the fiber.
    struct __cilkrts_stack_frame *current_stack_frame;

    // NOTE: The current hyper_table can be stored in the fiber header, but we
    // don't currently observe any performance advantage or disadvantage to
    // storing the hyper_table here.

    // Pointer to AddressSanitizer's fake stack associated with this fiber, when
    // AddressSanitizer is being used.
    void *fake_stack_save;

    // These next two words are for internal library use and are
    // constant for the life of this structure.
    char *alloc_low;         // lowest byte of mapped region
    char *stack_low;         // lowest byte of stack region

    // Three unused words remain 64 bit systems with 64 byte cache lines.

} __attribute__((aligned(CILK_CACHE_LINE)));

#endif // _FIBER_HEADER_H
