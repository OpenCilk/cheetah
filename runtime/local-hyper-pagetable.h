#ifndef _LOCAL_HYPER_PAGETABLE_H
#define _LOCAL_HYPER_PAGETABLE_H

#include "cilk/cilk_api.h"
#include "hyperobject_base.h"
#include "rts-config.h"
#include <cassert>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <limits>
#include <sys/mman.h>
#include <type_traits>
#include <variant>

template <typename V> struct EntryTy {
    uintptr_t key = 0;
    V data;

    void reset() {
        key = 0;
        if (std::is_destructible_v<V>)
            data.~V();
    }
};

template <typename V, ssize_t Capacity> struct SmallEntrySetTy {
    using EntryTy = EntryTy<V>;
    using OccupiedTy = ssize_t;

    OccupiedTy Occupied = 0;
    EntryTy Entries[Capacity];

    static_assert(Capacity <= 8 * sizeof(Occupied));

    EntryTy *get(uintptr_t Key) {
        for (ssize_t i = Occupied - 1; i >= 0; --i) {
            if (Entries[i].key == Key)
                return &Entries[i];
        }
        return nullptr;
    }
    const EntryTy *get(uintptr_t Key) const {
        return const_cast<decltype(*this)>(this).get(Key);
    }

    bool insert(uintptr_t Key, const V &Value) {
        if (Occupied < Capacity) {
            Entries[Occupied++] = {Key, Value};
            return true;
        }
        return false;
    }

    bool remove(uintptr_t Key) {
        for (ssize_t i = Occupied - 1; i >= 0; --i) {
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

template <typename V> struct SmallEntrySetTy<V, 1> {
    using EntryTy = EntryTy<V>;
    EntryTy Entry;

    EntryTy *get(uintptr_t Key) {
        if (Entry.key == Key)
            return &Entry;
        return nullptr;
    }
    const EntryTy *get(uintptr_t Key) const {
        return const_cast<decltype(*this)>(this).get(Key);
    }

    bool insert(uintptr_t Key, const V &Value) {
        auto OldKey = Entry.key;
        if (OldKey && OldKey != Key)
            // This slot is occupied by a different entry.
            return false;

        // Insert into this slot.
        Entry = {Key, Value};
        return true;
    }

    bool remove(uintptr_t Key) {
        if (Entry.key && Entry.key == Key) {
            Entry.reset();
            return true;
        }
        fprintf(stderr, "Failed to remove %lx from %p: Entry.Key %lx\n", Key,
                this, Entry.key);
        return false;
    }
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

template <typename V, size_t LgSz, size_t RShift, size_t EntryCapacity = 8,
          uintptr_t makeKey(uintptr_t) = makeKey<RShift>>
struct TableTy : public TableSizeTy<LgSz, RShift> {
    using SizeTy = TableSizeTy<LgSz, RShift>;
    using EntryTy = EntryTy<V>;

    SmallEntrySetTy<V, EntryCapacity> Entries[SizeTy::Size];

    // Insert a value associated with an address.
    bool insert(uintptr_t Addr, const V &Value) {
        return Entries[SizeTy::toIndex(Addr)].insert(makeKey(Addr), Value);
    }

    // Remove the value associated with the given address.
    bool remove(uintptr_t Addr) {
        return Entries[SizeTy::toIndex(Addr)].remove(makeKey(Addr));
    }

    // Lookup the value associated with an address.
    V *lookup(uintptr_t Addr) {
        if (EntryTy *Entry = Entries[SizeTy::toIndex(Addr)].get(makeKey(Addr)))
            return &Entry->data;
        return nullptr;
    }

    // Get the entry at the associated address.
    EntryTy *find(uintptr_t Addr) {
        return Entries[SizeTy::toIndex(Addr)].get(makeKey(Addr));
    }
};

// Leaf tables are indexed simply by the least significant 9 bits of an address.
template <typename V> using LeafTableTy = TableTy<V, 9, 0>;

// Pages are indexed by the 14 more significant bits of the address than the
// `RShift` template parameter.
template <size_t RShift> using PageSizeTy = TableSizeTy<14, RShift>;

template <typename V, size_t RShift>
struct PageTy : public TableTy<V, PageSizeTy<RShift>::LgSize, RShift, 1,
                               makeKey<RShift, PageSizeTy<RShift>::KeyMask>> {
    using PageSizeTy = PageSizeTy<RShift>;
    using TableTy = TableTy<V, PageSizeTy::LgSize, RShift, 1,
                            makeKey<RShift, PageSizeTy::KeyMask>>;
    using SizeTy = typename TableTy::SizeTy;
    using EntryTy = EntryTy<V>;

    // List of addresses in this page that have been inserted into.  Used for
    // destroying the higher-level PageTableTy.
    std::vector<size_t> Accessed;
    // Add the given address to the list of addresses accessed in this page.
    void recordAccess(uintptr_t Addr) { Accessed.push_back(Addr); }

    // Pages can be quite large.  Use mmap and munmap to manage their physical
    // memory on demand.
    void *operator new(size_t Size) {
        // Use MAP_ANONYMOUS to guarantee the page is initialized to zero.
        return mmap(nullptr, sizeof(PageTy), PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    }
    void operator delete(void *Ptr) { munmap(Ptr, sizeof(PageTy)); }

    V &operator[](uintptr_t Addr) {
        return this->Entries[PageSizeTy::toIndex(Addr)].Entry.data;
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

  private:
    // Helper method to insert an address-value pair into an inner node.
    static void insertIntoInnerNode(InnerNodeTy *Node, uintptr_t Addr,
                                    const V &Value) {
        // NOTE: This method is defined in the header in order to ensure it is
        // properly instantiated in all recursive PageTableTy instantiations.

        // Get the subtable corresponding with Addr.
        SubTableTy *Page = (*Node)[Addr];
        if (Page == nullptr) {
            // Create and insert a new subtable.
            Page = new SubTableTy;
            Node->recordAccess(Addr);
            [[maybe_unused]] bool Result = Node->insert(Addr, Page);
            assert(Result && "Failed to add new subtable to node.");
        }
        // Insert into subtable.
        [[maybe_unused]] bool Result = Page->insert(Addr, Value);
        assert(Result && "Failed to add address to to subtable.");
    }

    // Promote a leaf table to an inner node and then insert the given value
    // associated with the given address.
    [[clang::noinline]]
    static InnerNodeTy *promoteLeafNodeAndInsert(LeafTableTy &LeafTable,
                                                 uintptr_t Addr,
                                                 const V &Value) {
        // NOTE: This method is defined in the header in order to ensure it is
        // properly instantiated in all recursive PageTableTy instantiations.

        // The leaf table could not insert the new entry.  Convert the leaf
        // table into an inner node.
        InnerNodeTy *NewNode = new InnerNodeTy;
        // Insert all entries in the leaf table into the new inner node.
        for (auto EntrySet : LeafTable.Entries) {
            for (ssize_t i = 0; i < EntrySet.Occupied; ++i) {
                auto &Entry = EntrySet[i];
                insertIntoInnerNode(NewNode, getAddrFromKey(Entry.key),
                                    Entry.data);
            }
        }

        // Insert the new entry into the new inner node.
        insertIntoInnerNode(NewNode, Addr, Value);
        return NewNode;
    }

    // Insert the given value associated with the given address into an inner
    // node.
    [[clang::noinline]]
    bool insertInnerNode(uintptr_t Addr, const V &Value) {
        // NOTE: This method is defined in the header in order to ensure it is
        // properly instantiated in all recursive PageTableTy instantiations.
        if (std::holds_alternative<InnerNodeTy *>(Table)) {
            InnerNodeTy *Node = *std::get_if<InnerNodeTy *>(&Table);
            // Insert this entry into the inner node.
            insertIntoInnerNode(Node, Addr, Value);
            return true;
        }

        return false;
    }

    // Lookup the value at the given address from an inner node.
    V *lookupInnerNode(uintptr_t Addr);
    // Get the entry for the given address from an inner node.
    EntryTy<V> *findInnerNode(uintptr_t Addr);

    // Remove the entry for the given address from an inner node.
    bool removeInnerNode(uintptr_t Addr);

  public:
    ~PageTableTy() {
        if (std::holds_alternative<InnerNodeTy *>(Table)) {
            InnerNodeTy *Node = *std::get_if<InnerNodeTy *>(&Table);
            for (size_t Addr : Node->Accessed) {
                delete (*Node)[Addr];
            }
            delete Node;
        }
    }

    // Get the value at the given address.
    V *lookup(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            return std::get_if<LeafTableTy>(&Table)->lookup(Addr);
        }
        return lookupInnerNode(Addr);
    }

    // Get the table entry at the given address.
    EntryTy<V> *find(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            return std::get_if<LeafTableTy>(&Table)->find(Addr);
        }
        return findInnerNode(Addr);
    }

    // Insert the given value at the given address.
    bool insert(uintptr_t Addr, const V &Value) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            // Try to insert into this leaf table.
            auto &LeafTable = *std::get_if<LeafTableTy>(&Table);
            if (LeafTable.insert(Addr, Value))
                return true;

            InnerNodeTy *NewNode =
                promoteLeafNodeAndInsert(LeafTable, Addr, Value);
            // Replace this table with new inner node.
            Table = NewNode;
            return true;
        }

        return insertInnerNode(Addr, Value);
    }

    // Remove the value at the given address.
    bool remove(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            auto &LeafTable = *std::get_if<LeafTableTy>(&Table);
            return LeafTable.remove(Addr);
        }

        return removeInnerNode(Addr);
    }
};

// Template instantiation to prevent infinite recursion in template expansion.
template <typename V> struct PageTableTy<V, 48> {
    // This version of a PageTableTy should simply be a leaf table.
    using LeafTableTy = LeafTableTy<V>;
    using NodeTy = LeafTableTy;

    NodeTy Table;

    // Get the value at the given address.
    V *lookup(uintptr_t Addr) { return Table.lookup(Addr); }
    // Get the table entry at the given address.
    EntryTy<V> *find(uintptr_t Addr) { return Table.find(Addr); }

    // Insert the given value at the given address.
    bool insert(uintptr_t Addr, const V &Value) {
        return Table.insert(Addr, Value);
    }

    // Remove the value at the given address.
    bool remove(uintptr_t Addr) { return Table.remove(Addr); }
};

using bucket = EntryTy<reducer_data>;

template <typename T> struct AccessedListTy {
    static_assert(std::is_integral_v<T>, "T must be an integral type");

    static T encode(T Value) { return Value * 2; }
    static T decode(T Encoded) { return Encoded / 2; }
    static bool isTombstone(T Value) { return Value & 1; }
    static T makeTombstone(T Value) { return Value | 1; }
    static T hideTombstone(T Value) { return Value & ~1; }

    // std::vector<T>::size_type NumValid = 0;
    // List of accessed locations, maintained in sorted order.
    std::vector<T> Accessed;

    using size_type = decltype(Accessed)::size_type;
    using difference_type = decltype(Accessed)::difference_type;
    using AccessedIterTy = decltype(Accessed)::iterator;
    static constexpr difference_type ScanThreshold = 8;

    AccessedListTy() : Accessed(ScanThreshold, std::numeric_limits<T>::max()) {}

    size_type size() const { return Accessed.size(); }

  private:
    // Specialized version of std::upper_bound for use in insert() method.
    AccessedIterTy upper_bound(T Value) {
        auto Pos = Accessed.begin();
        auto End = Accessed.end();
        auto Len = std::distance(Pos, End);
        while (Len > 0) {
            if (Len <= ScanThreshold) {
                for (; Pos != End; ++Pos)
                    if (*Pos >= Value)
                        break;
                return Pos;
            }

            auto Half = Len >> 1;
            auto Middle = Pos;
            std::advance(Middle, Half);
            auto Mid = *Middle;
            if (Mid == hideTombstone(Value))
                return Middle;
            if (Value < Mid) {
                Len = Half;
            } else {
                Pos = Middle;
                ++Pos;
                Len = Len - Half - 1;
            }
        }
        return Pos;
    }

    // Specialized version of std::lower_bound for use in remove() method.
    AccessedIterTy search(T Value) {
        auto Pos = Accessed.begin();
        auto End = Accessed.end();
        auto Len = std::distance(Pos, End);
        while (Len > 0) {
            if (Len <= ScanThreshold) {
                for (; Pos != End; ++Pos)
                    if (*Pos == Value)
                        break;
                return Pos;
            }

            auto Half = Len >> 1;
            auto Middle = Pos;
            std::advance(Middle, Half);
            auto Mid = *Middle;
            if (Mid == Value)
                return Middle;
            if (Mid < Value) {
                Pos = Middle;
                ++Pos;
                Len = Len - Half - 1;
            } else {
                Len = Half;
            }
        }
        return Pos;
    }

  public:
    // Insert value into accessed list.
    void insert(T Value) {
        T Encoded = encode(Value);
        // auto Pos = std::upper_bound(Accessed.begin(), Accessed.end(), Encoded);
        auto Pos = upper_bound(Encoded);
        if (Pos == Accessed.end()) {
            Accessed.emplace_back(Encoded);
            return;
        }

        if (isTombstone(*Pos)) {
            *Pos = Encoded;
        } else if (Pos != Accessed.begin() && isTombstone(*(Pos - 1))) {
            *(Pos - 1) = Encoded;
        } else {
            Accessed.insert(Pos, Encoded);
        }
        // ++NumValid;
    }

    // Remove value from accessed list.
    void remove(T Value) {
        T Encoded = encode(Value);
        // auto Pos = std::lower_bound(Accessed.begin(), Accessed.end(), Encoded);
        auto Pos = search(Encoded);
        *Pos = makeTombstone(*Pos);
        // --NumValid;

        // TODO: Determine if this method for clearing elements from accessed
        // list is worthwhile.

        // if (Accessed.size() > ScanThreshold && NumValid < Accessed.size() / 4) {
        //     std::vector<T> Resized;
        //     Resized.reserve(NumValid * 2);
        //     for (auto V : Accessed) {
        //         if (!isTombstone(V)) {
        //             Resized.emplace_back(V);
        //             Resized.emplace_back(makeTombstone(V));
        //         }
        //     }
        //     Accessed.clear();
        //     size_type NumEntries =
        //         (((NumValid * 2) + ScanThreshold - 1) / ScanThreshold) *
        //         ScanThreshold;
        //     size_type Idx = 0;
        //     for (auto V : Resized) {
        //         Accessed.emplace_back(V);
        //         ++Idx;
        //     }
        //     for (; Idx < NumEntries; ++Idx)
        //         Accessed.emplace_back(std::numeric_limits<T>::max());
        // }
    }

    // Iterator structure to support traversal through non-tombstone elements in
    // Accessed.
    template <typename value_type> struct Iterator {
        static_assert(std::is_integral_v<value_type>,
                      "value_type must be an integral type");
        using difference_type = decltype(Accessed)::difference_type;
        using AccessedIterTy = decltype(Accessed)::const_iterator;

        AccessedIterTy It;
        AccessedIterTy End;

        bool shouldSkip(AccessedIterTy &It) const {
            return It != End && isTombstone(*It);
        }

        Iterator(AccessedListTy &List)
            : It(List.Accessed.begin()), End(List.Accessed.end()) {
            while (shouldSkip(It))
                ++It;
        }

        value_type operator*() const { return decode(*It); }

        Iterator &operator++() {
            do {
                ++It;
            } while (shouldSkip(It));
            return *this;
        }
        Iterator operator++(int) {
            auto Tmp = *this;
            ++*this;
            return Tmp;
        }

        bool operator==(const Iterator &Other) const {
            assert(End == Other.End);
            return It == Other.It;
        }
    };
    static_assert(std::input_or_output_iterator<Iterator<T>>);
    static_assert(std::input_or_output_iterator<Iterator<const T>>);

    using iterator = Iterator<T>;
    using const_iterator = Iterator<const T>;

    iterator begin() { return iterator(*this); }
    iterator end() {
        auto Iter = iterator(*this);
        Iter.It = Accessed.end();
        return Iter;
    }

    const_iterator begin() const { return const_iterator(*this); }
    const_iterator end() const {
        auto Iter = const_iterator(*this);
        Iter.It = Accessed.end();
        return Iter;
    }

    const_iterator cbegin() { return const_iterator(*this); }
    const_iterator cend() {
        auto Iter = const_iterator(*this);
        Iter.It = Accessed.end();
        return Iter;
    }
};

struct hyper_table : public PageTableTy<reducer_data> {
    using PageTableTy = PageTableTy<reducer_data>;
    using V = reducer_data;

    // Sorted list of addresses of reducers inserted into this table.
    AccessedListTy<uintptr_t> Accessed;

    uint64_t size() const { return Accessed.size(); }

    bool insert(uintptr_t Addr, const V &Value) {
        if (PageTableTy::insert(Addr, Value)) {
            Accessed.insert(Addr);
            return true;
        }
        return false;
    }

    bool remove(uintptr_t Addr) {
        if (PageTableTy::remove(Addr)) {
            Accessed.remove(Addr);
            return true;
        }
        return false;
    }

    // Iterator structure to support traversing valid elements in the table.
    // Used when merging two hyper_tables.
    struct Iterator {
        using difference_type = decltype(Accessed)::difference_type;
        using value_type = EntryTy<V>;

        hyper_table &Table;
        decltype(Accessed)::const_iterator It;

        Iterator(hyper_table &Table)
            : Table(Table), It(Table.Accessed.cbegin()) {}
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
        Iter.It = Accessed.cend();
        return Iter;
    }
};

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
static inline bool insert_hyperobject(hyper_table *table,
                                      const bucket &b) noexcept {
    // fprintf(stderr, "insert_hyperobject %lx -> %p into %p\n", b.Key,
    // b.Data.view,
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
