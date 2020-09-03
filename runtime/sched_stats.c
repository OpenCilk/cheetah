#include <stdio.h>

#include "cilk-internal.h"
#include "debug.h"
#include "internal-malloc-impl.h"
#include "local.h"
#include "sched_stats.h"

#if SCHED_STATS
static const char *enum_to_str(enum timing_type t) {
    switch (t) {
    case INTERVAL_WORK:
        return "working";
    case INTERVAL_SCHED:
        return "scheduling";
    case INTERVAL_IDLE:
        return "idling";
    default:
        return "unknonw";
    }
}

static inline double cycles_to_micro_sec(uint64_t cycle) {
    return (double)cycle / ((double)PROC_SPEED_IN_GHZ * 1000.0);
}

__attribute__((unused)) static inline double
micro_sec_to_sec(double micro_sec) {
    return micro_sec / 1000000.0;
}

static inline uint64_t begin_cycle_count() {
    unsigned int low, high;
    __asm__ volatile("cpuid\n\t"
                     "rdtsc\n\t"
                     "mov %%edx, %0\n\t"
                     "mov %%eax, %1\n\t"
                     : "=r"(high), "=r"(low)::"%rax", "%rbx", "%rcx", "%rdx");
    return ((uint64_t)high << 32) | low;
}

static inline uint64_t end_cycle_count() {
    unsigned int low, high;
    __asm__ volatile("rdtscp\n\t"
                     "mov %%edx, %0\n\t"
                     "mov %%eax, %1\n\t"
                     "cpuid\n\t"
                     : "=r"(high), "=r"(low)::"%rax", "%rbx", "%rcx", "%rdx");
    return ((uint64_t)high << 32) | low;
}

void cilk_global_sched_stats_init(struct global_sched_stats *s) {
    for (int i = 0; i < NUMBER_OF_STATS; ++i) {
        s->time[i] = 0.0;
    }
}

void cilk_sched_stats_init(struct sched_stats *s) {
    for (int i = 0; i < NUMBER_OF_STATS; ++i) {
        s->begin[i] = 0;
        s->end[i] = 0;
        s->time[i] = 0;
    }
}

void cilk_start_timing(__cilkrts_worker *w, enum timing_type t) {
    if (w) {
        struct sched_stats *s = &(w->l->stats);
        CILK_ASSERT(w, s->begin[t] == 0);
        s->end[t] = 0;
        s->begin[t] = begin_cycle_count();
    }
}

void cilk_stop_timing(__cilkrts_worker *w, enum timing_type t) {
    if (w) {
        struct sched_stats *s = &(w->l->stats);
        CILK_ASSERT(w, s->end[t] == 0);
        s->end[t] = end_cycle_count();
        CILK_ASSERT(w, s->end[t] > s->begin[t]);
        s->time[t] += (s->end[t] - s->begin[t]);
        s->begin[t] = 0;
    }
}

void cilk_drop_timing(__cilkrts_worker *w, enum timing_type t) {
    if (w) {
        struct sched_stats *s = &(w->l->stats);
        CILK_ASSERT(w, s->begin[t] != 0);
        s->begin[t] = 0;
    }
}

static void sched_stats_print_worker(__cilkrts_worker *w, void *data) {
    FILE *fp = (FILE *)data;
    fprintf(fp, WORKER_HDR_DESC, "Worker", w->self);
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        double tmp = cycles_to_micro_sec(w->l->stats.time[t]);
        g->stats.time[t] += (double)tmp;
        fprintf(fp, FIELD_DESC, micro_sec_to_sec(tmp));
    }
    fprintf(fp, "\n");
}

void cilk_sched_stats_print(struct global_state *g) {
#define HDR_DESC "%15s"
#define WORKER_HDR_DESC "%10s %3u:"
#define FIELD_DESC "%15.3f"

    fprintf(stderr, "\nSCHEDULING STATS (SECONDS):\n");
    fprintf(stderr, HDR_DESC, "");
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        fprintf(stderr, HDR_DESC, enum_to_str(t));
    }
    fprintf(stderr, "\n");

    for_each_worker(g, &sched_stats_print_worker, stderr);

    fprintf(stderr, HDR_DESC, "Total:");
    for (int t = 0; t < NUMBER_OF_STATS; t++) {
        fprintf(stderr, FIELD_DESC, micro_sec_to_sec(g->stats.time[t]));
    }
    fprintf(stderr, "\n");
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
