#ifndef _OPADD_REDUCER_H
#define _OPADD_REDUCER_H

#ifdef __cplusplus

namespace cilk {

template <typename T> static void zero(void *v) {
    *static_cast<T *>(v) = static_cast<T>(0);
}

template <typename T> static void plus(void *l, void *r) {
    *static_cast<T *>(l) += *static_cast<T *>(r);
}

template <typename T> using opadd_reducer = T _Hyperobject(zero<T>, plus<T>);

} // namespace cilk

#endif // #ifdef __cplusplus

#endif // _OPADD_REDUCER_H
