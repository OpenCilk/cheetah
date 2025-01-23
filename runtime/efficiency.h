#ifndef _EFFICIENCY_H
#define _EFFICIENCY_H
// Information for histories of efficient and inefficient worker-count samples
// and for sentinel counts.
typedef uint32_t history_sample_t;
#define HISTORY_LENGTH 32
#define SENTINEL_COUNT_HISTORY 4

typedef struct history_t {
    history_sample_t inefficient_history;
    history_sample_t efficient_history;
    unsigned int sentinel_count_history_tail;
    unsigned int recent_sentinel_count;
    unsigned int fails;
    unsigned int sample_threshold;
    unsigned int sentinel_count_history[SENTINEL_COUNT_HISTORY];
} history_t;

#endif
