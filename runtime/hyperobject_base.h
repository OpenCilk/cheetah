#ifndef _HYPEROBJECT_BASE
#define _HYPEROBJECT_BASE

#include <cilk/cilk_api.h> // __cilk_reduce_fn

// Reducer data.
//
// NOTE: Since the size and identity_fn are only used when a worker
// looks up a reducer after a steal, we don't need to store these in
// the reducer_base structure as long as the reducer_lookup function
// gets them as parameters.
//
// TODO: For small reducer views of size less than sizeof(void *),
// consider storing the view directly within the reducer_base
// structure.
// - Problem: A reducer_base structure may move around in the hash
//   table as other reducers are inserted.  As a result, a pointer to
//   a view may be invalidated by other hyper_lookup operations.
// - Problem: Need a way to keep track of whether the view in a
//   reducer_base is storing a pointer to the view or the view itself.
typedef struct reducer_base {
    void *view;
    __cilk_reduce_fn reduce_fn;
} reducer_base;

#endif /* _HYPEROBJECT_BASE */
