// Data type for indexing the hash table.  This type is used for
// hashes as well as the table's capacity.
static const int32_t MIN_CAPACITY = 1;
static const int32_t MIN_HT_CAPACITY = 1;

// Mock hash function for unit-testing the logic behind local hypertables.
static inline index_t hash(uintptr_t key_in) {
    return key_in;
}
