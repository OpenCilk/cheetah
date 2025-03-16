#include "../runtime/cilk-internal.h"
#include "../runtime/global.h"

__cilkrts_worker default_worker = {
    .self = 0,
    .hyper_table = NULL,
    .g = NULL,
    .l = NULL,
    .extension = NULL,
    .ext_stack = NULL,
    .tail = NULL,
    .exc = NULL,
    .head = NULL,
    .ltq_limit = NULL};

__thread __cilkrts_worker *__cilkrts_tls_worker = &default_worker;