#ifndef __SCHED_STATS_HEADER__
#define __SCHED_STATS_HEADER__

#include "rts-config.h"
#include <stdint.h>

typedef struct __cilkrts_worker __cilkrts_worker;

#define SCHED_STATS CILK_STATS

enum timing_type {
    INTERVAL_WORK = 0, // work time
    INTERVAL_SCHED,    // scheduling time
    INTERVAL_IDLE,     // idle time
    NUMBER_OF_STATS    // must be the very last entry
};

struct sched_stats {
    uint64_t time[NUMBER_OF_STATS];  // Total time measured for all stats
    uint64_t begin[NUMBER_OF_STATS]; // Begin time of current measurement
    uint64_t end[NUMBER_OF_STATS];   // End time of current measurement
};

struct global_sched_stats {
    double time[NUMBER_OF_STATS]; // Total time measured for all stats
};

#if SCHED_STATS
CHEETAH_INTERNAL
void cilk_global_sched_stats_init(struct global_sched_stats *s);
CHEETAH_INTERNAL
void cilk_sched_stats_init(struct sched_stats *s);
CHEETAH_INTERNAL
void cilk_start_timing(__cilkrts_worker *w, enum timing_type t);
CHEETAH_INTERNAL
void cilk_stop_timing(__cilkrts_worker *w, enum timing_type t);
CHEETAH_INTERNAL
void cilk_drop_timing(__cilkrts_worker *w, enum timing_type t);
CHEETAH_INTERNAL
void cilk_sched_stats_print(struct global_state *g);
// void cilk_reset_timing(__cilkrts_worker *w, enum timing_type t);
// FIXME: should have a header file that's user-code interfacing
// void __cilkrts_reset_timing(); // user-code facing

#define WHEN_SCHED_STATS(ex) ex
#define CILK_START_TIMING(w, t) cilk_start_timing(w, t)
#define CILK_STOP_TIMING(w, t) cilk_stop_timing(w, t)
#define CILK_DROP_TIMING(w, t) cilk_drop_timing(w, t)

#else
#define cilk_global_sched_stats_init(s)
#define cilk_sched_stats_init(s)
#define cilk_sched_stats_print(g)
// #define cilk_reset_timing()

#define WHEN_SCHED_STATS(ex)
#define CILK_START_TIMING(w, t)
#define CILK_STOP_TIMING(w, t)
#define CILK_DROP_TIMING(w, t)
#endif // SCHED_STATS

#endif // __SCHED_STATS_HEADER__
