#ifndef _LOCAL_HYPER_PAGETABLE_H
#define _LOCAL_HYPER_PAGETABLE_H

#include "cilk/cilk_api.h"
#include "hyperobject_base.h"
#include "rts-config.h"
#include <cassert>
#include <cstddef>
#include <iterator>
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

    ssize_t Occupied = 0;
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

    bool insert(uintptr_t Key, V &&Value) {
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
                    Entries[i] = std::move(Entries[Occupied - 1]);
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

template <typename V, size_t LgSz, size_t RShift, size_t EntrySetCapacity,
          uintptr_t makeKey(uintptr_t) = makeKey<RShift>>
struct TableTy : public TableSizeTy<LgSz, RShift> {
    using SizeTy = TableSizeTy<LgSz, RShift>;
    using EntryTy = EntryTy<V>;

    SmallEntrySetTy<V, EntrySetCapacity> Entries[SizeTy::Size];

    // Insert a value associated with an address.
    bool insert(uintptr_t Addr, V &&Value) {
        return Entries[SizeTy::toIndex(Addr)].insert(makeKey(Addr),
                                                     std::move(Value));
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

// Leaf tables are indexed simply by the least significant 12 bits of an
// address.
static constexpr size_t LeafLgSz = 9;
static constexpr size_t LeafLgSetCapacity = 3;
static constexpr size_t LeafSetCapacity = 1 << LeafLgSetCapacity;
template <typename V>
struct LeafTableTy
    : public TableTy<V, LeafLgSz, LeafLgSetCapacity, LeafSetCapacity> {
    using TableTy = TableTy<V, LeafLgSz, LeafLgSetCapacity, LeafSetCapacity>;
    using SizeTy = TableTy::SizeTy;

    // Bit set to track which sets in this table contain elements.
    static constexpr size_t AccessedFieldSize = 8 * sizeof(uint64_t);
    static constexpr size_t AccessedSize =
        (1UL << LeafLgSz) / AccessedFieldSize;
    uint64_t Accessed[AccessedSize] = {0UL};

  private:
    static size_t getAccessedIdx(size_t Idx) { return Idx / AccessedFieldSize; }
    static uint64_t getAccessedMask(size_t Idx) {
        return 1UL << (Idx % AccessedFieldSize);
    }

  public:
    bool insert(uintptr_t Addr, V &&Value) {
        if (TableTy::insert(Addr, std::move(Value))) {
            const size_t Idx = SizeTy::toIndex(Addr);
            if (this->Entries[Idx].Occupied == 1)
                Accessed[getAccessedIdx(Idx)] |= getAccessedMask(Idx);
            return true;
        }
        return false;
    }

    bool remove(uintptr_t Addr) {
        if (TableTy::remove(Addr)) {
            const size_t Idx = SizeTy::toIndex(Addr);
            if (this->Entries[Idx].Occupied == 0)
                Accessed[getAccessedIdx(Idx)] &= ~getAccessedMask(Idx);
            return true;
        }
        return false;
    }

    // Constants and methods for iterating through the entries in this table.
    static constexpr uintptr_t EndIteratorValue =
        1UL << (LeafLgSz + LeafLgSetCapacity);

    static ssize_t getSetIdx(uintptr_t It) {
        return It & (LeafSetCapacity - 1);
    }
    static uintptr_t getEntryIdx(uintptr_t It) {
        return It >> LeafLgSetCapacity;
    }

    uintptr_t advanceToNextEntry(uintptr_t It) {
        if (this->Entries[getEntryIdx(It)].Occupied > getSetIdx(It))
            return It;

        uintptr_t NextEntryIdx = getEntryIdx(It) + 1;
        size_t AccessedIdx = LeafTableTy::getAccessedIdx(NextEntryIdx);
        uint64_t AccessedMask = LeafTableTy::getAccessedMask(NextEntryIdx);
        for (; AccessedIdx < AccessedSize; ++AccessedIdx) {
            uint64_t AccessedField =
                Accessed[AccessedIdx] & ~(AccessedMask - 1);
            if (AccessedField) {
                It = ((AccessedIdx * AccessedFieldSize) +
                      __builtin_ctzl(AccessedField))
                     << LeafLgSetCapacity;
                return It;
            }
            AccessedMask = 1;
        }
        // Return the end iterator
        It = EndIteratorValue;
        return It;
    }

    EntryTy<V> &getEntryAt(uintptr_t It) {
        return this->Entries[getEntryIdx(It)].Entries[getSetIdx(It)];
    }
};

// Pages are indexed by the 12 more significant bits of the address than the
// `RShift` template parameter.
template <size_t RShift> using PageSizeTy = TableSizeTy<12, RShift>;

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
                                    V &&Value) {
        // NOTE: This method is defined in the header in order to ensure it is
        // properly instantiated in all recursive PageTableTy instantiations.

        // Get the subtable corresponding with Addr.
        SubTableTy *Page = (*Node)[Addr];
        if (Page == nullptr) {
            // Create and insert a new subtable.
            Page = new SubTableTy;
            Node->recordAccess(Addr);
            [[maybe_unused]] bool Result = Node->insert(Addr, std::move(Page));
            assert(Result && "Failed to add new subtable to node.");
        }
        // Insert into subtable.
        [[maybe_unused]] bool Result = Page->insert(Addr, std::move(Value));
        assert(Result && "Failed to add address to to subtable.");
    }

    // Promote a leaf table to an inner node and then insert the given value
    // associated with the given address.
    [[clang::noinline]]
    static InnerNodeTy *promoteLeafNodeAndInsert(LeafTableTy &LeafTable,
                                                 uintptr_t Addr, V &&Value) {
        // NOTE: This method is defined in the header in order to ensure it is
        // properly instantiated in all recursive PageTableTy instantiations.

        // The leaf table could not insert the new entry.  Convert the leaf
        // table into an inner node.
        InnerNodeTy *NewNode = new InnerNodeTy;
        // Insert all entries in the leaf table into the new inner node.
        for (auto EntrySet : LeafTable.Entries) {
            for (ssize_t Idx = 0; Idx < EntrySet.Occupied; ++Idx) {
                auto &Entry = EntrySet[Idx];
                insertIntoInnerNode(NewNode, getAddrFromKey(Entry.key),
                                    std::move(Entry.data));
            }
        }

        // Insert the new entry into the new inner node.
        insertIntoInnerNode(NewNode, Addr, std::move(Value));
        return NewNode;
    }

    // Insert the given value associated with the given address into an inner
    // node.
    [[clang::noinline]]
    bool insertInnerNode(uintptr_t Addr, V &&Value) {
        // NOTE: This method is defined in the header in order to ensure it is
        // properly instantiated in all recursive PageTableTy instantiations.
        if (std::holds_alternative<InnerNodeTy *>(Table)) {
            InnerNodeTy *Node = std::get<InnerNodeTy *>(Table);
            // Insert this entry into the inner node.
            insertIntoInnerNode(Node, Addr, std::move(Value));
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
            InnerNodeTy *Node = std::get<InnerNodeTy *>(Table);
            for (size_t Addr : Node->Accessed) {
                delete (*Node)[Addr];
            }
            delete Node;
        }
    }

    // Get the value at the given address.
    V *lookup(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            return std::get<LeafTableTy>(Table).lookup(Addr);
        }
        return lookupInnerNode(Addr);
    }

    // Get the table entry at the given address.
    EntryTy<V> *find(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            return std::get<LeafTableTy>(Table).find(Addr);
        }
        return findInnerNode(Addr);
    }

    // Insert the given value at the given address.
    bool insert(uintptr_t Addr, V &&Value) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            // Try to insert into this leaf table.
            auto &LeafTable = std::get<LeafTableTy>(Table);
            if (LeafTable.insert(Addr, std::move(Value)))
                return true;

            InnerNodeTy *NewNode =
                promoteLeafNodeAndInsert(LeafTable, Addr, std::move(Value));
            // Replace this table with new inner node.
            Table = NewNode;
            return true;
        }

        return insertInnerNode(Addr, std::move(Value));
    }

    // Remove the value at the given address.
    bool remove(uintptr_t Addr) {
        if (std::holds_alternative<LeafTableTy>(Table)) {
            auto &LeafTable = std::get<LeafTableTy>(Table);
            return LeafTable.remove(Addr);
        }

        return removeInnerNode(Addr);
    }

    // Iterator to traverse the elements in the table.  This iterator is used
    // for merging two tables.
    struct Iterator {
        using value_type = EntryTy<V>;
        using difference_type = ptrdiff_t;
        using InnerIterator = std::vector<size_t>::iterator;

        PageTableTy *PageTable = nullptr;
        std::variant<uintptr_t, InnerIterator> It =
            LeafTableTy::EndIteratorValue;
        SubTableTy::Iterator SubIt;

        Iterator() : SubIt() {}
        Iterator(PageTableTy &PageTable, bool MakeEnd = false)
            : PageTable(&PageTable) {
            if (MakeEnd) {
                // Create an end iterator for this table.
                if (std::holds_alternative<LeafTableTy>(PageTable.Table)) {
                    It = LeafTableTy::EndIteratorValue;
                } else {
                    InnerNodeTy *Node =
                        std::get<InnerNodeTy *>(PageTable.Table);
                    It = Node->Accessed.end();
                }
                return;
            }
            if (std::holds_alternative<LeafTableTy>(PageTable.Table)) {
                // Get the first entry in this leaf table.
                It = std::get<LeafTableTy>(PageTable.Table)
                         .advanceToNextEntry(0);
            } else {
                // Get a pointer to the first value within this inner-node.
                InnerNodeTy *Node = std::get<InnerNodeTy *>(PageTable.Table);
                auto InnerIt = Node->Accessed.begin();
                auto EndInnerIt = Node->Accessed.end();
                // Because inner nodes are not depopulated when all elements
                // within a page are removed, a scan is necessary to find the
                // first element in any subtable in this inner node.
                do {
                    SubIt = (*Node)[*InnerIt]->begin();
                } while (SubIt.atEnd() && ++InnerIt != EndInnerIt);
                It = InnerIt;
            }
        }

        value_type &operator*() {
            if (std::holds_alternative<LeafTableTy>(PageTable->Table)) {
                // Return the entry in this leaf table corresponding with the
                // iterator value.
                LeafTableTy &Leaf = std::get<LeafTableTy>(PageTable->Table);
                return Leaf.getEntryAt(std::get<uintptr_t>(It));
            }
            // Dereference the subtable iterator to get the value.
            return *SubIt;
        }

        Iterator &operator++() {
            if (std::holds_alternative<LeafTableTy>(PageTable->Table)) {
                // Advance the leaf iterator to the next entry.
                LeafTableTy &Leaf = std::get<LeafTableTy>(PageTable->Table);
                uintptr_t LeafIt = std::get<uintptr_t>(It);
                It = Leaf.advanceToNextEntry(++LeafIt);
                return *this;
            }
            // Advance the subtable iterator.
            ++SubIt;
            if (SubIt.atEnd()) {
                // The subtable iterator reached the end of its subtable.  Find
                // the next subtable with elements.
                InnerNodeTy *Node = std::get<InnerNodeTy *>(PageTable->Table);
                InnerIterator InnerIt = std::get<InnerIterator>(It);
                InnerIterator EndInnerIt = Node->Accessed.end();
                // Scan entries of this inner node until a valid entry is found.
                while (SubIt.atEnd() && ++InnerIt != EndInnerIt) {
                    SubIt = (*Node)[*InnerIt]->begin();
                }
                It = InnerIt;
                if (InnerIt == EndInnerIt)
                    // This inner node has no more entries.  Set the subtable
                    // iterator to the end-iterator value.
                    SubIt = typename SubTableTy::Iterator();
            }
            return *this;
        }
        Iterator operator++(int) {
            auto Tmp = *this;
            ++*this;
            return Tmp;
        }

        bool atEnd() const {
            if (std::holds_alternative<LeafTableTy>(PageTable->Table)) {
                // Check if this leaf-table iterator has the end value of a leaf
                // table.
                uintptr_t LeafIt = std::get<uintptr_t>(It);
                return LeafIt == LeafTableTy::EndIteratorValue;
            }
            // Check if this inner-node iterator is pointing to the end of the
            // node's accessed list.
            InnerNodeTy *Node = std::get<InnerNodeTy *>(PageTable->Table);
            const InnerIterator &InnerIt = std::get<InnerIterator>(It);
            return InnerIt == Node->Accessed.end();
        }

        bool operator==(const Iterator &Other) const {
            return It == Other.It && SubIt == Other.SubIt;
        }
    };
    static_assert(std::input_or_output_iterator<Iterator>);

    Iterator begin() { return Iterator(*this); }
    Iterator end() { return Iterator(*this, /*MakeEnd=*/true); }
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
    bool insert(uintptr_t Addr, V &&Value) {
        return Table.insert(Addr, std::move(Value));
    }

    // Remove the value at the given address.
    bool remove(uintptr_t Addr) { return Table.remove(Addr); }

    // Iterator type with the same methods as the general PageTableTy::Iterator,
    // to support recursive template instantiation.
    struct Iterator {
        // Because this particular instantiation of PageTableTy simply contains
        // a leaf table, this iterator simply handles the leaf table.
        using value_type = EntryTy<V>;
        using difference_type = ptrdiff_t;
        LeafTableTy *Table = nullptr;
        uintptr_t It = 0;

      public:
        Iterator() = default;
        Iterator(LeafTableTy &Table, bool MakeEnd = false)
            : Table(&Table), It(MakeEnd ? LeafTableTy::EndIteratorValue
                                        : Table.advanceToNextEntry(0)) {}
        Iterator(const Iterator &Other) : Table(Other.Table), It(Other.It) {}
        Iterator &operator=(const Iterator &Other) {
            Table = Other.Table;
            It = Other.It;
            return *this;
        }

        value_type &operator*() const { return Table->getEntryAt(It); }

        Iterator &operator++() {
            It = Table->advanceToNextEntry(++It);
            return *this;
        }
        Iterator operator++(int) {
            auto Tmp = *this;
            ++*this;
            return Tmp;
        }

        bool atEnd() const { return It == LeafTableTy::EndIteratorValue; }
        bool operator==(const Iterator &Other) const { return It == Other.It; }
    };
    static_assert(std::input_or_output_iterator<Iterator>);

    Iterator begin() { return Iterator(Table); }
    Iterator end() { return Iterator(Table, /*MakeEnd=*/true); }
};

using bucket = EntryTy<reducer_data>;

struct hyper_table : public PageTableTy<reducer_data> {
    using PageTableTy = PageTableTy<reducer_data>;
    using V = reducer_data;

    size_t NumEntries = 0;

    size_t size() const { return NumEntries; }

    bool insert(uintptr_t Addr, V &&Value) {
        if (PageTableTy::insert(Addr, std::move(Value))) {
            ++NumEntries;
            return true;
        }
        return false;
    }

    bool remove(uintptr_t Addr) {
        if (PageTableTy::remove(Addr)) {
            --NumEntries;
            return true;
        }
        return false;
    }
};

CHEETAH_API
hyper_table *__cilkrts_local_hyper_table_alloc(void);

static inline bucket *find_hyperobject(hyper_table *table,
                                       uintptr_t key) noexcept {
    auto *Tmp = table->find(key);
    return Tmp;
}

CHEETAH_INTERNAL
static inline bool remove_hyperobject(hyper_table *table,
                                      uintptr_t key) noexcept {
    auto Tmp = table->remove(key);
    return Tmp;
}

CHEETAH_INTERNAL
static inline bool insert_hyperobject(hyper_table *table, uintptr_t key,
                                      reducer_data &&data) noexcept {
    return table->insert(key, std::move(data));
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
