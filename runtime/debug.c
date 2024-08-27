#include "debug.h"
#include "cilk-internal.h"
#include "global.h"

#include <assert.h>
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

/**
 * Represents a usable alert level with a human-readable
 * <code>name<\code> and the corresponding
 * <code>mask_value<\code> used by the runtime.
 **/
typedef struct __alert_level_t {
    const char *name;
    int mask_value;
} alert_level_t;

/**
 * A table relating a human-readable alert level name to
 * the corresponding bitfield value used by the runtime.
 **/
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
    {"nobuf", ALERT_NOBUF},
};

/**
 * Compare the <code>name<\code> of the <code>alert_level_t<\code> pointed to by
 * <code>left<code> to the <code>name<\code> of the <code>alert_level_t<\code>
 * pointed to by <code>right<\code>, ignoring case.
 *
 * @param left   <code>alert_level_t<\code> to compare
 * @param right  <code>alert_level_t<\code> to compare
 *
 * @return       Ignoring case, returns negative if left->name < right->name,
 *               0 if they are equal, or positive if left->name > right->name
 **/
static int alert_name_comparison(const void *left, const void *right) {
    const alert_level_t *al_left = (const alert_level_t*)left;
    const alert_level_t *al_right = (const alert_level_t*)right;

    // TODO: If Windows, use _stricmp
    return strcasecmp(al_left->name, al_right->name);
}

/**
 * Parse an alert level represented by a string into the proper bitmask value.
 * If the string is not represented in <code>alert_table<\code>, then prints
 * an error and returns ALERT_NONE.
 *
 * @param alert_str  A C string to attempt to parse
 *
 * @return           The bitmask corresponding to <code>alert_str<\code>, if in
 *                   <code>alert_table<\code>, else ALERT_NONE.
 **/
static int parse_alert_level_str(const char *const alert_str) {
    size_t table_size = sizeof(alert_table) / sizeof(alert_table[0]);

    const alert_level_t search_key = { .name = alert_str, .mask_value = ALERT_NONE };

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

/**
 * Parse a CSV line representing which alert levels should be enabled, and
 * return the bitmask representing all of the passed in options. If the CSV line
 * is a single number, then treat that number as the bitmask.
 * <code>alert_csv<\code> is copied, as <code>strtok<\code> is used, and it
 * modifies its arguments.
 *
 * @param alert_csv  A C string that is either a comma-separated list of alert
 *                   level names -or- a single number.
 *
 * @return           The bitmask representing the passed in
 *                   <code>alert_csv<\code>. If <code>alert_csv<\code> cannot
 *                   be copied, then returns the current
 *                   <code>alert_level<\code> value.
 **/
static int parse_alert_level_csv(const char *const alert_csv) {
    int new_alert_lvl = ALERT_NONE;

    size_t csv_len = strlen(alert_csv);

    // strtok modifies the passed in string, so copy alert_csv and use
    // the copy instead
    char *alert_csv_cpy = malloc(csv_len + 1);
    if (!alert_csv_cpy) {
        // Non-critical error, so just print a warning
        fprintf(stderr, "Cilk: unable to copy CILK_ALERT settings (%s)\n",
                strerror(errno)
               );
        return alert_level;
    }
    strcpy(alert_csv_cpy, alert_csv);

    char *alert_str = strtok(alert_csv_cpy, ",");

    if (alert_str) {
        if (strlen(alert_str) == csv_len) {
            // Can be a number, as there is no other option in the string
            char *tol_end = alert_csv_cpy;
            new_alert_lvl = strtol(alert_csv_cpy, &tol_end, 0);
            if (new_alert_lvl == 0 && (*tol_end != '\0' || tol_end == alert_csv_cpy)) {
                new_alert_lvl |= parse_alert_level_str(alert_str);
            }
        } else {
            for (; alert_str; alert_str = strtok(NULL, ",")) {
                new_alert_lvl |= parse_alert_level_str(alert_str);
            }
        }
    }

    free(alert_csv_cpy);

    return new_alert_lvl;
}

/**
 * Parse a CSV line representing which alert levels should be enabled, and
 * and set the current <code>alert_level<\code> bitamsk based on the result.
 * If the passed in C string is NULL, then no change is made.
 *
 * @param alert_csv  A C string that is either a comma-separated list of alert
 *                   level names -or- a single number.
 **/
void set_alert_level_from_str(const char *const alert_csv) {
    if (alert_csv) {
        int new_alert_lvl = parse_alert_level_csv(alert_csv);
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
