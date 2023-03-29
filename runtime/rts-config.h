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
#define __CILKRTS_VERSION 0x0

#define __CILKRTS_ABI_VERSION 4

#ifndef CILK_DEBUG
#define CILK_DEBUG 1
#endif

#ifndef CILK_ENABLE_ASAN_HOOKS
#define CILK_ENABLE_ASAN_HOOKS 0
#endif

#ifndef CILK_STATS
#define CILK_STATS 0
#endif

#define BOSS_THIEF 1

// Use 128-bit cache lines to account for adjacent-cache-line prefetchers.
#define CILK_CACHE_LINE 128

#define PROC_SPEED_IN_GHZ 2.2

#define BUSY_PAUSE 1
#define BUSY_LOOP_SPIN 4096 / BUSY_PAUSE

#define ENABLE_THIEF_SLEEP 1

#define ENABLE_EXTENSION 1

#define ENABLE_WORKER_PINNING 0

#define MIN_NUM_PAGES_PER_STACK 4
#define MAX_NUM_PAGES_PER_STACK 2000

/* The largest known stack alignment requirement is for AVX-512
   which may access memory in aligned 64 byte units. */
#define MAX_STACK_ALIGN 64

#define DEFAULT_NPROC 0 // 0 for # of cores available
#define DEFAULT_DEQ_DEPTH 1024
#define DEFAULT_STACK_SIZE 0x100000 // 1 MBytes
#define DEFAULT_FIBER_POOL_CAP 8  // initial per-worker fiber pool capacity
#define DEFAULT_REDUCER_LIMIT 1024
#define DEFAULT_FORCE_REDUCE 0 // do not self steal to force reduce

#define MAX_CALLBACKS 32 // Maximum number of init or exit callbacks

#define HYPER_TABLE_HIDDEN 1
#endif                   // _CONFIG_H
