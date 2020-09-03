#include "debug.h"
#include "cilk-internal.h"
#include "global.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CHEETAH_INTERNAL unsigned int alert_level = 0;
CHEETAH_INTERNAL unsigned int debug_level = 0;

/* To reduce overhead of logging messages are accumulated into memory
   and written to stderr in batches of about 5,000 bytes. */
static size_t alert_log_size = 0, alert_log_offset = 0;
static char *alert_log = NULL;

void set_alert_level(unsigned int level) {
    alert_level = level;
    if (level == 0) {
        flush_alert_log();
        return;
    }
    if (level & 0x80000000) {
        return;
    }
    if (alert_log == NULL) {
        alert_log_size = 5000;
        alert_log = malloc(alert_log_size);
        if (alert_log) {
            memset(alert_log, ' ', alert_log_size);
        }
    }
}

void set_debug_level(unsigned int level) { debug_level = level; }

const char *const __cilkrts_assertion_failed =
    "%s:%d: cilk assertion failed: %s\n";

void cilk_die_internal(struct global_state *const g, const char *fmt, ...) {
    fflush(stdout);
    va_list l;
    va_start(l, fmt);
    cilk_mutex_lock(&(g->print_lock));
    flush_alert_log();
    fprintf(stderr, "Fatal error: ");
    vfprintf(stderr, fmt, l);
    fputc('\n', stderr);
    fflush(stderr);
    cilk_mutex_unlock(&(g->print_lock));
    exit(1);
}

CHEETAH_INTERNAL_NORETURN
void cilkrts_bug(__cilkrts_worker *w, const char *fmt, ...) {
    fflush(NULL);
    if (w) {
        cilk_mutex_lock(&(w->g->print_lock));
        flush_alert_log();
        cilk_mutex_unlock(&(w->g->print_lock));
        fprintf(stderr, "[W%02u]: ", w->self);
    }
    /* Without a worker there is no safe way to flush the log */
    va_list l;
    va_start(l, fmt);
    vfprintf(stderr, fmt, l);
    va_end(l);
    fputc('\n', stderr);
    fflush(stderr);
    abort(); // generate core file
}

void flush_alert_log() {
    if (ALERT_LVL == 0)
        return;
    if (alert_log == NULL) {
        return;
    }
    if (alert_log_offset > 0) {
        fflush(stdout);
        fwrite(alert_log, 1, alert_log_offset, stderr);
        alert_log_offset = 0;
    }
    alert_log_size = 0;
    free(alert_log);
    alert_log = NULL;
}

#undef cilkrts_alert

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

CHEETAH_INTERNAL
void cilkrts_alert(const int lvl, __cilkrts_worker *w, const char *fmt, ...) {
    if (ALERT_LVL == 0)
        return;
    char prefix[10], body[200];
    size_t size1 = 0, size2 = 0;
    if (w) {
        size1 = snprintf(prefix, sizeof prefix, "[W%02u]: ", w->self);
        assert(size1 >= 7 && size1 < 10);
    }
    {
        va_list l;
        va_start(l, fmt);
        int tmp = vsnprintf(body, sizeof body, fmt, l);
        assert(tmp >= 0);
        size2 = tmp;
        if (size2 > sizeof body - 1)
            size2 = sizeof body - 1;
        va_end(l);
    }

    pthread_mutex_lock(&lock);
    if (alert_log) {
        if (alert_log_offset + size1 + size2 + 1 >= alert_log_size) {
            fwrite(alert_log, 1, alert_log_offset, stderr);
            memset(alert_log, ' ', alert_log_offset);
            alert_log_offset = 0;
        }
        memcpy(alert_log + alert_log_offset, prefix, size1);
        memcpy(alert_log + alert_log_offset + size1, body, size2);
        alert_log[alert_log_offset + size1 + size2] = '\n';
        alert_log_offset += size1 + size2 + 1;
    } else {
        if (w)
            fprintf(stderr, "%s%s\n", prefix, body);
        else
            fprintf(stderr, "%s\n", body);
    }
    pthread_mutex_unlock(&lock);
}
