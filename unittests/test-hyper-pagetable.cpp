// #include "test-hypertable-common.h"
#include "../runtime/hyperobject_base.h"
#include "../runtime/local-hyper-pagetable.h"
#include <cassert>
#include <iostream>

namespace cilk {
template <typename T> static void zero(void *v) {
    *static_cast<T *>(v) = static_cast<T>(0);
}
template <typename T> static void plus(void *l, void *r) {
    *static_cast<T *>(l) += *static_cast<T *>(r);
}
} // namespace cilk

// using HyperPageTableTy = PageTableTy<reducer_data>;

int main(int argc, char *argv[]) {
    reducer_data Value{nullptr, cilk::plus<int>};
    // HyperPageTableTy T;
    hyper_table T;
    // PageTableTy<uintptr_t> T;

    for (uintptr_t i = 0; i < 640; i += 64) {
        // T.insert(i, getValueFor<uintptr_t>(i));
        assert(T.insert(i, Value));
        assert(T.insert(i + ((uintptr_t)1 << 27), Value));
    }

    bool BadEntry = false;
    for (uintptr_t i = 0; i < 640; i += 64) {
        auto *RD = T.lookup(i);
        if (!RD) {
            fprintf(stderr, "Missing entry at index %lx\n", i);
            BadEntry = true;
        } else if (RD->view != Value.view || RD->extra != Value.extra) {
            fprintf(stderr, "Incorrect entry at index %lx\n", i);
            BadEntry = true;
        }
        // } else if (*RD != Value) {
        //     std::cerr << "Incorrect entry at index " << i << ": Expected " <<
        //     getValueFor<uintptr_t>(i) << ", found " << *RD << "\n";
        // }
        // if (RD) {
        //     std::cout << "T[" << i << "] = " << *RD << "\n";
        // } else {
        //     std::cout << "T[" << i << "] is empty\n";
        // }
    }
    for (uintptr_t ii = 0; ii < 640; ii += 64) {
        uintptr_t i = ii + ((uintptr_t)1 << 27);
        auto *RD = T.lookup(i);
        if (!RD) {
            fprintf(stderr, "Missing entry at index %lx\n", i);
            BadEntry = true;
        } else if (RD->view != Value.view || RD->extra != Value.extra) {
            fprintf(stderr, "Incorrect entry at index %lx\n", i);
            BadEntry = true;
        }
        // } else if (*RD != Value) {
        //     std::cerr << "Incorrect entry at index " << i << ": Expected " <<
        //     getValueFor<uintptr_t>(i) << ", found " << *RD << "\n";
        // }
        // if (RD) {
        //     std::cout << "T[" << i << "] = " << *RD << "\n";
        // } else {
        //     std::cout << "T[" << i << "] is empty\n";
        // }
    }
    std::cout << (BadEntry ? "Test failed" : "Test passed") << "\n";

    return 0;
}