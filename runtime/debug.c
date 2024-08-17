#include "debug.h"
#include "cilk-internal.h"
#include "global.h"

#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALERT_STR_BUF_LEN 16

#if ALERT_LVL & (ALERT_CFRAME|ALERT_RETURN)
unsigned int alert_level = 0;
#else
CHEETAH_INTERNAL unsigned int alert_level = 0;
#endif
CHEETAH_INTERNAL unsigned int debug_level = 0;

/* To reduce overhead of logging messages are accumulated into memory
   and written to stderr in batches of about 5,000 bytes. */
static size_t alert_log_size = 0, alert_log_offset = 0;
static char *alert_log = NULL;

static const char *const ALERT_NONE_STR =      "none";
static const char *const ALERT_FIBER_STR =     "fiber";
static const char *const ALERT_MEMORY_STR =    "memory";
static const char *const ALERT_SYNC_STR =      "sync";
static const char *const ALERT_SCHED_STR =     "sched";
static const char *const ALERT_STEAL_STR =     "steal";
static const char *const ALERT_RETURN_STR =    "return";
static const char *const ALERT_EXCEPT_STR =    "except";
static const char *const ALERT_CFRAME_STR =    "cframe";
static const char *const ALERT_REDUCE_STR =    "reduce";
static const char *const ALERT_REDUCE_ID_STR = "reduce_id";
static const char *const ALERT_BOOT_STR =      "boot";
static const char *const ALERT_START_STR =     "start";
static const char *const ALERT_CLOSURE_STR =   "closure";

static void parse_alert_level_str(const char *const alert_str) {
    if (strcmp(ALERT_NONE_STR, alert_str) == 0) {
        // no-op
    } else if (strcmp(ALERT_FIBER_STR, alert_str) == 0) {
        alert_level |= ALERT_FIBER;
    } else if (strcmp(ALERT_MEMORY_STR, alert_str) == 0) {
        alert_level |= ALERT_MEMORY;
    } else if (strcmp(ALERT_SYNC_STR, alert_str) == 0) {
        alert_level |= ALERT_SYNC;
    } else if (strcmp(ALERT_SCHED_STR, alert_str) == 0) {
        alert_level |= ALERT_SCHED;
    } else if (strcmp(ALERT_STEAL_STR, alert_str) == 0) {
        alert_level |= ALERT_STEAL;
    } else if (strcmp(ALERT_RETURN_STR, alert_str) == 0) {
        alert_level |= ALERT_RETURN;
    } else if (strcmp(ALERT_EXCEPT_STR, alert_str) == 0) {
        alert_level |= ALERT_EXCEPT;
    } else if (strcmp(ALERT_CFRAME_STR, alert_str) == 0) {
        alert_level |= ALERT_CFRAME;
    } else if (strcmp(ALERT_REDUCE_STR, alert_str) == 0) {
        alert_level |= ALERT_REDUCE;
    } else if (strcmp(ALERT_REDUCE_ID_STR, alert_str) == 0) {
        alert_level |= ALERT_REDUCE_ID;
    } else if (strcmp(ALERT_BOOT_STR, alert_str) == 0) {
        alert_level |= ALERT_BOOT;
    } else if (strcmp(ALERT_START_STR, alert_str) == 0) {
        alert_level |= ALERT_START;
    } else if (strcmp(ALERT_CLOSURE_STR, alert_str) == 0) {
        alert_level |= ALERT_CLOSURE;
    } else {
        fprintf(stderr, "Invalid CILK_ALERT value: %s\n", alert_str);
    }
}

static void parse_alert_level_env(const char *const alert_env) {
    char curr_alert[ALERT_STR_BUF_LEN + 1];

    size_t buf_str_len = 0;

    // We only allow numbers for backwards compatibility, so
    // only allow numbers if they are the only thing passed to
    // CILK_ALERT
    bool numbers_allowed = true;
    bool invalid = false;

    for (size_t i = 0; i < strlen(alert_env); ++i) {
        if (alert_env[i] == ',') {
            curr_alert[buf_str_len] = '\0';
            numbers_allowed = false;
            if (invalid) {
                fputc('\n', stderr);
                invalid = false;
            } else {
                parse_alert_level_str(curr_alert);
            }
            buf_str_len = 0;
        } else if (buf_str_len < ALERT_STR_BUF_LEN) {
            curr_alert[buf_str_len++] = tolower(alert_env[i]);
        } else {
            curr_alert[buf_str_len] = '\0';
            fprintf(stderr, "Invalid CILK_ALERT option: %s%c", curr_alert, alert_env[i]);
            curr_alert[0] = '\0';
        }
    }

    if (invalid) {
        fputc('\n', stderr);
        buf_str_len = 0;
    }

    if (buf_str_len > 0) {
        curr_alert[buf_str_len] = '\0';

        if (numbers_allowed) {
            char **tol_end = &alert_env;
            alert_level = strtol(alert_env, tol_end, 0);
            if (alert_level == 0 && (**tol_end != '\0' || *tol_end == alert_env)) {
                parse_alert_level_str(curr_alert);
            }
        } else {
            parse_alert_level_str(curr_alert);
        }
    }
}

void set_alert_level(const char *const alert_env) {
    if (alert_env) {
        parse_alert_level_env(alert_env);
    }
}

//void set_alert_level(unsigned int level) {
//    alert_level = level;
//    if (level == 0) {
//        flush_alert_log();
//        return;
//    }
//    if (level & ALERT_NOBUF) {
//        return;
//    }
//    if (alert_log == NULL) {
//        alert_log_size = 5000;
//        alert_log = malloc(alert_log_size);
//        if (alert_log) {
//            memset(alert_log, ' ', alert_log_size);
//        }
//    }
//}

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
void cilkrts_bug(const char *fmt, ...) {
    fflush(NULL);
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
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

#if !(ALERT_LVL & (ALERT_CFRAME|ALERT_RETURN))
CHEETAH_INTERNAL
#endif
void cilkrts_alert(const int lvl, const char *fmt, ...) {
    if ((ALERT_LVL & lvl) == 0)
        return;
    char prefix[10], body[200];
    size_t size1 = 0, size2 = 0;

    __cilkrts_worker *w = __cilkrts_get_tls_worker();

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
