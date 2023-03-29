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
} __attribute__((aligned(CILK_CACHE_LINE)));

// Constant method to get the stack size.
__attribute__((const)) static inline size_t get_stack_size(void) {
    return 1 << LG_STACK_SIZE;
}

// Get the fiber header from the given SP.
static inline struct fiber_header *get_fiber_header(const char *sp) {
    const size_t stack_size = get_stack_size();
    // Because fibers are aligned to their power-of-2 size, we can get the fiber
    // header using some simple pointer arithmetic based on the current value of
    // SP.
    return (struct fiber_header *)(((uintptr_t)sp & -stack_size) +
                                   (stack_size -
                                    sizeof(struct fiber_header)));
}

// Convenience method to get the current worker out of fiber-local storage,
// given a stack pointer pointing within that fiber.
static inline struct __cilkrts_worker *get_worker(const char *sp) {
    return get_fiber_header(sp)->worker;
}

#endif // _FIBER_HEADER_H
