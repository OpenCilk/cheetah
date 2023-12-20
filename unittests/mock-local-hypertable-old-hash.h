// Data type for indexing the hash table.  This type is used for
// hashes as well as the table's capacity.
static const int32_t MIN_CAPACITY = 1;
static const int32_t MIN_HT_CAPACITY = 1;

static const uint64_t salt = 0x96b9af4f6a40de92UL;

// Mock hash function for unit-testing the logic of local hypertables.
static inline index_t hash(uintptr_t key_in) {
    uint64_t x = key_in ^ salt;
    // mix64 from SplitMix.
    x = (x ^ (x >> 33)) * 0xff51afd7ed558ccdUL;
    x = (x ^ (x >> 33)) * 0xc4ceb9fe1a85ec53UL;
    return x;
}
