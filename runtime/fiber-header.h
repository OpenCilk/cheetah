#ifndef _FIBER_HEADER_H
#define _FIBER_HEADER_H

#include "rts-config.h"

struct __cilkrts_worker;
struct __cilkrts_stack_frame;

// Structure inserted at the top of a fiber, to implement fiber-local storage.
// The stack begins just below this structure.  See get_stack_start().
// This must be a standard layout class.
struct alignas(CILK_CACHE_LINE) cilk_fiber {
    // Worker currently executing on the fiber.
    __cilkrts_worker *worker;
    // Current stack frame executing on the fiber.
    __cilkrts_stack_frame *current_stack_frame;

    // NOTE: The current hyper_table can be stored in the fiber header, but we
    // don't currently observe any performance advantage or disadvantage to
    // storing the hyper_table here.

    // Pointer to AddressSanitizer's fake stack associated with this fiber, when
    // AddressSanitizer is being used.
    void *fake_stack_save;

    // These next two words are for internal library use and are
    // constant for the life of this structure.
    char *alloc_low; // lowest byte of mapped region
    char *stack_low; // lowest byte of stack region

    // Three unused words remain on 64 bit systems with 64 byte cache lines.

    char *get_fiber_start() { return alloc_low; }
    char *get_fiber_end() { return (char *)(this + 1); }
    char *get_stack_start() { return (char *)this; }

    bool in_fiber(void *addr) {
        void *stack_high = (char *)this;
        // One past the end is considered in the fiber.
        return addr >= stack_low && addr <= stack_high;
    }

    void clear() {
        worker = nullptr;
        current_stack_frame = nullptr;
        fake_stack_save = nullptr;
    }
};

#endif // _FIBER_HEADER_H
