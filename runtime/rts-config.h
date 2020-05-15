#ifndef _CONFIG_H
#define _CONFIG_H

#define CHEETAH_INTERNAL __attribute((visibility("hidden")))
#define CHEETAH_INTERNAL_NORETURN __attribute((noreturn, visibility("hidden")))

#define __CILKRTS_VERSION 0x0

/* The definition must be spelled the same as in cilk/common.h, 1 not 0x1. */
#if __cilk >= 300 || defined OPENCILK_ABI
#define __CILKRTS_ABI_VERSION 2
#else
#define __CILKRTS_ABI_VERSION 1
#endif

#define CILK_DEBUG 1
#define CILK_STATS 0

#define CILK_CACHE_LINE 64

#define PROC_SPEED_IN_GHZ 2.2

#if defined __linux__
#define CILK_PAGE_SIZE 0 /* page size not available at compile time */
#elif defined __APPLE__
#define CILK_PAGE_SIZE 4096 /* Apple implies x86 or ARM */
#else
#include <machine/param.h>
#endif

#define MIN_NUM_PAGES_PER_STACK 4
#define MAX_NUM_PAGES_PER_STACK 2000
#define DEFAULT_STACKSIZE 0x100000 // 1 MBytes

/* The largest known stack alignment requirement is for AVX-512
   which may access memory in aligned 64 byte units. */
#define MAX_STACK_ALIGN 64

#define DEFAULT_NPROC 0 // 0 for # of cores available
#define DEFAULT_DEQ_DEPTH 1024
#define DEFAULT_STACK_SIZE 0x100000 // 1 MBytes
#define DEFAULT_FIBER_POOL_CAP 128  // initial per-worker fiber pool capacity
#endif                              // _CONFIG_H
