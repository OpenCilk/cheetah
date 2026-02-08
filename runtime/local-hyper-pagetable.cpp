#include "local-hyper-pagetable.h"
#include "cilk/cilk_api.h"
#include "cilk/reducer"
#include "internal-malloc.h"
#include <cassert>

///////////////////////////////////////////////////////////////////////////
// Implementations of methods from PageTableTy.

template <typename V, size_t Bits>
V *PageTableTy<V, Bits>::lookupInnerNode(uintptr_t Addr) {
    if (auto *Page = (*std::get_if<InnerNodeTy *>(&Table))->lookup(Addr)) {
        return (*Page)->lookup(Addr);
    }
    return nullptr;
}

template <typename V, size_t Bits>
EntryTy<V> *PageTableTy<V, Bits>::findInnerNode(uintptr_t Addr) {
    if (auto *Page = (*std::get_if<InnerNodeTy *>(&Table))->lookup(Addr)) {
        return (*Page)->find(Addr);
    }
    return nullptr;
}

template <typename V, size_t Bits>
bool PageTableTy<V, Bits>::removeInnerNode(uintptr_t Addr) {
    if (std::holds_alternative<InnerNodeTy *>(Table)) {
        InnerNodeTy *InnerNode = *std::get_if<InnerNodeTy *>(&Table);
        if (auto *Page = InnerNode->lookup(Addr)) {
            return (*Page)->remove(Addr);
        }
    }

    return false;
}

// Ensure the PageTableTy is fully instantiated for hyper_table.
template struct PageTableTy<reducer_data>;

///////////////////////////////////////////////////////////////////////////
// Query, insert, and delete methods for the hash table.

hyper_table *__cilkrts_local_hyper_table_alloc(void) {
    auto *Tmp = new hyper_table();
    // fprintf(stderr, "cilkrts_local_hyper_table_alloc -> %p\n", Tmp);
    return Tmp;
}

__reducer_base *__cilkrts_insert_new_view_0(hyper_table *table,
                                            __reducer_base *key) {
    // Create a new view and initialize it with the identity function.
    size_t size = key->size();
    void *new_view = cilk_aligned_alloc(64, round_size_to_alignment(64, size));
    __reducer_base *base = key->identity(new_view);

    // Insert the new view into the local hypertable.
    [[maybe_unused]] bool success =
        table->insert((uintptr_t)key, {new_view, base});
    assert(success && "Failed to insert reducer data");
    return base;
}

void *__cilkrts_insert_new_view_1(hyper_table *table, uintptr_t key,
                                  const __reducer_callbacks &callbacks) {
    // Create a new view and initialize it with the identity function.
    void *new_view =
        cilk_aligned_alloc(64, round_size_to_alignment(64, callbacks.size));
    callbacks.identity(new_view);

    // Insert the new view into the local hypertable.
    [[maybe_unused]] bool success =
        table->insert((uintptr_t)key, {new_view, &callbacks.reduce});
    assert(success && "Failed to insert reducer data");
    return new_view;
}

void *__cilkrts_insert_new_view_2(hyper_table *table, uintptr_t key,
                                  size_t size, __cilk_c_identity_fn *identity,
                                  __cilk_c_reduce_fn *reduce) {
    // Create a new view and initialize it with the identity function.
    void *new_view = cilk_aligned_alloc(64, round_size_to_alignment(64, size));
    identity(new_view);

    // Insert the new view into the local hypertable.
    [[maybe_unused]] bool success =
        table->insert((uintptr_t)key, {new_view, reduce});
    // fprintf(stderr, "cilkrts_insert_new_view_2: inserted %lx -> %p into
    // %p\n",
    //         key, new_view, table);
    assert(success && "Failed to insert reducer data");
    return new_view;
}

void bucket_reduce(bucket *Left, bucket *Right) {
    assert(Left->data.extra.index() == Right->data.extra.index());
    void *LeftView = Left->data.view, *RightView = Right->data.view;
    if (std::holds_alternative<__reducer_base *>(Left->data.extra)) {
        __reducer_base *Leftmost = static_cast<__reducer_base *>(
            reinterpret_cast<void *>(getAddrFromKey(Left->key)));
        __reducer_base *LeftR = std::get<__reducer_base *>(Left->data.extra);
        __reducer_base *RightR = std::get<__reducer_base *>(Right->data.extra);
        Leftmost->reduce(LeftR, RightR);
        RightR->~__reducer_base();
    } else if (std::holds_alternative<const __cilk_reduce_fn *>(
                   Left->data.extra)) {
        (*std::get<const __cilk_reduce_fn *>(Left->data.extra))(LeftView,
                                                                RightView);
    } else {
        // fprintf(stderr, "bucket_reduce %p, %p\n", LeftView, RightView);
        std::get<__cilk_c_reduce_fn *>(Left->data.extra)(LeftView, RightView);
    }
    Right->data.extra = (__reducer_base *)nullptr;
    Right->data.view = nullptr;
    free(RightView);
}

// Merge two hypertables, left and right.  Returns the merged hypertable and
// deletes the other.
hyper_table *merge_two_hts(hyper_table *__restrict Left,
                           hyper_table *__restrict Right) {
    // fprintf(stderr, "merge_two_hts %p (%lld), %p (%lld)\n", Left,
    // Left->size(),
    //         Right, Right->size());
    // In the trivial case of an empty hyper_table, return the other
    // hyper_table.
    if (!Left)
        return Right;
    if (!Right)
        return Left;
    if (Left->size() == 0) {
        delete Left;
        return Right;
    }
    if (Right->size() == 0) {
        delete Right;
        return Left;
    }

    // Pick the smaller hyper_table to be the source, which we will iterate
    // over.
    bool LeftDst;
    hyper_table *Src, *Dst;
    if (Left->size() >= Right->size()) {
        Src = Right;
        Dst = Left;
        LeftDst = true;
    } else {
        Src = Left;
        Dst = Right;
        LeftDst = false;
    }

    for (bucket B : *Src) {
        uintptr_t Addr = getAddrFromKey(B.key);
        bucket *DstB = Dst->find(Addr);
        if (DstB == nullptr) {
            // fprintf(stderr, "merge_two_hts: inserting %lx -> %p into %p\n",
            //         Addr, B.Data.view, Dst);
            Dst->insert(Addr, B.data);
        } else {
            if (LeftDst) {
                // fprintf(stderr, "merge_two_hts: reduction (%d): %p -> %p and
                // %p -> %p\n",
                //         LeftDst, Dst, DstB->Data.view, Src, B.Data.view);
                bucket_reduce(DstB, &B);
            } else {
                // fprintf(stderr, "merge_two_hts: reduction (%d): %p -> %p and
                // %p -> %p\n",
                //         LeftDst, Src, B.Data.view, Dst, DstB->Data.view);
                bucket_reduce(&B, DstB);
                DstB->data = B.data;
                B.data.extra = (__reducer_base *)nullptr;
                B.data.view = nullptr;
            }
        }
    }

    // Destroy the source hyper_table, and return the destination.
    delete Src;

    return Dst;
}