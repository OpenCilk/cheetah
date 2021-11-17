#include "pedigree-internal.h"

// This variable needs to be accessed both from the external pedigree library
// and the pedigree-extension code in the core runtime library.
uint64_t *__pedigree_dprng_m_array = NULL;
