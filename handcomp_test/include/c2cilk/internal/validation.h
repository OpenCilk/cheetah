#pragma once

/**
 * Denotes whether or not we are in a c2cilk_context, and thus capable of
 * spawning. Used to assert that spawns and syncs are placed in valid locations.
 *
 * Defaults to false.
 * Is shadowed withing local scopes to enable spawns and syncs.
 */
enum { __c2cilk_in_c2cilk_context = false };

/**
 * Denotes whether or not we are in a c2cilk function, and thus capable of
 * having cilk_contexts. Used to assert that we are in a cilk function when we
 * add a c2cilk_context. By default, main is assumed to be a cilk function that
 * cannot be spawned.
 *
 * Defaults to false.
 * Is shadowed and set to true in cilk functions defined using the appropriate
 * macros (CILK_FUNC, CILK_VOID_FUNC).
 */
enum { __c2cilk_in_c2cilk_function = false };


/**
 * Denotes whether or not the programmer has specified that they intend to use
 * this unsafe library. There are no guarantees there are not footguns in this
 * library, and THIS LIBRARY IS DEFINITELY NOT THE INTENDED CILK INTERFACE.
 * This library exists for internal testing and benchmarking purposes.
 *
 * Is set to true if the proper magic is supplied.
 */
enum { __unsafe_c2cilk_library_enabled = 
#if !defined(ENABLE_UNSAFE_C2CILK_LIBRARY)
    0
#else
    /* 1ee7-like spelling of "danger" */
    (ENABLE_UNSAFE_C2CILK_LIBRARY == 0xda179e12)
#endif
};

#define C2CILK_STATIC_ASSERT(...) \
    _Static_assert(__VA_ARGS__)

#define C2CILK_ASSERT_SPAWN_IS_VALID                     \
    C2CILK_STATIC_ASSERT(__c2cilk_in_c2cilk_context,     \
            "ERROR: Must be in a cilk context to spawn")

#define C2CILK_DETECT_IN_MAIN                                       \
    (__func__[0] == 'm' && __func__[1] == 'a' && __func__[2] == 'i' \
        && __func__[3] == 'n' && __func__[4] == '\0')

#define C2CILK_ASSERT_CONTEXT_IS_VALID                                   \
    do {                                                                 \
        enum { __c2cilk_in_c2cilk_function_or_main =                     \
            __c2cilk_in_c2cilk_function || C2CILK_DETECT_IN_MAIN         \
        };                                                               \
        C2CILK_STATIC_ASSERT(                                            \
                __c2cilk_in_c2cilk_function_or_main,                     \
                "ERROR: Must be in a cilk function (or main) to enter a" \
                " cilk context"                                          \
                );                                                       \
        C2CILK_STATIC_ASSERT(!__c2cilk_in_c2cilk_context,                \
                "ERROR: Cannot nest cilk contexts");                     \
    } while (0)

// This error message intentionally does not specify how to enable the library
#define C2CILK_ASSERT_LIBRARY_ENABLED                                        \
    do {                                                                     \
        enum { __unsafe_c2cilk_library_properly_enabled =                    \
            __unsafe_c2cilk_library_enabled };                               \
        C2CILK_STATIC_ASSERT(__unsafe_c2cilk_library_properly_enabled,       \
                "ERROR: Use of unsafe c2cilk library"                        \
                " (intended for internal testing) is not marked intentional" \
                " using the proper magic"                                    \
        );                                                                   \
    } while (0)
