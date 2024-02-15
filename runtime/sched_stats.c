#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "cilk-internal.h"
#include "debug.h"
#include "global.h"
#include "internal-malloc-impl.h"
#include "local.h"
#include "sched_stats.h"
#include "types.h"

#if SCHED_STATS
static const char *enum_to_str(enum timing_type t) {
    switch (t) {
    case INTERVAL_WORK:
        return "working";
    case INTERVAL_SCHED:
        return "scheduling";
    case INTERVAL_IDLE:
        return "idling";
    case INTERVAL_SLEEP:
        return "sleep (sched)";
    case INTERVAL_SLEEP_UNCILK:
        return "sleep (uncilk)";
    case INTERVAL_CILKIFY_ENTER:
        return "cilkify (enter)";
    case INTERVAL_CILKIFY_EXIT:
        return "cilkify (exit)";
    default:
        return "unknown";
    }
}

__attribute__((unused)) static inline double
micro_sec_to_sec(double micro_sec) {
    return micro_sec / 1000000.0;
}

static inline double nsec_to_sec(uint64_t nsec) { return nsec / 1.0e9; }

static inline uint64_t begin_time() {
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return (res.tv_sec * 1e9) + (res.tv_nsec);
}

static inline uint64_t end_time() {
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return (res.tv_sec * 1e9) + (res.tv_nsec);
}

int nr_events = 32;

void cilk_global_sched_stats_init(struct global_sched_stats *s) {
    s->boss_waiting = 0;
    s->boss_wait_count = 0;
    s->boss_begin = 0;
    s->boss_end = 0;
    s->exit_time = 0;
    s->steals = 0;
    s->repos = 0;
    s->reeng_rqsts = 0;
    s->onesen_rqsts = 0;
    for (int i = 0; i < NUMBER_OF_STATS; ++i) {
        s->time[i] = 0.0;
        s->count[i] = 0;
    }
}

void cilk_sched_stats_init(struct sched_stats *s) {
    for (int i = 0; i < NUMBER_OF_STATS; ++i) {
        s->begin[i] = 0;
        s->end[i] = 0;
        s->time[i] = 0;
        s->count[i] = 0;
    }
    s->steals = 0;
    s->repos = 0;
    s->reeng_rqsts = 0;
    s->onesen_rqsts = 0;
}

void cilk_start_timing(__cilkrts_worker *w, enum timing_type t) {
    if (w) {
        struct sched_stats *s = &(w->l->stats);
        CILK_ASSERT(s->begin[t] == 0);
        s->end[t] = 0;
        s->begin[t] = begin_time();
    }
}

void cilk_stop_timing(__cilkrts_worker *w, enum timing_type t) {
    if (w) {
        struct sched_stats *s = &(w->l->stats);
        CILK_ASSERT(s->end[t] == 0);
        s->end[t] = end_time();
        CILK_ASSERT(s->end[t] >= s->begin[t]);
        s->time[t] += (s->end[t] - s->begin[t]);
        s->count[t]++;
        s->begin[t] = 0;
    }
}

void cilk_switch_timing(__cilkrts_worker *w, enum timing_type t1,
                        enum timing_type t2) {
    if (w) {
        struct sched_stats *s = &(w->l->stats);
        // Stop timer t1
        CILK_ASSERT(s->end[t1] == 0);
        s->end[t1] = end_time();
        CILK_ASSERT(s->end[t1] >= s->begin[t1]);
        s->time[t1] += (s->end[t1] - s->begin[t1]);
        s->count[t1]++;
        s->begin[t1] = 0;

        // Start timer t2 where t1 left off
        CILK_ASSERT(s->begin[t2] == 0);
        s->end[t2] = 0;
        s->begin[t2] = begin_time();
    }
}

void cilk_drop_timing(__cilkrts_worker *w, enum timing_type t) {
    if (w) {
        struct sched_stats *s = &(w->l->stats);
        CILK_ASSERT(s->begin[t] != 0);
        s->begin[t] = 0;
    }
}

void cilk_boss_start_timing(struct global_state *g) {
    struct global_sched_stats *s = &(g->stats);
    CILK_ASSERT(s->boss_begin == 0);
    s->boss_begin = begin_time();
    s->boss_end = 0;
}

void cilk_boss_stop_timing(struct global_state *g) {
    struct global_sched_stats *s = &(g->stats);
    CILK_ASSERT(s->boss_end == 0);
    s->boss_end = end_time();
    CILK_ASSERT(s->boss_end >= s->boss_begin);
    CILK_ASSERT(s->boss_end >= s->exit_time);
    uint64_t last = s->exit_time > s->boss_begin ? s->exit_time : s->boss_begin;
    s->boss_waiting += (s->boss_end - last);
    s->boss_wait_count++;
    s->boss_begin = 0;
    s->exit_time = 0;
}

void cilk_exit_worker_timing(struct global_state *g) {
    struct global_sched_stats *s = &(g->stats);
    CILK_ASSERT(s->exit_time == 0);
    s->exit_time = begin_time();
}

static void sched_stats_reset_worker(__cilkrts_worker *w,
                                     void *data __attribute__((unused))) {
    local_state *l = w->l;
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        l->stats.time[t] = 0;
        l->stats.count[t] = 0;
    }
    l->stats.steals = 0;
    l->stats.repos = 0;
    l->stats.reeng_rqsts = 0;
    l->stats.onesen_rqsts = 0;
}

#define COL_DESC "%15s"
#define HDR_DESC "%18s %10s"
#define WORKER_HDR_DESC "%10s %3u:"
#define FIELD_DESC "%18.6f %10" PRIu64
#define COUNT_HDR_DESC "%10s"
#define COUNT_DESC "%10" PRIu64

static void sched_stats_print_worker(__cilkrts_worker *w, void *data) {
    FILE *fp = (FILE *)data;
    fprintf(fp, WORKER_HDR_DESC, "Worker", w->self);
    global_state *g = w->g;
    local_state *l = w->l;
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        double tmp = nsec_to_sec(l->stats.time[t]);
        g->stats.time[t] += (double)tmp;
        uint64_t tmp_count = l->stats.count[t];
        g->stats.count[t] += tmp_count;
        fprintf(fp, FIELD_DESC, tmp, tmp_count);
    }
    g->stats.steals += l->stats.steals;
    g->stats.repos += l->stats.repos;
    g->stats.reeng_rqsts += l->stats.reeng_rqsts;
    g->stats.onesen_rqsts += l->stats.onesen_rqsts;

    fprintf(stderr, COUNT_DESC, l->stats.steals);
    fprintf(stderr, COUNT_DESC, l->stats.repos);
    fprintf(stderr, COUNT_DESC, l->stats.reeng_rqsts);
    fprintf(stderr, COUNT_DESC, l->stats.onesen_rqsts);
    fprintf(fp, "\n");
}

void cilk_sched_stats_print(struct global_state *g) {
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        g->stats.time[t] = 0.0;
        g->stats.count[t] = 0;
    }
    g->stats.steals = 0;
    g->stats.repos = 0;
    g->stats.reeng_rqsts = 0;
    g->stats.onesen_rqsts = 0;

    fprintf(stderr, "\nSCHEDULING STATS (SECONDS):\n");
    {
        fprintf(stderr, COL_DESC, "Boss waiting:");
        double tmp = nsec_to_sec(g->stats.boss_waiting);
        fprintf(stderr, FIELD_DESC, tmp, g->stats.boss_wait_count);
        fprintf(stderr, "\n");
        g->stats.boss_waiting = 0;
        g->stats.boss_wait_count = 0;
    }
    fprintf(stderr, COL_DESC, "");
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        fprintf(stderr, HDR_DESC, enum_to_str(t), "count");
    }
    fprintf(stderr, COUNT_HDR_DESC, "steals");
    fprintf(stderr, COUNT_HDR_DESC, "reposses");
    fprintf(stderr, COUNT_HDR_DESC, "reengs");
    fprintf(stderr, COUNT_HDR_DESC, "onesen");
    fprintf(stderr, "\n");

    for_each_worker(g, &sched_stats_print_worker, stderr);

    fprintf(stderr, COL_DESC, "Total:");
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        fprintf(stderr, FIELD_DESC, g->stats.time[t], g->stats.count[t]);
    }
    fprintf(stderr, COUNT_DESC, g->stats.steals);
    fprintf(stderr, COUNT_DESC, g->stats.repos);
    fprintf(stderr, COUNT_DESC, g->stats.reeng_rqsts);
    fprintf(stderr, COUNT_DESC, g->stats.onesen_rqsts);
    fprintf(stderr, "\n");

    for_each_worker(g, &sched_stats_reset_worker, NULL);
}

/*
void cilk_reset_timing() {
    global_state_t *g = cilkg_get_global_state();
    for(int i = 0; i < NUMBER_OF_STATS; ++i) {
        //resent global counter
        g->stats->time[i] = 0.0;

        //reset worker local counter
        for(int j = 0; j < g->total_workers; ++j) {
            g->workers[j]->l->stats->time[i] = 0;
        }

    }

    LIKWID_MARKER_CLOSE;
    LIKWID_MARKER_INIT;
}
*/
#endif

void __cilkrts_sched_stats_print(void) {
    WHEN_SCHED_STATS(cilk_sched_stats_print(default_cilkrts));
}
