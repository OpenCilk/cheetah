#pragma once

#include <c2cilk/internal/expand.h>

// Function attributes
#define C2CILK_INLINE inline __attribute__((__always_inline__))
#define C2CILK_NOINLINE __attribute__((__noinline__))
#define C2CILK_WEAK __attribute__((__weak__))

// Variable -or- function attributes
#define C2CILK_MAYBE_UNUSED __attribute__((__unused__))

// Variable attributes
#define C2CILK_ON_CLEANUP(func)   __attribute__((__cleanup__(func)))

// Macros related to getting the preprocessor to re-expand macros that have
// already been expanded
#define C2CILK_EMPTY()
#define C2CILK_DELAY(x) x C2CILK_EMPTY()
#define C2CILK_OBSTRUCT(...) __VA_ARGS__ C2CILK_DELAY(C2CILK_EMPTY)()

// Strip the parentheses from X (e.g., if X = (Y), replaces with Y)
#define C2CILK_STRIP_PARENS(X) C2CILK_EXPAND_1 X

/**
 * Takes variadic argument of the form:
 *   `type1, name1, type2, name2, ..., typeN, nameN `
 * And transforms it into the form:
 *   `type1 name1, type2 name2, ..., typeN nameN`
 *
 * @param ...: List of variable types and names, separated by commas.
 */
#define C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS(...)                               \
    C2CILK_EXPAND_PARAMS(                                                     \
            __VA_OPT__(                                                       \
                C2CILK_EXPAND_PARAMS(                                         \
                    C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS_HELPER() (__VA_ARGS__) \
                )                                                             \
            )                                                                 \
    )

#define C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS_HELPER() \
    C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS_HELPER_HELPER

#define C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS_HELPER_HELPER(_1, _2, ...) \
    _1 _2                                                             \
    __VA_OPT__(                                                       \
        , C2CILK_OBSTRUCT(C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS_HELPER)  \
            ()(__VA_ARGS__)                                           \
    )

/**
 * Takes variadic argument of the form:
 *   `type1, name1, type2, name2, ..., typeN, nameN `
 * And transforms it into the form:
 *   `name1, name2, ..., nameN`
 *
 * @param ...: List of variable types and names, separated by commas.
 */
#define C2CILK_MACRO_PARAMS_TO_FUNC_ARGS(...)                               \
    C2CILK_EXPAND_PARAMS(                                                   \
            __VA_OPT__(                                                     \
                C2CILK_EXPAND_PARAMS(                                       \
                    C2CILK_MACRO_PARAMS_TO_FUNC_ARGS_HELPER() (__VA_ARGS__) \
                )                                                           \
            )                                                               \
    )

#define C2CILK_MACRO_PARAMS_TO_FUNC_ARGS_HELPER() \
    C2CILK_MACRO_PARAMS_TO_FUNC_ARGS_HELPER_HELPER

#define C2CILK_MACRO_PARAMS_TO_FUNC_ARGS_HELPER_HELPER(_1, _2, ...)    \
    _2                                                                 \
    __VA_OPT__(                                                        \
            , C2CILK_OBSTRUCT(C2CILK_MACRO_PARAMS_TO_FUNC_ARGS_HELPER) \
            ()(__VA_ARGS__)                                            \
    )

#ifdef __cplusplus

// The c++ standard states a minimum cap of 128 parameters to functions; this
// is already ridiculous, so go with that limit
#define C2CILK_EXPAND_PARAMS C2CILK_EXPAND_128

#else

// The c standard states a minimum cap of 64 parameters to functions; this
// is already ridiculous, so go with that limit
#define C2CILK_EXPAND_PARAMS C2CILK_EXPAND_64

#endif
