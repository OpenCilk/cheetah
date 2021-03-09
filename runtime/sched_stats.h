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
    INTERVAL_SLEEP,    // spleeing time in work-stealing loop
    INTERVAL_SLEEP_UNCILK,  // spleeing time outside of work-stealing loop
    INTERVAL_CILKIFY_ENTER, // time entering cilkified regions
    INTERVAL_CILKIFY_EXIT,  // time exiting cilkified regions
    NUMBER_OF_STATS    // must be the very last entry
};

struct sched_stats {
    uint64_t time[NUMBER_OF_STATS];  // Total time measured for all stats
    uint64_t count[NUMBER_OF_STATS];
    uint64_t begin[NUMBER_OF_STATS]; // Begin time of current measurement
    uint64_t end[NUMBER_OF_STATS];   // End time of current measurement

    uint64_t steals;
    uint64_t repos;
};

struct global_sched_stats {
    // Stats for the boss thread
    uint64_t boss_waiting;
    uint64_t boss_wait_count;
    uint64_t boss_begin;
    uint64_t exit_time;
    uint64_t boss_end;
    uint64_t steals;
    uint64_t repos;
    double time[NUMBER_OF_STATS]; // Total time measured for all stats
    uint64_t count[NUMBER_OF_STATS];
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
void cilk_switch_timing(__cilkrts_worker *w, enum timing_type t1,
                        enum timing_type t2);
CHEETAH_INTERNAL
void cilk_drop_timing(__cilkrts_worker *w, enum timing_type t);
CHEETAH_INTERNAL
void cilk_boss_start_timing(struct global_state *g);
CHEETAH_INTERNAL
void cilk_boss_stop_timing(struct global_state *g);
CHEETAH_INTERNAL
void cilk_exit_worker_timing(struct global_state *g);
CHEETAH_INTERNAL
void cilk_sched_stats_print(struct global_state *g);
// void cilk_reset_timing(__cilkrts_worker *w, enum timing_type t);
// FIXME: should have a header file that's user-code interfacing
// void __cilkrts_reset_timing(); // user-code facing

#define WHEN_SCHED_STATS(ex) ex
#define CILK_START_TIMING(w, t) cilk_start_timing(w, t)
#define CILK_STOP_TIMING(w, t) cilk_stop_timing(w, t)
#define CILK_SWITCH_TIMING(w, t1, t2) cilk_switch_timing(w, t1, t2)
#define CILK_DROP_TIMING(w, t) cilk_drop_timing(w, t)
#define CILK_BOSS_START_TIMING(g) cilk_boss_start_timing(g)
#define CILK_BOSS_STOP_TIMING(g) cilk_boss_stop_timing(g)
#define CILK_EXIT_WORKER_TIMING(g) cilk_exit_worker_timing(g)

#else
#define cilk_global_sched_stats_init(s)
#define cilk_sched_stats_init(s)
#define cilk_sched_stats_print(g)
// #define cilk_reset_timing()

#define WHEN_SCHED_STATS(ex)
#define CILK_START_TIMING(w, t)
#define CILK_STOP_TIMING(w, t)
#define CILK_SWITCH_TIMING(w, t1, t2)
#define CILK_DROP_TIMING(w, t)
#define CILK_BOSS_START_TIMING(g)
#define CILK_BOSS_STOP_TIMING(g)
#define CILK_EXIT_WORKER_TIMING(g)
#endif // SCHED_STATS

#endif // __SCHED_STATS_HEADER__
