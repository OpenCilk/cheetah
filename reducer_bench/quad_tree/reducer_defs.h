#ifndef REDUCER_DEFS_H_
#define REDUCER_DEFS_H_

#include <cilk/cilk.h>
#include <cilk/reducer.h>
#include <cilk/reducer_opadd.h>

#include "./line.h"
#include "./intersection_event_list.h"
#include "./intersection_detection.h"

//----- IntersectionEventList

// Evaluates *left = *left OPERATOR *right
void IEL_merge(IntersectionEventList * left, IntersectionEventList * right);

// Evaluates *left = *left OPERATOR *right
void IEL_list_reduce(void * key, void * left, void * right);

// Sets *value to the the identity value.
void IEL_list_identity(void* key, void* value);



typedef CILK_C_DECLARE_REDUCER(IntersectionEventList) IELReducer;

#define IEL_RED CILK_C_INIT_REDUCER(IntersectionEventList, IEL_list_reduce, IEL_list_identity, __cilkrts_hyperobject_noop_destroy, (IntersectionEventList) { .head = NULL, .tail = NULL })

//----- unsigned int

#define UINT_RED(name) CILK_C_REDUCER_OPADD(name, int, 0)

//----- Misc.

#define REGRED(name) CILK_C_REGISTER_REDUCER(name)

#define REDVAL(name) REDUCER_VIEW(name)

#define KILLRED(name) CILK_C_UNREGISTER_REDUCER(name)

#endif
