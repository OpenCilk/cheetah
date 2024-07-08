#ifndef _CONFIG_H
#define _CONFIG_H

/* Functions defined in the library and visible outside the library. */
#ifndef CHEETAH_API
#if defined __BSD__ || defined __linux__ /* really, if using ELF */
#define CHEETAH_API __attribute((visibility("protected")))
#else
#define CHEETAH_API
#endif
#endif
/* Functions defined in the library and not visible outside the library. */
#ifndef CHEETAH_INTERNAL
#define CHEETAH_INTERNAL __attribute((visibility("hidden")))
#endif
#ifndef CHEETAH_INTERNAL_NORETURN
#define CHEETAH_INTERNAL_NORETURN __attribute((noreturn, visibility("hidden")))
#endif

#ifndef __CILKRTS_VERSION
#define __CILKRTS_VERSION 0x0
#endif

#ifndef __CILKRTS_ABI_VERSION
#define __CILKRTS_ABI_VERSION 4
#endif

#ifndef CILK_DEBUG
#define CILK_DEBUG 1
#endif

#ifndef CILK_ENABLE_ASAN_HOOKS
#define CILK_ENABLE_ASAN_HOOKS 0
#endif

#ifndef CILK_STATS
#define CILK_STATS 0
#endif

#ifndef CILK_CACHE_LINE
// Use 128-bit cache lines to account for adjacent-cache-line prefetchers.
#define CILK_CACHE_LINE 128
#endif

#ifndef BUSY_PAUSE
#define BUSY_PAUSE 1
#endif

#ifndef BUSY_LOOP_SPIN
#define BUSY_LOOP_SPIN 4096 / BUSY_PAUSE
#endif

#ifndef ENABLE_THIEF_SLEEP
#define ENABLE_THIEF_SLEEP 1
#endif

#ifndef ENABLE_EXTENSION
#define ENABLE_EXTENSION 1
#endif

#ifndef ENABLE_WORKER_PINNING
#define ENABLE_WORKER_PINNING 0
#endif

#ifndef MIN_NUM_PAGES_PER_STACK
#define MIN_NUM_PAGES_PER_STACK 4 // must be greater than 1
#endif

_Static_assert(MIN_NUM_PAGES_PER_STACK >= 2, "Invalid Cheetah RTS config: MIN_NUM_PAGES_PER_STACK must be at least 2");

#ifndef MAX_NUM_PAGES_PER_STACK
#define MAX_NUM_PAGES_PER_STACK 2000
#endif

_Static_assert(MAX_NUM_PAGES_PER_STACK >= MIN_NUM_PAGES_PER_STACK, "Invalid Cheetah RTS config: MAX_NUM_PAGES_PER_STACK must be at least MIN_NUM_PAGES_PER_STACK");

#ifndef DEFAULT_NPROC
#define DEFAULT_NPROC 0 // 0 for # of cores available
#endif

#ifndef DEFAULT_DEQ_DEPTH
#define DEFAULT_DEQ_DEPTH 1024
#endif

#ifndef LG_STACK_SIZE
#define LG_STACK_SIZE 20 // 1 MBytes
#endif

#ifndef DEFAULT_STACK_SIZE
#define DEFAULT_STACK_SIZE (1U << LG_STACK_SIZE) // 1 MBytes
#endif

#ifndef DEFAULT_FIBER_POOL_CAP
#define DEFAULT_FIBER_POOL_CAP 8 // initial per-worker fiber pool capacity
#endif

#ifndef MAX_CALLBACKS
#define MAX_CALLBACKS 32 // Maximum number of init or exit callbacks
#endif

#ifndef HYPER_TABLE_HIDDEN
#define HYPER_TABLE_HIDDEN 1
#endif

#endif                   // _CONFIG_H
