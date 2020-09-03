#ifndef _DEBUG_H
#define _DEBUG_H

#include <stdarg.h>

#include "rts-config.h"

// forward declaration for using struct global_stat
struct global_state;
struct __cilkrts_worker;

#define CILK_CHECK(g, cond, complain, ...)                                     \
    ((cond) ? (void)0 : cilk_die_internal(g, complain, __VA_ARGS__))

#ifndef ALERT_LVL
#define ALERT_LVL 0x3103
#endif
#define ALERT_NONE 0x0
#define ALERT_FIBER 0x001
#define ALERT_FIBER_SUMMARY 0x002
#define ALERT_MEMORY 0x004
#define ALERT_SYNC 0x010
#define ALERT_SCHED 0x020
#define ALERT_STEAL 0x040
#define ALERT_RETURN 0x080
#define ALERT_EXCEPT 0x100
#define ALERT_CFRAME 0x200
#define ALERT_REDUCE 0x400
#define ALERT_REDUCE_ID 0x800
#define ALERT_BOOT 0x1000
#define ALERT_START 0x2000
#define ALERT_CLOSURE 0x4000

extern CHEETAH_INTERNAL unsigned int alert_level;
#define ALERT_ENABLED(flag) (alert_level & (ALERT_LVL & ALERT_##flag))

#ifndef DEBUG_LVL
#define DEBUG_LVL 0xff
#endif

#define DEBUG_MEMORY 0x01
#define DEBUG_MEMORY_SLOW 0x02
#define DEBUG_FIBER 0x04
#define DEBUG_REDUCER 0x08
extern CHEETAH_INTERNAL unsigned int debug_level;
#define DEBUG_ENABLED(flag) (debug_level & (DEBUG_LVL & DEBUG_##flag))
#define DEBUG_ENABLED_STATIC(flag) (DEBUG_LVL & DEBUG_##flag)

// Unused: compiler inlines the stack frame creation
// #define CILK_STACKFRAME_MAGIC 0xCAFEBABE

CHEETAH_INTERNAL void set_alert_level(unsigned int);
CHEETAH_INTERNAL void set_debug_level(unsigned int);
CHEETAH_INTERNAL void flush_alert_log();

__attribute__((__format__(__printf__, 2, 3))) CHEETAH_INTERNAL_NORETURN void
cilkrts_bug(struct __cilkrts_worker *w, const char *fmt, ...);
CHEETAH_INTERNAL_NORETURN
void cilk_die_internal(struct global_state *const g, const char *fmt, ...);

#if CILK_DEBUG
__attribute__((__format__(__printf__, 3, 4))) void
cilkrts_alert(int lvl, struct __cilkrts_worker *w, const char *fmt, ...);
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#define cilkrts_alert(CODE, W, FMT, ...)                                       \
    (alert_level & ((ALERT_##CODE) & ALERT_LVL))                               \
        ? cilkrts_alert(ALERT_##CODE, W, FMT, ##__VA_ARGS__)                   \
        : (void)0

#define WHEN_CILK_DEBUG(ex) ex

/** Standard text for failed assertion */
CHEETAH_INTERNAL extern const char *const __cilkrts_assertion_failed;

#define CILK_ASSERT(w, ex)                                                     \
    (__builtin_expect((ex) != 0, 1)                                            \
         ? (void)0                                                             \
         : cilkrts_bug(w, __cilkrts_assertion_failed, __FILE__, __LINE__,      \
                       #ex))

#define CILK_ASSERT_POINTER_EQUAL(w, P1, P2)                                   \
    ({                                                                         \
        void *_t1 = (P1), *_t2 = (P2);                                         \
        __builtin_expect(_t1 == _t2, 1)                                        \
            ? (void)0                                                          \
            : cilkrts_bug(w,                                                   \
                          "%s: %d: cilk_assertion failed: %s (%p) == %s (%p)", \
                          __FILE__, __LINE__, #P1, _t1, #P2, _t2);             \
    })

#define CILK_ASSERT_ZERO(w, ex, FMT)                                           \
    (__builtin_expect(!(ex), 1)                                                \
         ? (void)0                                                             \
         : cilkrts_bug(w, "%s: %d: cilk_assertion failed: %s (" FMT ") == 0",  \
                       __FILE__, __LINE__, #ex, ex))

#define CILK_ASSERT_INDEX_ZERO(w, LEFT, I, RIGHT, FMT)                         \
    (__builtin_expect(!(LEFT[I] RIGHT), 1)                                     \
         ? (void)0                                                             \
         : cilkrts_bug(w,                                                      \
                       "%s: %d: cilk_assertion failed: %s[%u]%s = " FMT        \
                       " should be 0",                                         \
                       __FILE__, __LINE__, #LEFT, I, #RIGHT, LEFT[I] RIGHT))

#define CILK_ASSERT_G(ex)                                                      \
    (__builtin_expect((ex) != 0, 1)                                            \
         ? (void)0                                                             \
         : cilkrts_bug(NULL, __cilkrts_assertion_failed, __FILE__, __LINE__,   \
                       #ex))

#define CILK_ASSERT_G_LE(A, B, FMT)                                            \
    (__builtin_expect(((A) <= (B)) != 0, 1)                                    \
         ? (void)0                                                             \
         : cilkrts_bug(NULL,                                                   \
                       "%s: %d: cilk assertion failed: %s (" FMT               \
                       ") <= %s " FMT ")",                                     \
                       __FILE__, __LINE__, #A, A, #B, B))

#define CILK_ABORT(w, msg)                                                     \
    cilkrts_bug(w, __cilkrts_assertion_failed, __FILE__, __LINE__, msg)

#define CILK_ABORT_G(msg)                                                      \
    cilkrts_bug(NULL, __cilkrts_assertion_failed_g, __FILE__, __LINE__, msg)

#else
#define cilkrts_alert(lvl, fmt, ...)
#define CILK_ASSERT(w, ex)
#define CILK_ASSERT_G(ex)
#define CILK_ABORT(w, msg)
#define CILK_ABORT_G(msg)
#define WHEN_CILK_DEBUG(ex)
#endif // CILK_DEBUG

// to silence compiler warning for vars only used during debugging
#define USE_UNUSED(var) (void)(var)
#endif
