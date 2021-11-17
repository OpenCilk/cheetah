#include "pedigree-internal.h"

// External pedigree library code.  Linking this code with a Cilk program
// enables pedigrees.

////////////////////////////////////////////////////////////////////////////////
// Global variables local to the library.

uint64_t __pedigree_dprng_seed = 0x8c679c168e6bf733ul;
uint64_t __pedigree_dprng_m_X = 0;
CHEETAH_INTERNAL
__pedigree_frame root_frame = {.pedigree = {.rank = 0, .parent = NULL},
                               .rank = 0,
                               .dprng_depth = 0,
                               .dprng_dotproduct = 0};

////////////////////////////////////////////////////////////////////////////////
// Initialization and deinitialization

CHEETAH_INTERNAL
void __cilkrts_deinit_dprng(void) {
    if (__pedigree_dprng_m_array) {
        free(__pedigree_dprng_m_array);
        __pedigree_dprng_m_array = NULL;
    }
}

void __cilkrts_init_dprng(void) {
    // TODO: Disallow __cilkrts_init_dprng() from being called in parallel.
    if (!__pedigree_dprng_m_array) {
        __pedigree_dprng_m_array =
            (uint64_t *)malloc(sizeof(uint64_t *) * 4096);
        atexit(__cilkrts_deinit_dprng);
    }

    for (int i = 0; i < 4096; i++) {
        __pedigree_dprng_m_array[i] =
            __cilkrts_dprng_mix_mod_p(__pedigree_dprng_seed + i);
    }
    __pedigree_dprng_m_X =
        __cilkrts_dprng_mix_mod_p(__pedigree_dprng_seed + 4096);
}

CHEETAH_INTERNAL
void __pedigree_init(void) {
    root_frame.dprng_dotproduct = __pedigree_dprng_m_X;

    __cilkrts_register_extension(&root_frame);
}

CHEETAH_INTERNAL
__attribute__((constructor)) void __pedigree_startup(void) {
    __cilkrts_init_dprng();

    if (!__cilkrts_is_initialized())
        __cilkrts_atinit(__pedigree_init);
    else
        __pedigree_init();
}

////////////////////////////////////////////////////////////////////////////////
// API methods, callable from user code.
//
// These methods are included here so that, if a Cilk program attempts to use
// one of these routines without incorporating this library, the user will get
// sensible-looking linker errors.

// Helper method to advance the pedigree and dprng states.
void __cilkrts_bump_worker_rank(void) { bump_worker_rank(); }

// Set the seed for the dprand DPRNG.
void __cilkrts_dprand_set_seed(uint64_t seed) {
    __pedigree_dprng_seed = seed;
    __cilkrts_init_dprng();
}

// Get the current value of the dprand DPRNG.
uint64_t __cilkrts_get_dprand(void) {
    __pedigree_frame *frame = bump_worker_rank();
    return __cilkrts_dprng_mix_mod_p(frame->dprng_dotproduct);
}

// Get the current pedigree, in the form of a pointer to its leaf node.
__cilkrts_pedigree __cilkrts_get_pedigree(void) {
    __cilkrts_pedigree ret_ped;
    __pedigree_frame *frame = (__pedigree_frame *)(__cilkrts_get_extension());
    ret_ped.parent = &(frame->pedigree);
    ret_ped.rank = frame->rank;
    return ret_ped;
}
