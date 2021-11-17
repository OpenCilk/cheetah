#ifndef _HOLDER_H
#define _HOLDER_H

#ifdef __cplusplus

#include <type_traits>

namespace cilk {

template <typename A> static void init(void *view) {
    new(view) A;
}
template <typename A> static void reduce(void *left, void *right) {
    if (std::is_destructible<A>::value)
        static_cast<A *>(right)->~A();
}

template <typename A>
using holder = A _Hyperobject(init<A>, reduce<A>);

} // namespace cilk

#endif // __cplusplus

#endif // _HOLDER_H
