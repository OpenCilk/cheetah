#pragma once

#if __cilk
    #include <c2cilk/internal/backends/cilk.h>
#else
    #include <c2cilk/internal/backends/clang.h>
#endif
