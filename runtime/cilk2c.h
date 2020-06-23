#ifndef _CILK2C_H
#define _CILK2C_H

#include "cilk-internal.h"
#include <stdlib.h>

// mainly used by invoke-main.c
CHEETAH_INTERNAL unsigned long cilkrts_zero;

// These functions are mostly inlined by the compiler, except for
// __cilkrts_leave_frame.  However, their implementations are also
// provided in cilk2c.c.  The implementations in cilk2c.c are used
// by invoke-main.c and can be used to "hand compile" cilk code.
CHEETAH_API void __cilkrts_enter_frame(__cilkrts_stack_frame *sf);
CHEETAH_API void __cilkrts_enter_frame_fast(__cilkrts_stack_frame *sf);
CHEETAH_API void __cilkrts_save_fp_ctrl_state(__cilkrts_stack_frame *sf);
CHEETAH_API void __cilkrts_detach(__cilkrts_stack_frame *self);
CHEETAH_API void __cilkrts_check_exception_raise(__cilkrts_stack_frame *sf);
CHEETAH_API void __cilkrts_check_exception_resume(__cilkrts_stack_frame *sf);
CHEETAH_API void __cilkrts_cleanup_fiber(__cilkrts_stack_frame *, int32_t sel);
CHEETAH_API void __cilkrts_sync(__cilkrts_stack_frame *sf);
CHEETAH_API void __cilkrts_pop_frame(__cilkrts_stack_frame *sf);
CHEETAH_API void __cilkrts_pause_frame(__cilkrts_stack_frame *sf, char *exn);
CHEETAH_API void __cilkrts_leave_frame(__cilkrts_stack_frame *sf);
CHEETAH_API unsigned __cilkrts_get_nworkers(void);
#endif
