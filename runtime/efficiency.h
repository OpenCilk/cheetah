#ifndef _EFFICIENCY_H
#define _EFFICIENCY_H

#include <cstdint>

// Information for histories of efficient and inefficient worker-count samples
// and for sentinel counts.
typedef uint32_t history_sample_t;
#define HISTORY_LENGTH 32
#define SENTINEL_COUNT_HISTORY 4

// Threshold for number of consective failed steal attempts to declare a
// thief as sentinel.  Must be a power of 2.
#define SENTINEL_THRESHOLD 128

// Number of attempted steals the thief should do each time it copies the
// worker state.  ATTEMPTS must divide SENTINEL_THRESHOLD.
#define ATTEMPTS 4

typedef struct history_t {
    history_sample_t inefficient_history = 0;
    history_sample_t efficient_history = 0;
    unsigned int sentinel_count_history_tail = 0;
    unsigned int recent_sentinel_count = SENTINEL_COUNT_HISTORY;
    unsigned int fails = 0; // rts->init_fails(...);
    unsigned int sample_threshold = SENTINEL_THRESHOLD;
    unsigned int sentinel_count_history[SENTINEL_COUNT_HISTORY] = { 1 };
} history_t;

#endif
