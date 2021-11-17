/* Open hash table with linear probing, mapping pointers to pointers.
   Null keys and values are not allowed.  Internally, EMPTY (null)
   indicates an empty slot and DELETED a deleted entry.  When an entry
   is deleted an attempt is made to move another value into the vacated
   slot to reduce chain lengths.  When the total of chain lengths is too
   large the table is rehashed.

   It is an error to delete a key that is not in the table.

   It is an error to insert a key that is already in the table.
   (But a deleted key can be re-inserted with a different value.)

   Lookups are attempted without locks; this will not crash but may
   give an incorrect result if the table changes during the lookup.
   If the table changes, based on a modification count, the lookup
   is retried with the lock held.

   In multithreaded environments readers can use a hyper_table_cache,
   a one entry cache.  (TODO: With lock-free reading this may not
   be needed any more.)

   The table includes a count of the number of modifications to signal
   that a previous successful lookup has become stale.  This is
   necessary to avoid the ABA problem.  (TODO: Separate insert and
   delete counters would allow hits to be invalidated by delete
   and misses to be invalidated by insert, potentially halving the
   number of cache misses.)

   TODO: Prove that if there are duplicate keys the first key is
   paired with the correct value. */

#include "hypertable.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <strings.h> /* for fls() */

#define CACHE_LINE 64

#ifndef HYPER_TABLE_ASSERT
#define HYPER_TABLE_ASSERT HYPER_TABLE_DEBUG
#endif

/* For debugging only, to examine code generation for static functions. */
#ifndef HYPER_TABLE_CODEGEN
#define HYPER_TABLE_CODEGEN 0
#endif

/* This should normally be set for performance. */
#ifndef LOCK_FREE_LOOKUP
#define LOCK_FREE_LOOKUP 1
#endif

/* This should normally be set for performance. */
#ifndef ENABLE_CACHE
#define ENABLE_CACHE 1
#endif

/* Emit a store-store barrier that prevents stores from moving
   in either direction.  atomic_signal_fence(memory_order_release)
   is sufficient but slightly stronger than needed.  Override it
   on ARM to use a store-store barrier instead.
   LLVM docs/Atomics.html explains:
   "store-store fences are generally not exposed to IR
   because they are extremely difficult to use correctly".  */
#ifdef __aarch64__
#define MEMBAR_ST_ST { \
    atomic_signal_fence(memory_order_acquire); \
    asm ("dmb ishst" : : : "memory"); \
  }
#else
#define MEMBAR_ST_ST \
    atomic_thread_fence(memory_order_release)
#endif

#define EMPTY 0
/* For strict C compliance, because 1 might convert to a valid pointer,
   define this to be the address of a file scope variable. */
#define DELETED 1

/* Alignment isn't important without a fast atomic 128 bit write
   (available on ARM from v8.4).  Tell the compiler about alignment
   anyway just in case it can do something with the information. */
struct bucket {
  uintptr_t key; /* EMPTY, DELETED, or a user-provided pointer. */
  void *value;
} __attribute__((aligned(2 * sizeof(void*))));

#define LOG2_MIN_BUCKETS  5
#define LOG2_MAX_BUCKETS 14 /* inclusive */

#define BUCKET(TABLE, SIZE) \
  &(TABLE)->buckets[(SIZE) - LOG2_MIN_BUCKETS]

/* An integer big enough to hold 2^LOG2_MAX_BUCKETS (inclusive),
   for internal use only. The API uses size_t.  Making it signed
   allows reserving negative values for invalid indices.  An
   unsigned integer could hold an extra bit.  There are small
   differences in code generation for unsigned or 16 bit indices. */

#if 0 /* signed implementation */
typedef int32_t index_t;
#define INVALID_INDEX     ((index_t)-1) /* or (index_t)~(index_t)0 */
#define IS_INVALID(INDEX) ((INDEX) < 0) /* or !!(index_t)~(INDEX) */
#define IS_VALID(INDEX)   ((INDEX) >=0) /* or !(index_t)~(INDEX) */
#else /* unsigned implementation */
typedef uint32_t index_t;
#define INVALID_INDEX     ((index_t)~(index_t)0)
#define IS_INVALID(INDEX) __builtin_expect(((INDEX) >> (sizeof(index_t) * 8 - 1)), 0)
#define IS_VALID(INDEX)   !IS_INVALID(INDEX)
#endif

#define BUSY(GEN) __builtin_expect((GEN) & 1U, 0)

#define ALLOC_FAILED(PTR) __builtin_expect(!(PTR), 0)
#define NO_BUCKET(PTR) __builtin_expect(!(PTR), 0)

struct hyper_table {
    /* A count of changes to the table, with the low bit meaning
       the table is busy and readers should wait or acquire the
       lock.  The value increments once at the beginning and once
       at the end of each modification. */
    unsigned long _Atomic gen;

    /* Log base 2 of capacity.  This field is an index into buckets[]
       after subtracting LOG2_MIN_BUCKETS. */
    int _Atomic log_capacity;

    /* Number of values in the table. */
    index_t entries;

    /* A measure of total chain length added since last rehash, used
       to decide when another rehash is required. */
    index_t waste;

    /* For statistics. */
    unsigned int rehashes;

    /* The cost of being lock-free most of the time is having to
       keep around old storage.  Each array element is null or a
       pointer to an array of size 2^(index + LOG2_MIN_BUCKETS). */
    struct bucket *_Atomic buckets[LOG2_MAX_BUCKETS + 1 - LOG2_MIN_BUCKETS];

    /* Number of child caches.  Currently unused. */
    unsigned int _Atomic caches;

    /* The lock serializes additions, deletions, and rehashes.  The only
       field that can be modified without holding the lock is caches.
       Lookups only take a lock if lock-free lookup detects a race. */
    pthread_mutex_t lock;

} __attribute__((aligned(CACHE_LINE)));

static enum hyper_table_error
hyper_table_insert_locked(struct hyper_table *, const void *, void *)
    __attribute__((nonnull));
static struct bucket *hyper_table_lookup_locked(struct hyper_table *,
                                                const void *)
    __attribute__((nonnull));
static void *hyper_table_remove_locked(struct hyper_table *, const void *_p)
    __attribute__((nonnull));

#if defined(_ISOC11_SOURCE) || __FreeBSD__ >= 10 || \
  __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 101500
#define hyper_aligned_alloc(A, S) aligned_alloc(A, S)
#else
static void *hyper_aligned_alloc(size_t alignment, size_t size)
{
    void *ptr;
    if (posix_memalign(&ptr, alignment, size) == 0)
        return ptr;
    return 0;
}
#endif

static void lock_table(struct hyper_table *table)
{
    int error = pthread_mutex_lock(&table->lock);
    assert(!error);
}

static void unlock_table(struct hyper_table *table)
{
#if HYPER_TABLE_ASSERT
    assert(!BUSY(table->gen));
#endif
    int error = pthread_mutex_unlock(&table->lock);
    assert(!error);
}

#if HYPER_TABLE_ASSERT
__attribute__((noinline))
static enum hyper_table_error fail(struct hyper_table *table,
                                   enum hyper_table_error code)
{
    fprintf(stdout, "Operation failure code %d\n", (int)code);
    hyper_table_dump(stdout, table);
    fflush(stdout);
    return code;
}
#else
#define fail(table, code) (code)
#endif

/* Return a hash function where the low bits are hopefully random.
   The caller is responsible for reducing the hash to the desired range. */
static index_t calc_hash(uintptr_t key_in)
{
    uintptr_t key = key_in;
    /* TODO: Improve this.  Knuth likes the golden ratio for hashing. */
    /* Mac on x86 has addresses like 0x0000602000000210 with lots
         of consecutive zero bits between groups of nonzero.  */
    if (sizeof key > 4)
        key += __builtin_rotateleft64(key, 21);
    else
        key += __builtin_rotateleft32(key, 21);
    if (sizeof key > 4)
        return (key * 0x595a5b5c5d5e5f53) >> 30;
    else
        return (key * 0x5a5a5a5b) >> 10;
}

struct hyper_table *hyper_table_create(size_t capacity_req)
{
    int start_size = LOG2_MIN_BUCKETS;
    if (3 * capacity_req >= (size_t)1U << (LOG2_MAX_BUCKETS + 1)) {
        start_size = LOG2_MAX_BUCKETS;
    } else if (capacity_req <= ((size_t)3U << (LOG2_MIN_BUCKETS - 1))) {
        start_size = LOG2_MIN_BUCKETS;
    } else {
        /* Multiply by 3/2 for rounding. */
        long adjusted = capacity_req * 3; /* / 2 implied by -2 below */
#if defined __linux__ || defined __APPLE__ /* No inlined flsl. */
        start_size = 8 * sizeof(long) - 2 - __builtin_clzl(adjusted);
#else
        start_size = flsl(adjusted) - 2;
#endif
        assert(start_size >= LOG2_MIN_BUCKETS && start_size < LOG2_MAX_BUCKETS);
    }

    size_t capacity = (size_t)1 << start_size;
    /* This needs to be a multiple of CACHE_LINE or aligned_alloc will fail. */
    size_t bucket_bytes = capacity * sizeof(struct bucket);
    struct bucket *buckets = hyper_aligned_alloc(CACHE_LINE, bucket_bytes);
    if (ALLOC_FAILED(buckets))
        return 0;
    struct hyper_table *table =
        hyper_aligned_alloc(CACHE_LINE, sizeof(struct hyper_table));
    if (ALLOC_FAILED(table))
        goto cleanup;
    memset(table, 0, sizeof(struct hyper_table));
    if (pthread_mutex_init(&table->lock, 0))
        goto cleanup;
    memset(buckets, 0, bucket_bytes);
    atomic_store_explicit(&table->log_capacity, start_size, memory_order_relaxed);
    /* Integer fields were set to zero by memset above.  Also assume memset
         nulled pointers, which is not strictly required by the C standard. */
    atomic_store_explicit(BUCKET(table, start_size), buckets,
                          memory_order_relaxed);
    atomic_store_explicit(&table->gen, 2, memory_order_release);
    return table;
 cleanup:
    free(buckets);
    free(table);
    return 0;
}

void hyper_table_destroy(struct hyper_table *table)
{
    pthread_mutex_destroy(&table->lock);
    for (unsigned i = 0; i < sizeof table->buckets / sizeof table->buckets[0]; ++i) {
        struct bucket *b =
            atomic_load_explicit(&table->buckets[i], memory_order_relaxed);
        atomic_store_explicit(&table->buckets[i], 0, memory_order_relaxed);
        free(b);
    }
    free(table);
}

/* Called by remove and lookup to find a bucket that holds the given key,
     stopping the search when an empty bucket is found.  The caller must
     handle a null value. */
static struct bucket *
find_bucket(struct bucket *buckets, int log_capacity,
                index_t hash, uintptr_t key)
{
    index_t capacity = (index_t)1 << log_capacity;
    index_t mask = capacity - 1U;
    hash &= mask;
    index_t index = hash;
    do {
        struct bucket *bucket = &buckets[index];
        /* With reasonable load factors the first bucket will match. */
        if (__builtin_expect(bucket->key == key, 1))
            return bucket;
        /* Predicted false for remove and true for lookup. */
        if (bucket->key == EMPTY)
            return 0;
        index = (index + 1) & mask;
    } while (index != hash);
    return 0;
}

/* Set the busy flag in the low bit of table->gen to inform readers
     that the table is being modified.  Return the old generation number
     before setting the flag. */
static unsigned long mark_busy(struct hyper_table *table)
{
    unsigned long gen = atomic_load_explicit(&table->gen, memory_order_relaxed);
#if HYPER_TABLE_ASSERT
    assert(!BUSY(gen));
#endif
    /* The store below is meant to act like a store with acquire semantics
         (which does not exist in isolation).  The store-store barrier ensures
         that the set of the busy bit is visible before any changes to data.  */
    atomic_store_explicit(&table->gen, gen + 1, memory_order_relaxed);
    MEMBAR_ST_ST;
    return gen;
}

static void mark_free(struct hyper_table *table, unsigned long old_gen)
{
    /* Readers will load table->gen with acquire semantics.  If the value
         is unchanged from the start of the read operation then the table
         has not been modified. */
    atomic_store_explicit(&table->gen, old_gen + 2, memory_order_release);
}

/* Find a bucket that is empty or deleted.  This function is called to
     insert a key that is known not to be in the table.  If insert instead
     meant modify the function would need to continue past deleted buckets
     to look for a matching key. */
static struct bucket *
find_insert_point(struct bucket *buckets, index_t capacity,
                      index_t start, index_t *waste)
{
    index_t mask = capacity - 1U;
    index_t index = start;
    index_t waste0 = *waste;
    for (index_t i = 0; i < capacity; ++i) {
        struct bucket *bucket = &buckets[index++ & mask];
        uintptr_t key = bucket->key;
        /* With reasonable load factors the first bucket will be available. */
        if (__builtin_expect(key == EMPTY || key == DELETED, 1)) {
            *waste = waste0 + i;
            return bucket;
        }
    }
    return 0;
}

/* Copy all valid entries to a new bucket list.  Assert that the
     number of entries copied is the same number thought to be in
     the table.  Return a measure of wasted space. */
static void copy(struct bucket *restrict to_ptr, index_t to_size,
                     const struct bucket *restrict from_ptr, index_t from_size,
                     index_t expected)
{
    index_t waste = 0;
    index_t new_entries = 0;
    for (index_t from = 0; from < from_size; ++from) {
        const struct bucket *b = &from_ptr[from];
        uintptr_t key = b->key;
        if (key == EMPTY || key == DELETED)
            continue;
        index_t hash = calc_hash(key);
        /* In this function find_insert_point should never fail because the
           new table is guaranteed to be big enough. */
        *find_insert_point(to_ptr, to_size, hash, &waste) = *b;
        ++new_entries;
    }
    assert(new_entries == expected);
}

/* Effectively, erase the table and re-insert all entries in an attempt
     to reduce chain lengths. */
static void table_rehash(struct hyper_table *table)
{
    /* The lock is held so loads can use relaxed order. */
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    struct bucket *old_buckets =
        atomic_load_explicit(BUCKET(table, log_capacity), memory_order_relaxed);
    assert(log_capacity <= LOG2_MAX_BUCKETS);
    size_t capacity = (size_t)1 << log_capacity;

    struct bucket *tmp =
        hyper_aligned_alloc(CACHE_LINE, capacity * sizeof(struct bucket));
    if (ALLOC_FAILED(tmp)) {
        table->waste = 0; /* avoid repeated futile rehash attempts */
        return;
    }
    memset(tmp, 0, capacity * sizeof(struct bucket));
    copy(tmp, capacity, old_buckets, capacity, table->entries);
    unsigned long old_gen = mark_busy(table);
    memcpy(old_buckets, tmp, capacity * sizeof(struct bucket));
    table->waste = 0;
    ++table->rehashes;
    mark_free(table, old_gen); /* includes release fence */
    free(tmp);
    return;
}

/* Return null on failure, otherwise the new bucket list. */
static struct bucket *
table_grow(struct hyper_table *table)
{
    /* The lock is held so loads can use relaxed order. */
    int old_log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    struct bucket *old_buckets =
        atomic_load_explicit(BUCKET(table, old_log_capacity), memory_order_relaxed);
    assert(old_log_capacity < LOG2_MAX_BUCKETS);
    int new_log_capacity = old_log_capacity + 1;
    size_t old_capacity = (size_t)1 << old_log_capacity;
    size_t new_capacity = (size_t)1 << new_log_capacity;

    assert(new_log_capacity > old_log_capacity);

    /* Reuse an old array if there is one.  This could happen
         when shrinking is implemented. */     
    struct bucket *new_buckets =
        atomic_load_explicit(BUCKET(table, new_log_capacity), memory_order_relaxed);
    if (!new_buckets) {
        new_buckets =
            hyper_aligned_alloc(CACHE_LINE,
                                new_capacity * sizeof(struct bucket));
        if (ALLOC_FAILED(new_buckets))
            return 0;
        memset(new_buckets, 0, new_capacity * sizeof(struct bucket));
        /* Publish the new pointer after the memory is cleared. */
        atomic_store_explicit(BUCKET(table, new_log_capacity),
                              new_buckets, memory_order_release);
    }

    copy(new_buckets, new_capacity, old_buckets, old_capacity, table->entries);

    table->waste = 0;
    ++table->rehashes;

    /* First, mark the table busy so no readers come in between the
         next two stores. */
    unsigned long old_gen = mark_busy(table);

    /* Force all writes to complete before the bucket pointer goes live. */
    atomic_store_explicit(&table->log_capacity, new_log_capacity,
                          memory_order_release);

    mark_free(table, old_gen);

    return new_buckets;
}

enum hyper_table_error
hyper_table_insert(struct hyper_table *table, const void *key, void *value)
{
    if (__builtin_expect(!key, 0) || __builtin_expect(!value, 0))
        return HYPER_NULL;
    lock_table(table);
    enum hyper_table_error error = hyper_table_insert_locked(table, key, value);
    unlock_table(table);
    return error;
}

/* Unlike lookup, this must be called with the lock held. */
static enum hyper_table_error
hyper_table_insert_locked(struct hyper_table *restrict table, const void *key_p,
                              void *restrict value)
{
    /* Lock is held, relaxed order is fine. */
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    struct bucket *buckets =
        atomic_load_explicit(BUCKET(table, log_capacity), memory_order_relaxed);
    index_t capacity = (index_t)1 << log_capacity;
    /* Keep the load factor .5 or less if possible.  If chain lengths are
         growing long, which should be rare, rehash in place.  */
    if (log_capacity < LOG2_MAX_BUCKETS &&
          __builtin_expect(table->entries > capacity / 2, 0)) {
        capacity *= 2;
        buckets = table_grow(table);
        /* Strictly speaking this error is recoverable, but inability to
           allocate a new hash table indicates memory is about to run out.
           Also, inability to recreate the old hash table is very unlikely. */
        if (ALLOC_FAILED(buckets))
            return fail(table, HYPER_NOMEM);
    } else if (__builtin_expect(table->waste * 3UL > capacity, 0)) {
        table_rehash(table); /* bucket pointer unchanged */
    }
    uintptr_t key = (uintptr_t)key_p;
    index_t hash = calc_hash(key);
    index_t waste = table->waste;
    struct bucket *bucket = find_insert_point(buckets, capacity, hash, &waste);
    if (NO_BUCKET(bucket))
        return fail(table, HYPER_FULL);
    index_t entries = table->entries;
    /* Up to now lookups can proceed in parallel with this insertion,
         but filling the bucket is not atomic. */
    unsigned long old_gen = mark_busy(table);
    /* These stores (before the release in mark_free) can happen in any order.  */
    bucket->key = key;
    bucket->value = value;
    table->entries = entries + 1;
    table->waste = waste;
    mark_free(table, old_gen);
    return HYPER_OK;
}

void *hyper_table_remove(struct hyper_table *table, const void *key)
{
    lock_table(table);
    void *value = hyper_table_remove_locked(table, key);
    unlock_table(table);
    return value;
}

void *hyper_table_remove_locked(struct hyper_table *table, const void *key_p)
{
    /* Lock is held, relaxed order is fine for both loads. */
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    struct bucket *buckets =
        atomic_load_explicit(BUCKET(table, log_capacity), memory_order_relaxed);
    uintptr_t key = (uintptr_t)key_p;
    index_t hash = calc_hash(key);
    struct bucket *bucket = find_bucket(buckets, log_capacity, hash, key);
#if HYPER_TABLE_ASSERT
    /* In anticipated uses of this table, the entry must exist or outside
         bookkeeping has gone wrong. */
    assert(!NO_BUCKET(bucket));
#endif
    if (NO_BUCKET(bucket))
        return 0;

    index_t index = bucket - buckets;
    index_t mask = ((index_t)1 << log_capacity) - 1;
    index_t this_target = hash & mask;
    index_t entries = table->entries;
    index_t waste = table->waste;
    if (this_target != index && waste > 0) {
        --waste;
    }
    unsigned long old_gen = mark_busy(table);

    table->entries = entries - 1;

    void *value = bucket->value;
    bucket->key = DELETED;
    bucket->value = 0;
    /* While the lock is held do some cleanup in the vicinity of the
         deleted entry:
         1. If the next bucket is empty mark this one empty, and also
         the previous bucket if that bucket is deleted.
         2. If the next bucket wants to be earlier in the chain, move it up
         to the newly vacated slot.  */
    index_t prev = (index - 1) & mask;
    index_t next = (index + 1) & mask;
    uintptr_t next_key = buckets[next].key;
    if (next_key == EMPTY) {
        buckets[index].key = EMPTY; /* deleted -> empty */
        if (buckets[prev].key == DELETED)
            buckets[prev].key = EMPTY; /* deleted -> empty */
        goto done;
    }
    ++waste; /* a new deleted bucket has been created */
    if (buckets[next].key == DELETED)
        goto done;
    /* Where does the next bucket want to be? */
    index_t next_target = calc_hash(next_key) & mask;
    /* If the next bucket wants to be earlier, advance it into this slot.
         A simple equality test is good enough here.  If the bucket doesn't
         want to be where it is, one place earlier is better. */
    if (next_target != next) {
        buckets[index] = buckets[next];
        buckets[next].key = DELETED; /* full -> deleted */
    }
 done:
    table->waste = waste;
    mark_free(table, old_gen);
    return value;
}

void *hyper_table_lookup(struct hyper_table *table, const void *key)
{
    lock_table(table);
    struct bucket *bucket = hyper_table_lookup_locked(table, key);
    void *value = bucket ? bucket->value : 0;
    unlock_table(table);
    return value;
}

static struct bucket *
hyper_table_lookup_locked(struct hyper_table *table, const void *key_p)
{
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    struct bucket *buckets =
        atomic_load_explicit(BUCKET(table, log_capacity), memory_order_relaxed);
    uintptr_t key = (uintptr_t)key_p;
    return find_bucket(buckets, log_capacity, calc_hash(key), key);
}

void hyper_table_iter(struct hyper_table *table,
                          void (*fn)(void *, const void *, void *),
                          void *arg)
{
    int error = pthread_mutex_lock(&table->lock);
    assert(!error);
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    struct bucket *buckets =
        atomic_load_explicit(BUCKET(table, log_capacity), memory_order_relaxed);
    index_t size = table->entries;
    struct bucket tmp[size];
    index_t out = 0;
    index_t capacity = (index_t)1 << log_capacity;
    for (index_t i = 0; i < capacity; ++i) {
        if (buckets[i].key != EMPTY && buckets[i].key != DELETED)
          tmp[out++] = buckets[i];
    }
    assert(out == table->entries);
    pthread_mutex_unlock(&table->lock);
    for (index_t i = 0; i < size; ++i)
        fn(arg, (const void *)tmp[i].key, tmp[i].value);
}

#if HYPER_TABLE_DEBUG
void hyper_table_dump(FILE *out, const struct hyper_table *table)
{
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_acquire);
    struct bucket *buckets =
        atomic_load_explicit(BUCKET(table, log_capacity), memory_order_consume);
    index_t capacity = (index_t)1 << log_capacity;
    fprintf(out,
              "Table %p size %lu capacity %lu waste %lu rehash %u gen %lu\n",
              table, (unsigned long)table->entries,
              (unsigned long)capacity,
              (unsigned long)table->waste,
              table->rehashes, table->gen);
    for (index_t i = 0; i < capacity; ++i) {
        if (buckets[i].key == EMPTY)
            continue;
        if (buckets[i].key == DELETED) {
            fprintf(out, "[%5u] = link\n", (unsigned)i);
            continue;
        }
        index_t target = calc_hash((uintptr_t)buckets[i].key) & (capacity - 1);
        fprintf(out, "[%5u]: %p -> %p", (unsigned)i,
                (void *)buckets[i].key, buckets[i].value);
        if (target != i)
            fprintf(out, " (target %3lu)", (unsigned long)target);
        fputc('\n', out);
    }
    fflush(out);
}
#endif

/* Once global_table is non-null its value will not change.

     In order to ensure that readers of the table see the
     initialization of the mutex, the value is published
     with release semantics and loaded with consume semantics.
     Consume order tells the compiler to tell the processor
     not to allow any loads based off of [table] to be moved
     before [table] is loaded.  In practice (1) most processors
     do this automatically, (2) the stupid compiler promotes
     consume to acquire anyway.

     This ordering has no effect on x86 code generation: atomic
     compare and exchange is always a full barrier, and causality
     prevents any accesses based off of [global_table] from being
     moved before the load.

     TODO: Performance should be tested on ARM. */
static struct hyper_table *_Atomic global_table;

struct hyper_table *hyper_table_get_or_create(size_t capacity)
{
    struct hyper_table *table =
        atomic_load_explicit(&global_table, memory_order_consume);
    if (!ALLOC_FAILED(table))
        return table;
    table = hyper_table_create(capacity);
    if (ALLOC_FAILED(table))
        return 0;
    /* If [global_table] is still null, store [table] into [global_table].
         Otherwise, copy [global_table] into [tmp].  */
    struct hyper_table *tmp = 0;
    if (__c11_atomic_compare_exchange_strong(&global_table, &tmp, table,
                                             memory_order_release,
                                             memory_order_consume))
        return table;
    hyper_table_destroy(table);
    return tmp;
}

/* A simple two entry cache.  In order to prevent an ABA problem a
     cache lookup reads a word from the parent table to check whether
     the table has changed.  If a lookup races with a entry creation
     or deletion the result is undefined.

     The structure needs to fill a cache line to prevent false sharing.  */
struct hyper_table_cache {
    struct hyper_table *parent;
#if ENABLE_CACHE
    unsigned long gen;
    struct bucket entry[2];
    unsigned int count;
#endif
} __attribute__((aligned(CACHE_LINE)));

static void hyper_table_cache_invalidate(struct hyper_table_cache *c)
{
#if ENABLE_CACHE
    c->entry[0].key = 0;
    c->entry[0].value = 0;
    c->entry[1].key = 0;
    c->entry[1].value = 0;
    c->count = 0; /* any value will do */
    c->gen = 1; /* 1 is never valid because the busy bit is set */
#endif
}

struct hyper_table_cache *hyper_table_cache_create(struct hyper_table *parent)
{
    struct hyper_table_cache *c =
        hyper_aligned_alloc(__alignof__(struct hyper_table_cache),
                            sizeof(struct hyper_table_cache));
    if (ALLOC_FAILED(c))
        return 0;
    c->parent = parent;
    hyper_table_cache_invalidate(c);
    atomic_fetch_add_explicit(&parent->caches, 1, memory_order_acquire);
    return c;
}

void hyper_table_cache_destroy(struct hyper_table_cache *c)
{
    struct hyper_table *parent = c->parent;
    c->parent = 0;
    hyper_table_cache_invalidate(c);
    atomic_fetch_sub_explicit(&parent->caches, 1, memory_order_release);
    free(c);
}

static void *
hyper_table_cache_lookup_slow(struct hyper_table_cache *cache,
                                  struct hyper_table *table,
                                  const void *key_p)
{
    lock_table(table);
    struct bucket *bucket = hyper_table_lookup_locked(table, key_p);
    /* Relaxed order is fine with the lock held. */
    unsigned long gen = atomic_load_explicit(&table->gen, memory_order_relaxed);
    void *value = 0;
    if (bucket) {
#if ENABLE_CACHE
        unsigned int e = 1U & ++cache->count;
        cache->entry[e] = *bucket;
        cache->gen = gen;
#endif
        value = bucket->value;
    }
    unlock_table(table);
    return value;
}

void *hyper_table_cache_lookup(struct hyper_table_cache *cache, const void *key_p)
{
    if (__builtin_expect(!key_p, 0))
        return 0;

    /* On memory ordering:

         Table writers guarantee that there are no writes to the table
         between a write of table->gen with low bit clear and the next
         write to table->gen.  Writes to table->gen with low bit clear
         have release semantics.

         The first load of table->gen can find the low bit set or clear.

         If the bit is clear, the acquire pairs with the store-release of
         the last write to table->gen to ensure the table is consistent.

         If the bit is set, a lock is taken to ensure consistency with
         writers.  Writers also take the lock.

         Following a load of table->gen with low bit clear, a second
         load is issued at the end of the lookup fast path.  If it finds
         a different value, a lock is taken as above.

         What remains is to ensure that if both loads of table->gen
         return the same value then values read in between them are
         a consistent view of the table with no writes to it.

         An acquire fence before the second load pairs with the store-release
         of the new value of table->gen.  If the load of table->gen does not
         see the new value, then none of the earlier loads saw stores that
         preceded the write to table->gen. */

    struct hyper_table *table = cache->parent;

    /* The cache hit case can use a relaxed load because it makes
         no other accesses to the main table.  The cache miss flow
         depends on acquire semantics. */
    unsigned long gen1 = atomic_load_explicit(&table->gen, memory_order_acquire);

#if ENABLE_CACHE
    if (__builtin_expect(cache->gen == gen1, 1)) {
        uintptr_t key0 = cache->entry[0].key;
        uintptr_t key1 = cache->entry[1].key;
        if (key0 == (uintptr_t)key_p)
            return cache->entry[0].value;
        if (key1 == (uintptr_t)key_p)
            return cache->entry[1].value;
    } else {
        hyper_table_cache_invalidate(cache);
    }
#endif

#if LOCK_FREE_LOOKUP
    /* Attempt lock-free lookup first.  */
    if (!BUSY(gen1)) {
        /* Arguably the load of log_capacity should have memory_order_consume, but
           1: That only matters for a few unsupported DEC ALPHA chips
           (where stores issued in order remotely may appear out of order locally).
           2: Consume is promoted to acquire, which has a cost. */
        int log_capacity =
            atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
        struct bucket *buckets =
            atomic_load_explicit(BUCKET(table, log_capacity),
                                 memory_order_relaxed);
        uintptr_t key = (uintptr_t)key_p;
        struct bucket *bucket =
            find_bucket(buckets, log_capacity, calc_hash(key), key);
        if (!NO_BUCKET(bucket)) {
          /* Optimistically save the value in the cache.  The cache invalidate
             call below will clean up if the value is incorrect. */
          unsigned int e = 1U & ++cache->count;
          cache->entry[e] = *bucket;
          void *result = bucket->value;
          /* See comment above on memory ordering. */
          atomic_thread_fence(memory_order_acquire);
          unsigned long gen2 =
              atomic_load_explicit(&table->gen, memory_order_relaxed);
          if (__builtin_expect(gen1 == gen2, 1)) {
              return result;
          }
        }
    }
#endif

    return hyper_table_cache_lookup_slow(cache, table, key_p);
}

void *hyper_table_cache_remove(struct hyper_table_cache *cache, const void *key)
{
    if (__builtin_expect(!key, 0))
        return 0;
    struct hyper_table *table = cache->parent;
    lock_table(table);
    hyper_table_cache_invalidate(cache);
    void *value = hyper_table_remove_locked(table, key);
    unlock_table(table);
    return value;
}

enum hyper_table_error
hyper_table_cache_insert(struct hyper_table_cache *cache, const void *key,
                             void *value)
{
    if (__builtin_expect(!key, 0) || __builtin_expect(!value, 0))
        return HYPER_NULL;
    struct hyper_table *table = cache->parent;
    lock_table(table);
    enum hyper_table_error error =
        hyper_table_insert_locked(table, key, value);
    unsigned long gen = atomic_load_explicit(&table->gen, memory_order_relaxed);
    unlock_table(table);
#if ENABLE_CACHE
    /* Reset the cache to hold only the newly added entry in slot 0,
         with slot 1 being the next used. */
    cache->count = 0;
    cache->gen = gen;
    cache->entry[0].key = (uintptr_t)key;
    cache->entry[0].value = value;
    cache->entry[1].key = 0;
    cache->entry[1].value = 0;
#endif
    return error;
}

#if HYPER_TABLE_CODEGEN
void copy_debug(struct bucket *restrict to_ptr, index_t to_size,
                    const struct bucket *restrict from_ptr, index_t from_size,
                    index_t expected)
{
    copy(to_ptr, to_size, from_ptr, from_size, expected);
}

struct bucket *find_bucket_debug(struct bucket *buckets, int log_capacity,
                                     index_t hash, uintptr_t key)
{
    return find_bucket(buckets, log_capacity, hash, key);
}

unsigned long mark_busy_debug(struct hyper_table *table)
{
    return mark_busy(table);
}
#endif

const char *hyper_table_error_string(enum hyper_table_error code)
{
    switch (code) {
    case HYPER_OK: return "no error";
    case HYPER_NOT_FOUND: return "key not found";
    case HYPER_NULL: return "null key";
    case HYPER_NOMEM: return "out of memory";
    case HYPER_FULL: return "table full";
    default: return "unknown error";
    }
}

size_t hyper_table_size(const struct hyper_table *table)
{
    return table->entries;
}

size_t hyper_table_index(const struct hyper_table *table, const void *key)
{
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    return calc_hash((uintptr_t)key) & (((index_t)1 << log_capacity) - 1);
}

size_t hyper_table_capacity(const struct hyper_table *table)
{
    int log_capacity =
        atomic_load_explicit(&table->log_capacity, memory_order_relaxed);
    return (index_t)1 << log_capacity;
}
