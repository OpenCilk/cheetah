#include "debug.h"
#include "cilk-internal.h"
#include "global.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

CHEETAH_INTERNAL unsigned int alert_level = ALERT_LVL;

const char *const __cilkrts_assertion_failed =
    "%s:%d: cilk assertion failed: %s\n";

void cilk_die_internal(struct global_state *const g, const char *complain) {
    cilk_mutex_lock(&(g->print_lock));
    fprintf(stderr, "Fatal error: %s\n", complain);
    cilk_mutex_unlock(&(g->print_lock));
    exit(1);
}

CHEETAH_INTERNAL_NORETURN
void cilkrts_bug(__cilkrts_worker *w, const char *fmt, ...) {
    if (w) {
        fprintf(stderr, "[W%02u]: ", w->self);
    }

    /* To reduce user confusion, write all user-generated output
       before the system-generated error message. */
    va_list l;
    fflush(NULL);
    va_start(l, fmt);
    vfprintf(stderr, fmt, l);
    va_end(l);
    fputc('\n', stderr);
    fflush(stderr);

    abort(); // generate core file
}

#if CILK_DEBUG
#undef cilkrts_alert

CHEETAH_INTERNAL
void cilkrts_alert(const int lvl, __cilkrts_worker *w, const char *fmt, ...) {
    /* To reduce user confusion, write all user-generated output
       before the system-generated error message. */
#ifndef ALERT_LVL
    if (w) {
        fprintf(stderr, "[W%02u]: ", w->self);
    }
    va_list l;
    va_start(l, fmt);
    vfprintf(stderr, fmt, l);
    va_end(l);
    fputc('\n', stderr);
#else
    if (lvl & ALERT_LVL) {
        if (w) {
            fprintf(stderr, "[W%02u]: ", w->self);
        }
        va_list l;
        va_start(l, fmt);
        vfprintf(stderr, fmt, l);
        va_end(l);
        fputc('\n', stderr);
    }
#endif
}
#endif
