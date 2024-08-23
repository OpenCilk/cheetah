#pragma once

#include <stdbool.h>

#include <c2cilk/internal/backend.h>

/**
 * Spawns a function, ignoring any returned value.
 * Must be used within a cilkified region (e.g., c2cilk_context).
 *
 * @param func_name: Name of the function to spawn
 * @param       ...: Arguments to be passed to func_name
 */
#define c2cilk_void_spawn(func_name, ...) \
    __c2cilk_void_spawn_impl(func_name, __VA_ARGS__)

/**
 * Spawns a function, storing the returned result into the passed in pointer.
 * Must be used within a cilkified region (e.g., c2cilk_context).
 *
 * result_ptr: Where to store the result of the spawned function
 *
 * @param func_name: Which function to spawn
 * @param       ...: The arguments to be passed to func_name.
 */
#define c2cilk_spawn(result_ptr, func_name, ...) \
    __c2cilk_spawn_impl(result_ptr, func_name, __VA_ARGS__)

/**
 * Waits for all prior spawns in the current cilk function (or context to
 * complete.
 * Must be used within a cilkified region (e.g., c2cilk_context).
 */
#define c2cilk_sync \
    __c2cilk_sync_impl

/**
 * Places the enclosed code into a cilkified region.
 * c2cilk_contexts cannot be nested within the same function scope.
 * Cannot be used at global scope.
 *
 * @param ...: Lines of code to be placed in the cilkified region.
 */
#define c2cilk_context(...) \
    __c2cilk_context_impl(__VA_ARGS__)

/**
 * Makes it possible to spawn a function where the return is void or, when not
 * void, where we choose to ignore the return value. c2cilk_enable_void_spawn
 * must not be used for a function that has had its spawns enabled via
 * c2cilk_enable_spawn, as that macro also enables this type of spawn.
 *
 * Must be used in global scope.
 *
 * @param    ret_type: The return type of the function we want to be able to
 *                     spawn.
 * @param   func_name: The name of the function we want to be able to spawn.
 * @param func_params: The list of parameters to the function we want to spawn,
 *                     in the form:
 *                       `(arg_type1, arg_name1, arg_type2, arg_name2, ...)`
 */
#define c2cilk_enable_void_spawn(ret_type, func_name, func_params) \
    __c2cilk_enable_void_spawn_impl(ret_type, func_name, func_params)

/**
 * Makes it possible to spawn a function that returns a value and where we
 * choose not to ignore the return value. This macro also enables spawns where
 * we choose to ignore the return value, so should not be used in conjunction
 * with c2cilk_enable_void_spawn for the same function.
 *
 * Must be used in global scope.
 *
 * @param    ret_type: The return type of the function we want to be able to
 *                     spawn.
 * @param   func_name: The name of the function we want to be able to spawn.
 * @param func_params: The list of parameters to the function we want to spawn,
 *                     in the form:
 *                       `(arg_type1, arg_name1, arg_type2, arg_name2, ...)`
 */ 
#define c2cilk_enable_spawn(ret_type, func_name, func_params) \
    __c2cilk_enable_spawn_impl(ret_type, func_name, func_params)

/**
 * Implements a parallel for loop over all integers in the range [begin, end)
 * and executes a function. The iterations are recursively split in half and
 * spawned. The granularity defines the cut off for recursively splitting, such
 * that if the number of iterations  are <= granularity then it is no longer
 * split, and those iterations are run sequentially.
 *
 * @param       begin: int - the starting iteration value
 * @param         end: int - the stopping condition (iteration < end)
 * @param granularity: int - the point at which iterations should no longer
 *                     be split and executed in parallel
 * @param        body: A function representing the body of the loop. Must be of
 *                     the form `void body_func(int, void*)`.
 * @param        args: A void pointer that will be passed in as the second
 *                     argument to the body function in each iteration.
 */
#define c2cilk_basic_for(begin, end, granularity, body, args) \
    do { \
        C2CILK_ASSERT_LIBRARY_ENABLED; \
        c2cilk_basic_for_impl(begin, end, granularity, body, args); \
    } while (false)

/**
 * Sets up and defines a void function such that it can be used to spawn other
 * functions. This implicitly performs:
 *   c2cilk_enable_void_spawn(void, func_name, func_params)
 * as a convenience because cilk functions often recursively spawn themselves.
 *
 * THIS DOES NOT NEED TO BE USED FOR MAIN, though main by default cannot (and
 * should not) be spawned.
 *
 * Such a c2cilk function can be spawned by invoking 
 * `cilk_void_spawn(func_name, ...)`, or called directly the same as any
 * function, `func_name(...)`.
 *
 * @param   func_name: The name of the cilk function being defined.
 * @param func_params: The list of parameters to the function, in the form:
 *                       `(arg_type1, arg_name1, arg_type2, arg_name2, ...)`
 * @param         ...: The statements that make up the body of the function.
 */
#define C2CILK_VOID_FUNC(func_name, func_params, ...) \
    C2CILK_VOID_FUNC_IMPL(func_name, func_params, __VA_ARGS__)

/**
 * Sets up and defines a function such that it can be used to spawn other
 * functions. This implicitly performs:
 *   c2cilk_enable_spawn(ret_type, func_name, func_params)
 * as a convenience because cilk functions often recursively spawn themselves.
 *
 * THIS DOES NOT NEED TO BE USED FOR MAIN, though main by default cannot (and
 * should not) be spawned.
 *
 * Such a c2cilk function can be spawned by invoking 
 * `cilk_void_spawn(func_name, ...)`, spawned by invoking
 * `cilk_spawn(ret_val_ptr, func_name, ...)`, or called directly the same as any
 * function, `func_name(...)`.
 *
 * @param    ret_type: The return type of the function being defined.
 * @param   func_name: The name of the cilk function being defined.
 * @param func_params: The list of parameters to the function, in the form:
 *                       `(arg_type1, arg_name1, arg_type2, arg_name2, ...)`
 * @param         ...: The statements that make up the body of the function.
 */
#define C2CILK_FUNC(ret_type, func_name, func_params, ...) \
    C2CILK_FUNC_IMPL(ret_type, func_name, func_params, __VA_ARGS__)
