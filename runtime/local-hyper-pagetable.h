#ifndef _LOCAL_HYPER_PAGETABLE_H
#define _LOCAL_HYPER_PAGETABLE_H

#include "cilk/cilk_api.h"
#include "hyperobject_base.h"
#include "rts-config.h"
#include <cassert>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <set>
#include <sys/mman.h>
#include <type_traits>
#include <variant>

template <typename V> struct EntryTy {
    uintptr_t key = 0;
    V data;

    void reset() {
        key = 0;
        if (std::is_destructible<V>::value)
            data.~V();
    }
    bool isValid() const { return key != 0; }
};

template <size_t LgSz, size_t RShift> struct TableSizeTy {
    static_assert(LgSz <= 64);
    static_assert(RShift < 64);
    static constexpr size_t LgSize =
        (LgSz + RShift < 48) ? LgSz : (48 - RShift);
    static constexpr size_t Size = (size_t)1 << LgSize;
    static constexpr uintptr_t AddrMask = Size - 1;
    static constexpr size_t Bits = LgSize + RShift;
    static constexpr size_t KeyMask = ((size_t)1 << Bits) - 1;
    static constexpr size_t toIndex(uintptr_t Addr) {
        return (Addr >> RShift) & AddrMask;
    }
};

template <size_t RShift, uintptr_t Mask = (uintptr_t)-1>
static inline uintptr_t makeKey(uintptr_t Addr) {
    return ~((Addr >> RShift) << RShift) & Mask;
}

static inline uintptr_t getAddrFromKey(uintptr_t Key) { return ~Key; }

template <typename V, size_t LgSz, size_t RShift,
          uintptr_t makeKey(uintptr_t) = makeKey<RShift>>
struct TableTy : public TableSizeTy<LgSz, RShift> {
    using SizeTy = TableSizeTy<LgSz, RShift>;
    using EntryTy = EntryTy<V>;

    struct SmallEntrySetTy {
        static constexpr size_t Capacity = 4;
        using OccupiedTy = uint8_t;
        OccupiedTy Occupied = 0;

        EntryTy Entries[Capacity];

        static_assert(Capacity <= 8 * sizeof(Occupied));

        EntryTy *get(uintptr_t Key) {
            for (size_t i = 0; i < Occupied; ++i)
                if (Entries[i].key == Key)
                    return &Entries[i];
            return nullptr;
        }
        const EntryTy *get(uintptr_t Key) const {
            return const_cast<decltype(*this)>(this).get(Key);
        }

        bool insert(uintptr_t Key, V Value) {
            if (Occupied < Capacity) {
                Entries[Occupied++] = {Key, Value};
                return true;
            }
            return false;
        }

        bool remove(uintptr_t Key) {
            for (size_t i = 0; i < Occupied; ++i) {
                if (Entries[i].key == Key) {
                    Entries[i].reset();
                    if (i != Occupied - 1)
                        Entries[i] = Entries[Occupied - 1];
                    --Occupied;
                    return true;
                }
            }
            return false;
        }

        EntryTy &operator[](size_t Idx) { return Entries[Idx]; }
    };

    EntryTy Entries[SizeTy::Size];
    // SmallEntrySetTy Entries[SizeTy::Size];

    // Insert a value associated with an address into this table.
    bool insert(uintptr_t Addr, V Value) {
        auto NewKey = makeKey(Addr);
        auto Key = Entries[SizeTy::toIndex(Addr)].key;
        if (Key && Key != NewKey)
            // This slot is occupied by a different entry.
            return false;

        // Insert into this slot.
        Entries[SizeTy::toIndex(Addr)] = {NewKey, Value};
        return true;
    }

    bool remove(uintptr_t Addr) {
        auto Entry = Entries[SizeTy::toIndex(Addr)];
        if (Entry.key && Entry.key == makeKey(Addr)) {
            Entry.reset();
            return true;
        }
        fprintf(stderr, "Failed to remove %lx from %p: Entry.Key %lx\n", Addr,
                this, Entry.key);
        return false;
    }

    // Lookup the value associated with an address.
    V *lookup(uintptr_t Addr) {
        // fprintf(stderr, "TableTy<%ld, %ld>::lookup %lx (%lx) -> %lx vs %lx\n",
        //         LgSz, RShift, Addr, SizeTy::toIndex(Addr),
        //         Entries[SizeTy::toIndex(Addr)].Key, makeKey(Addr));
        if (Entries[SizeTy::toIndex(Addr)].key == makeKey(Addr)) {
            return &Entries[SizeTy::toIndex(Addr)].data;
        }
        return nullptr;
    }

    EntryTy *find(uintptr_t Addr) {
        if (Entries[SizeTy::toIndex(Addr)].key == makeKey(Addr)) {
            return &Entries[SizeTy::toIndex(Addr)];
        }
        return nullptr;
    }

    // bool insert(uintptr_t Addr, V Value) {
    //     return Entries[SizeTy::toIndex(Addr)].insert(makeKey(Addr), Value);
    // }
    // bool remove(uintptr_t Addr) {
    //     return Entries[SizeTy::toIndex(Addr)].remove(makeKey(Addr));
    // }

    // V *lookup(uintptr_t Addr) {
    //     if (EntryTy *Entry = Entries[SizeTy::toIndex(Addr)].get(makeKey(Addr)))
    //         return &Entry->data;
    //     return nullptr;
    // }

    // EntryTy *find(uintptr_t Addr) {
    //     return Entries[SizeTy::toIndex(Addr)].get(makeKey(Addr));
    // }
};

// Leaf tables are indexed simply by the least significant 9 bits of an address.
template <typename V> using LeafTableTy = TableTy<V, 9, 0>;

// template <typename V> struct LeafTableTy : public TableTy<V, 9, 0> {
//     using TableTy = TableTy<V, 9, 0>;
//     using SizeTy = typename TableTy::SizeTy;
//     uint64_t Accessed = 0;

//     bool insert(uintptr_t Addr, V Value) {
//         if (TableTy::insert(Addr, Value)) {
//             Accessed |= (uint64_t)1 << SizeTy::toIndex(Addr);
//             return true;
//         }
//         return false;
//     }

//     bool remove(uintptr_t Addr) {
//         if (TableTy::remove(Addr)) {
//             Accessed &= ~((uint64_t)1 << SizeTy::toIndex(Addr));
//             return true;
//         }
//         return false;
//     }

//     struct Iterator {
//         using difference_type = std::ptrdiff_t;
//         using value_type = EntryTy<V>;

//         LeafTableTy &Table;
//         uint64_t It = 0;

//         static constexpr uint64_t getNextAccessed(uint64_t A, uint64_t X) {
//             return A ^ X;
//         }

//         Iterator(LeafTableTy &Table) : Table(Table) {}
//         Iterator &operator=(const Iterator &Other) {
//             Table = Other.Table;
//             It = Other.It;
//             return *this;
//         }

//         value_type &operator*() const {
//             return Table
//                 .Entries[__builtin_ctzll(getNextAccessed(Table.Accessed, It))];
//         }

//         Iterator &operator++() {
//             auto Tmp = getNextAccessed(Table.Accessed, It);
//             It |= (Tmp & -Tmp);
//             return *this;
//         }
//         Iterator operator++(int) {
//             auto Tmp = *this;
//             ++*this;
//             return Tmp;
//         }

//         bool operator==(const Iterator &Other) const {
//             assert(Table == Other.Table);
//             return It == Other.It;
//         }
//     };
//     static_assert(std::input_or_output_iterator<Iterator>);

//     Iterator begin() { return Iterator(*this); }
//     Iterator end() {
//         auto Iter = Iterator(*this);
//         Iter.It = Accessed;
//         return Iter;
//     }
// };

// Pages are indexed by the 14 more significant bits of the address than the
// `RShift` template parameter.
template <size_t RShift> using PageSizeTy = TableSizeTy<14, RShift>;

template <typename V, size_t RShift>
struct PageTy : public TableTy<V, PageSizeTy<RShift>::LgSize, RShift,
                               makeKey<RShift, PageSizeTy<RShift>::KeyMask>> {
    using PageSizeTy = PageSizeTy<RShift>;
    using TableTy = TableTy<V, PageSizeTy::LgSize, RShift>;
    using SizeTy = typename TableTy::SizeTy;
    using EntryTy = EntryTy<V>;

    // Pages can be quite large.  Use mmap to map their storage on demand.
    void *operator new(size_t Size) {
        return mmap(nullptr, sizeof(PageTy), PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    }
    void operator delete(void *Ptr) { munmap(Ptr, sizeof(PageTy)); }

    V &operator[](uintptr_t Addr) {
        return this->Entries[PageSizeTy::toIndex(Addr)].data;
        // return this->Entries[PageSizeTy::toIndex(Addr)].get(Addr)->data;
    }
    const V &operator[](uintptr_t Addr) const {
        return const_cast<decltype(*this)>(this)[Addr];
    }
};

// A page table is a dynamic tree structure, where each node is either a leaf
// table or a page of page tables.
template <typename V, size_t Bits = LeafTableTy<V>::Bits> struct PageTableTy {
    static_assert(Bits < 48);
    using LeafTableTy = LeafTableTy<V>;
    // Subtables are page tables indexed by more significant bits of the
    // address.
    using SubTableTy = PageTableTy<V, PageSizeTy<Bits>::Bits>;
    // An inner node is a page of pointers to page tables indexed using more
    // significant bits than the `Bits` parameter.
    using InnerNodeTy = PageTy<SubTableTy *, Bits>;
    using InnerNodeSizeTy = typename InnerNodeTy::PageSizeTy;
    // The table is either a leaf table or a pointer to an inner node.
    using NodeTy = std::variant<LeafTableTy, InnerNodeTy *>;
    NodeTy Table;

    V *lookup(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            // fprintf(stderr, "PageTableTy::lookup %lx LeafTableTy\n", Addr);
            return std::get_if<LeafTableTy>(&Table)->lookup(Addr);
        }
        // fprintf(stderr, "PageTableTy::lookup %lx InnerNodeTy\n", Addr);
        if (auto *Page = (*std::get_if<InnerNodeTy *>(&Table))->lookup(Addr)) {
            // fprintf(stderr,
            //         "PageTableTy::lookup %lx InnerNodeTy, Subtable %p\n", Addr,
            //         *Page);
            return (*Page)->lookup(Addr);
        }
        // fprintf(stderr, "PageTableTy::lookup %lx InnerNodeTy, No page found\n",
        //         Addr);
        return nullptr;
    }

    EntryTy<V> *find(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            return std::get_if<LeafTableTy>(&Table)->find(Addr);
        }
        if (auto *Page = (*std::get_if<InnerNodeTy *>(&Table))->lookup(Addr)) {
            return (*Page)->find(Addr);
        }
        return nullptr;
    }

    // Helper method to insert into an inner node.
    static void insertIntoInnerNode(InnerNodeTy *Node, uintptr_t Addr,
                                    V Value) {
        // Get the subtable corresponding with Addr.
        SubTableTy *Page = (*Node)[Addr];
        if (Page == nullptr) {
            // Create and insert a new subtable.
            Page = new SubTableTy;
            // fprintf(stderr,
            //         "PageTableTy::insertIntoInnerNode: inserting new SubTable "
            //         "%p into Node %p for Addr %lx\n",
            //         Page, Node, Addr);
            [[maybe_unused]]bool Result = Node->insert(Addr, Page);
            assert(Result && "Failed to add new subtable to node.");
        }
        // fprintf(stderr,
        //         "PageTableTy::insertIntoInnerNode: inserting into SubTable "
        //         "%p in Node %p, Addr %lx\n",
        //         Page, Node, Addr);
        // Insert into subtable.
        [[maybe_unused]]bool Result = Page->insert(Addr, Value);
        assert(Result && "Failed to add address to to subtable.");
    }

    bool insert(uintptr_t Addr, V Value) {
        // std::cout << "PageTableTy<" << Bits << ">: my size: " <<
        // sizeof(*this) << "\n";
        // fprintf(stderr, "PageTableTy::insert %lx into %p\n", Addr, this);
        if (std::holds_alternative<LeafTableTy>(Table)) {
            // fprintf(stderr, "PageTableTy::insert %lx into %p LeafNodeTy variant\n", Addr, this);
            // Try to insert into this leaf table.
            auto &LeafTable = *std::get_if<LeafTableTy>(&Table);
            if (LeafTable.insert(Addr, Value)) {
                return true;
            }

            // The leaf table could not insert the new entry.  Convert the leaf
            // table into an inner node.
            InnerNodeTy *NewNode = new InnerNodeTy;
            // fprintf(stderr,
            //         "PageTableTy::insert %p: Replacing LeafNodeTy with "
            //         "InnerNodeTy %p\n",
            //         this, NewNode);
            // Insert all entries in the leaf table into the new inner node.
            // for (auto EntrySet : LeafTable.Entries) {
            //     fprintf(stderr, "inserting %d entries into inner node\n", EntrySet.Occupied);
            //     for (size_t i = 0; i < EntrySet.Occupied; ++i) {
            //         fprintf(stderr, "inserting entry[%ld], key = %lx\n", i, EntrySet[i].key);
            //         auto &Entry = EntrySet[i];
            //         fprintf(stderr, "inserting entry[%ld], key = %lx, data %p\n", i,
            //                 Entry.key, &Entry.data);
            //         insertIntoInnerNode(NewNode, getAddrFromKey(Entry.key),
            //                             Entry.data);
            //     }
            // }
            for (auto Entry : LeafTable.Entries) {
                if (Entry.isValid()) {
                    // fprintf(stderr,
                    //         "PageTableTy::insert %lx into %p new InnerNode\n",
                    //         LeafTableTy::getAddrFromKey(Entry.Key), this);
                    insertIntoInnerNode(NewNode,
                                        getAddrFromKey(Entry.key),
                                        Entry.data);
                }
            }

            // fprintf(stderr,
            //         "PageTableTy::insert %lx into %p new InnerNodeTy\n",
            //         Addr, this);
            // Insert the new entry into the new inner node.
            insertIntoInnerNode(NewNode, Addr, Value);
            // Replace this table with new inner node.
            Table = NewNode;
            return true;
        }

        if (std::holds_alternative<InnerNodeTy *>(Table)) {
            // fprintf(stderr, "PageTableTy::insert %lx into %p InnerNodeTy variant\n",
            //         Addr, this);
            InnerNodeTy *Node = *std::get_if<InnerNodeTy *>(&Table);
            // Insert this entry into the inner node.
            insertIntoInnerNode(Node, Addr, Value);
            return true;
        }

        return false;
    }

    bool remove(uintptr_t Addr) {
        // fprintf(stderr, "PageTableTy::remove %lx from %p\n", Addr, this);
        if (std::holds_alternative<LeafTableTy>(Table)) {
            // fprintf(stderr,
            //         "PageTableTy::remove %lx from %p, LeafTableTy variant\n",
            //         Addr, this);
            auto &LeafTable = *std::get_if<LeafTableTy>(&Table);
            return LeafTable.remove(Addr);
        }

        if (std::holds_alternative<InnerNodeTy *>(Table)) {
            // fprintf(stderr, "PageTableTy::remove %lx from %p, InnerNodeTy variant\n",
            //         Addr, this);
            if (auto *Page =
                    (*std::get_if<InnerNodeTy *>(&Table))->lookup(Addr)) {
                return (*Page)->remove(Addr);
            // } else {
            //     fprintf(stderr,
            //             "PageTableTy::remove %lx from %p, InnerNodeTy variant, no Page "
            //             "found\n",
            //             Addr, this);
            }
        }

        return false;
    }
};

// Template instantiation to prevent infinite recursion in template expansion.
template <typename V> struct PageTableTy<V, 48> {
    // This version of a PageTableTy should simply be a leaf table.
    using LeafTableTy = LeafTableTy<V>;
    using NodeTy = LeafTableTy;

    NodeTy Table;

    V *lookup(uintptr_t Addr) { return Table.lookup(Addr); }
    EntryTy<V> *find(uintptr_t Addr) { return Table.find(Addr); }

    bool insert(uintptr_t Addr, V Value) { return Table.insert(Addr, Value); }

    bool remove(uintptr_t Addr) { return Table.remove(Addr); }
};

using bucket = EntryTy<reducer_data>;

struct hyper_table : public PageTableTy<reducer_data> {
    using PageTableTy = PageTableTy<reducer_data>;
    using V = reducer_data;

    std::set<size_t> Accessed;

    uint64_t size() const { return Accessed.size(); }

    bool insert(uintptr_t Addr, V Value) {
        if (PageTableTy::insert(Addr, Value)) {
            Accessed.insert(Addr);
            return true;
        }
        return false;
    }

    bool remove(uintptr_t Addr) {
        if (PageTableTy::remove(Addr)) {
            Accessed.erase(Addr);
            return true;
        }
        return false;
    }

    struct Iterator {
        using difference_type = std::ptrdiff_t;
        using value_type = EntryTy<V>;

        hyper_table &Table;
        std::set<size_t>::const_iterator It;

        Iterator(hyper_table &Table)
            : Table(Table), It(Table.Accessed.begin()) {}
        Iterator(const Iterator &Other) : Table(Other.Table), It(Other.It) {}
        Iterator &operator=(const Iterator &Other) {
            Table = Other.Table;
            It = Other.It;
            return *this;
        }

        value_type &operator*() const { return *Table.find(*It); }

        Iterator &operator++() {
            ++It;
            return *this;
        }
        Iterator operator++(int) {
            auto Tmp = *this;
            ++*this;
            return Tmp;
        }

        bool operator==(const Iterator &Other) const {
            assert(&Table == &Other.Table);
            return It == Other.It;
        }
    };
    static_assert(std::input_or_output_iterator<Iterator>);

    Iterator begin() { return Iterator(*this); }
    Iterator end() {
        auto Iter = Iterator(*this);
        Iter.It = Accessed.end();
        return Iter;
    }
};

template struct PageTableTy<reducer_data, 23>;

CHEETAH_API
hyper_table *__cilkrts_local_hyper_table_alloc(void);

static inline bucket *find_hyperobject(hyper_table *table,
                                       uintptr_t key) noexcept {
    // fprintf(stderr, "find_hyperobject: %p, %lx\n", table, key);
    auto *Tmp = table->find(key);
    // fprintf(stderr, "find_hyperobject: %p (%lld), %lx -> %p\n", table,
    //         table->size(), key, (Tmp ? Tmp->Data.view : Tmp));
    return Tmp;
}

CHEETAH_INTERNAL
static inline bool remove_hyperobject(hyper_table *table,
                                      uintptr_t key) noexcept {
    auto Tmp = table->remove(key);
    // fprintf(stderr, "remove_hyperobject: %p (%lld), %lx\n", table,
    //         table->size(), key);
    return Tmp;
}

CHEETAH_INTERNAL
static inline bool insert_hyperobject(hyper_table *table, bucket b) noexcept {
    // fprintf(stderr, "insert_hyperobject %lx -> %p into %p\n", b.Key, b.Data.view,
    //         table);
    return table->insert(b.key, b.data);
}

CHEETAH_API
bucket *__cilkrts_find_hyperobject_hash(hyper_table *table, uintptr_t key);

CHEETAH_API
__reducer_base *__cilkrts_insert_new_view_0(hyper_table *table,
                                            struct __reducer_base *key)
    __attribute__((nonnull, returns_nonnull));

CHEETAH_API
void *__cilkrts_insert_new_view_1(hyper_table *table, uintptr_t key,
                                  const __reducer_callbacks &callbacks)
    __attribute__((nonnull, returns_nonnull));

CHEETAH_API
void *__cilkrts_insert_new_view_2(hyper_table *table, uintptr_t key,
                                  size_t size, __cilk_c_identity_fn identity,
                                  __cilk_c_reduce_fn reduce)
    __attribute__((nonnull, returns_nonnull));

CHEETAH_INTERNAL
hyper_table *merge_two_hts(hyper_table *__restrict left,
                           hyper_table *__restrict right);

#endif // _LOCAL_HYPER_PAGETABLE_H
