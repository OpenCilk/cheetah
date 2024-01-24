#pragma once

/**
 * The type of function that can be used as the body for a c2cilk_basic_for.
 * The first argument is the integer iteration number, and the second is
 * a pointer to loop specific arguments.
 */
typedef void (*c2cilk_basic_for_body_fn_t)(int, void *);

#define __c2cilk_func_prototype(ret_type, func_name, func_params)              \
    ret_type func_name                                                         \
        (C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS(C2CILK_STRIP_PARENS(func_params)))

/**
 * The spawn helpers are named in such a way that the error messages are
 * informative.
 */
#define __c2cilk_spawn_helper_name(func_name) \
    func_name ## _must_be_enabled_for_spawn_using_c2cilk_enable_spawn

#define __c2cilk_void_spawn_helper_name(func_name) \
    func_name ## _must_be_enabled_for_void_spawn_using_c2cilk_enable_void_spawn
