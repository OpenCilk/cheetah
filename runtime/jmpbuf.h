#ifndef _JMPBUF_H
#define _JMPBUF_H

#include <setjmp.h>
#include <stddef.h>

#include "debug.h"

#define JMPBUF_SIZE 5
typedef void *jmpbuf[JMPBUF_SIZE];

#define JMPBUF_FP(ctx) (ctx)[0] // frame addr, i.e., %rbp
#define JMPBUF_PC(ctx) (ctx)[1] // PC counter, i.e., %rip
#define JMPBUF_SP(ctx) (ctx)[2] // stack addr, i.e., %rsp

/**
 * @brief Get frame pointer from jump buffer in__cilkrts_stack_frame.
 */
#define FP(SF) JMPBUF_FP((SF)->ctx)

/**
 * @brief Get program counter from jump buffer in__cilkrts_stack_frame.
 */
#define PC(SF) JMPBUF_PC((SF)->ctx)

/**
 * @brief Get stack pointer from jump buffer in__cilkrts_stack_frame.
 */
#define SP(SF) JMPBUF_SP((SF)->ctx)
// typedef void *__CILK_JUMP_BUFFER[8];

/* These macros are only for debugging. */
#if defined __i386__
#define ASM_GET_SP(osp) asm volatile("movl %%esp, %0" : "=r"(osp))
#elif defined __x86_64__
#define ASM_GET_SP(osp) asm volatile("movq %%rsp, %0" : "=r"(osp))
#else
#define ASM_GET_SP(osp) (osp) = 0
#endif

#define ASM_GET_FP(ofp) (ofp) = __builtin_frame_address(0)

#define DUMP_STACK(lvl, w)                                                     \
    {                                                                          \
        char *x_bp;                                                            \
        char *x_sp;                                                            \
        ASM_GET_FP(x_bp);                                                      \
        ASM_GET_SP(x_sp);                                                      \
        cilkrts_alert((lvl), (w), "rbp %p rsp %p", x_bp, x_sp);                \
    }
#endif
