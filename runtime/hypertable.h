#ifdef HYPER_TABLE_DEBUG
#include <stdio.h>
#endif
#include <stdlib.h>

#ifdef HYPER_TABLE_HIDDEN
#define HYPER_TABLE_HIDE __attribute__((visibility("hidden"), nothrow))
#define HYPER_TABLE_OP __attribute__((visibility("hidden"), nothrow, nonnull(1)))
#define HYPER_TABLE_CHECK __attribute__((visibility("hidden"), nothrow, warn_unused_result, nonnull(1)))
#define HYPER_TABLE_ALLOC __attribute__((visibility("hidden"), nothrow, malloc))
#else
#define HYPER_TABLE_HIDE  __attribute__((nothrow))
#define HYPER_TABLE_OP __attribute__((nothrow, nonnull(1)))
#define HYPER_TABLE_CHECK __attribute__((warn_unused_result, nothrow, nonnull(1)))
#define HYPER_TABLE_ALLOC __attribute__((malloc, nothrow))
#endif

enum hyper_table_error {
    HYPER_OK,
    HYPER_NOT_FOUND,
    HYPER_FULL,
    HYPER_NULL,     /* user error: null key */
    HYPER_NOMEM,    /* unable to allocate memory */
};

struct hyper_table;

/* Get the unique global hyperobject table, creating it if it does
   not already exist. */
HYPER_TABLE_HIDE
struct hyper_table *hyper_table_get_or_create(size_t capacity);

/* Create a new hyperobject table. */
HYPER_TABLE_ALLOC
struct hyper_table *hyper_table_create(size_t capacity);

/* Destroy a hyperobject table created by hyper_table_create. */
HYPER_TABLE_OP
void hyper_table_destroy(struct hyper_table *);

/* Insert a new entry.  The key must not be in the table already. */
HYPER_TABLE_CHECK
enum hyper_table_error
hyper_table_insert(struct hyper_table *, const void *key, void *value);

/* Remove a key and return the old value.  The key must be in the table. */
HYPER_TABLE_OP
void *hyper_table_remove(struct hyper_table *, const void *key);

/* Return the value for a key, or null if it is not present. */
HYPER_TABLE_OP
void *hyper_table_lookup(struct hyper_table *, const void *key);

/* Return the number of keys in the table. */
HYPER_TABLE_OP
size_t hyper_table_size(const struct hyper_table *);

/* Apply a function to every table entry. */
HYPER_TABLE_OP
void hyper_table_iter(struct hyper_table *,
                      void (*fn)(void *, const void *, void *),
                      void *);

/* Return the current bucket list length, which will not be less
   than hyper_table_size().  This is intended for testing. */
HYPER_TABLE_OP
size_t hyper_table_capacity(const struct hyper_table *);

/* Return the index where a key belongs.  The value will be less
   than hyper_table_capacity().  This is intended for testing. */
HYPER_TABLE_OP
size_t hyper_table_index(const struct hyper_table *, const void *);

#if HYPER_TABLE_DEBUG
/* Print a text representation of the table. */
HYPER_TABLE_HIDE
void hyper_table_dump(FILE *out, const struct hyper_table *table);
#endif

struct hyper_table_cache;

/* Create, destroy, insert, lookup, and remove work like the
   functions above except they use a cache. */

HYPER_TABLE_CHECK
struct hyper_table_cache *hyper_table_cache_create(struct hyper_table *);

HYPER_TABLE_OP
void hyper_table_cache_destroy(struct hyper_table_cache *);

HYPER_TABLE_CHECK
enum hyper_table_error
hyper_table_cache_insert(struct hyper_table_cache *, const void *key,
                         void *value);

HYPER_TABLE_OP
void *hyper_table_cache_lookup(struct hyper_table_cache *, const void *key);

HYPER_TABLE_OP
void *hyper_table_cache_remove(struct hyper_table_cache *, const void *key);

HYPER_TABLE_HIDE
const char *hyper_table_error_string(enum hyper_table_error error)
    __attribute__((returns_nonnull));

#undef HYPER_TABLE_HIDE
#undef HYPER_TABLE_CHECK
#undef HYPER_TABLE_ALLOC
