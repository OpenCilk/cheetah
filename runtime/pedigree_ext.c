#include "pedigree-internal.h"

// Pedigree-extension code, included in the runtime as part of the bitcode file.

void __cilkrts_extend_spawn(__cilkrts_worker *w, void **parent_extension,
                            void **child_extension) {
    // Copy the child extension into the parent, and create a new
    // __pedigree_frame for the child.
    *parent_extension = *child_extension;

    // Get a new pedigree frame for the child extension.
    __pedigree_frame *frame = push_pedigree_frame(w);
    *child_extension = frame;

    // Initialize the new frame.
    __pedigree_frame *parent_frame = (__pedigree_frame *)(*parent_extension);
    // Copy the parent's rank into the child frame's pedigree.rank.
    frame->pedigree.rank = parent_frame->rank;
    // Append the child frame's pedigree onto the linked list.
    frame->pedigree.parent = &(parent_frame->pedigree);
    // Initialize the child frame's rank to 0.
    frame->rank = 0;

    // Increment the dprng_depth in the child frame.
    frame->dprng_depth = parent_frame->dprng_depth + 1;
    // Update the child frame's dprng_dotproduct.
    uint64_t parent_dprng_dotproduct = parent_frame->dprng_dotproduct;
    frame->dprng_dotproduct = __cilkrts_dprng_sum_mod_p(
        parent_dprng_dotproduct, __pedigree_dprng_m_array[frame->dprng_depth]);

    // Update the rank and dprng_dotproduct in the parent frame.
    parent_frame->rank++;
    parent_frame->dprng_dotproduct = __cilkrts_dprng_sum_mod_p(
        parent_dprng_dotproduct,
        __pedigree_dprng_m_array[parent_frame->dprng_depth]);
}

void __cilkrts_extend_return_from_spawn(__cilkrts_worker *w, void **extension) {
    // Free the pedigree frame.
    pop_pedigree_frame(w);
    (void)extension; // TODO: Remove the parameter?
}

void __cilkrts_extend_sync(void **extension) {
    // Update the rank and dprng_dotproduct.
    __pedigree_frame *frame = (__pedigree_frame *)(*extension);
    frame->rank++;
    frame->dprng_dotproduct = __cilkrts_dprng_sum_mod_p(
        frame->dprng_dotproduct, __pedigree_dprng_m_array[frame->dprng_depth]);
}
