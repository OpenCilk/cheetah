#ifndef _WORKER_COORD_H
#define _WORKER_COORD_H

#include "rts-config.h"

// Routines for coordinating workers, specifically, putting workers to sleep and
// waking workers when execution enters and leaves cilkified regions.

//=========================================================
// Common internal interface for managing execution of workers.
//=========================================================

__attribute__((always_inline)) static inline void busy_loop_pause() {
#ifdef __SSE__
    __builtin_ia32_pause();
#endif
#ifdef __aarch64__
    __builtin_arm_yield();
#endif
}

__attribute__((always_inline)) static inline void busy_pause(void) {
    for (int i = 0; i < BUSY_PAUSE; ++i)
        busy_loop_pause();
}

#endif /* _WORKER_COORD_H */
