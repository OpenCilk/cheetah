#include "debug.h"
#include "cilk-internal.h"
#include "global.h"

#include <assert.h>
#include <ctype.h>
#include <search.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define ALERT_STR_BUF_LEN 16

typedef struct __alert_level_t {
    const char *const name;
    const int mask_value;
} alert_level_t;

static const alert_level_t alert_table[] = {
    {"none", ALERT_NONE},
    {"fiber", ALERT_FIBER},
    {"memory", ALERT_MEMORY},
    {"sync", ALERT_SYNC},
    {"sched", ALERT_SCHED},
    {"steal", ALERT_STEAL},
    {"return", ALERT_RETURN},
    {"except", ALERT_EXCEPT},
    {"cframe", ALERT_CFRAME},
    {"reduce", ALERT_REDUCE},
    {"reduce_id", ALERT_REDUCE_ID},
    {"boot", ALERT_BOOT},
    {"start", ALERT_START},
    {"closure", ALERT_CLOSURE},
    // Must be last in the table
    {"nobuf", ALERT_NOBUF},
};

static int alert_name_comparison(const void *a, const void *b) {
    const alert_level_t *ala = (const alert_level_t*)a;
    const alert_level_t *alb = (const alert_level_t*)b;

    return strcmp(ala->name, alb->name);
}

static size_t get_alert_table_size() {
    size_t s = 0;

    for (s = 0; alert_table[s].mask_value != ALERT_NOBUF; ++s) {
        // no-op
    }

    return s;
}

static int parse_alert_level_str(const char *const alert_str) {
    char alert_str_lowered[512];
    // save 1 for the null terminator
    size_t max_str_len = sizeof(alert_str_lowered) - 1;

    size_t which_alert;
    size_t table_size = get_alert_table_size();
    size_t alert_len = strlen(alert_str);

    size_t i;
    for (i = 0; i < alert_len && i < max_str_len; ++i) {
        alert_str_lowered[i] = tolower(alert_str[i]);
    }
    alert_str_lowered[i] = '\0';

    alert_level_t search_key = { .name = alert_str, .mask_value = ALERT_NONE };

    // The table is small, and performance isn't critical for loading
    // debug options, so use linear search (lfind)
    alert_level_t *table_element =
        (alert_level_t*)lfind(&search_key, alert_table, &table_size,
                              sizeof(search_key), alert_name_comparison
        );

    if (table_element != NULL) {
        return table_element->mask_value;
    }
    
    fprintf(stderr, "Invalid CILK_ALERT value: %s\n", alert_str);

    return ALERT_NONE;
}

static int parse_alert_level_env(char *alert_env) {
    int new_alert_lvl = ALERT_NONE;

    size_t env_len = strlen(alert_env);

    char *alert_str = strtok(alert_env, ",");

    if (alert_str) {
        if (strlen(alert_str) == env_len) {
            // Can be a number
            char **tol_end = &alert_env;
            new_alert_lvl = strtol(alert_env, tol_end, 0);
            if (new_alert_lvl == 0 && (**tol_end != '\0' || *tol_end == alert_env)) {
                new_alert_lvl |= parse_alert_level_str(alert_str);
            }
        } else {
            while (alert_str != NULL) {
                new_alert_lvl |= parse_alert_level_str(alert_str);
                char *new_alert_str = strtok(NULL, ",");
                // Do this after reading the next pointer to avoid
                // (1) branching (if last string) and (2) segfaulting
                // in strtok due to overwriting the null terminator.
                alert_str[strlen(alert_str)] = ',';
                alert_str = new_alert_str;
            }
        }
    }

    // We may have overwritten NULL with , above;
    // fix it here
    alert_env[env_len] = '\0';

    return new_alert_lvl;
}

void set_alert_level_from_str(char *alert_env) {
    if (alert_env) {
        int new_alert_lvl = parse_alert_level_env(alert_env);
        set_alert_level(new_alert_lvl);
    }
}

void set_alert_level(unsigned int level) {
    alert_level = level;
    if (level == 0) {
        flush_alert_log();
        return;
    }
    if (level & ALERT_NOBUF) {
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
