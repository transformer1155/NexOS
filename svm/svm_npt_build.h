// =====================================================================
//  svm_npt_build.c  -  NPT page-table construction (pure memory writes)
// ---------------------------------------------------------------------
//  SAFETY: this file contains NO privileged instruction.  It only fills
//  ordinary memory with page-table entries.  Nothing here sets nCR3,
//  touches CR3, or executes any VM instruction.  It is therefore fully
//  verifiable on any machine by inspecting the bytes it produces.
//
//  STRATEGY: 2 MB LARGE PAGES
//    A 128 MiB guest is covered by exactly 64 large pages.  That needs
//    just three table levels:
//
//        nPML4E[0] -> nPDPTE[0] -> nPDE[0..63] -> (2 MiB each)
//
//    No nPTE level at all, because every nPDE has PS=1.  This keeps the
//    whole structure to 3 pages for a 128 MiB guest, and it avoids the
//    enormous nPTE arrays a 4 KiB-granular mapping would need.
//
//  THE ONE INVARIANT THAT IS EASY TO GET WRONG
//    Every table-pointer entry stores the PHYSICAL ADDRESS of the next
//    table, with the low 12 bits ZERO (they encode flags instead).  If a
//    caller passes a virtual address (which in a paged host is not
//    necessarily physical), the CPU walks into garbage.  We therefore
//    take physical addresses explicitly and assert alignment.
// =====================================================================
#ifndef NEXOS_SVM_NPT_BUILD_H
#define NEXOS_SVM_NPT_BUILD_H

#include <stdint.h>
#include "svm_npt.h"

// ---------------------------------------------------------------------
//  Result codes
// ---------------------------------------------------------------------
typedef enum {
    NPTBUILD_OK              = 0,
    NPTBUILD_ERR_NULL        = -1,   // a required pointer was NULL
    NPTBUILD_ERR_ALIGN       = -2,   // a table base was not 4 KiB aligned
    NPTBUILD_ERR_TOO_SMALL   = -3,   // guest memory smaller than one page
    NPTBUILD_ERR_TOO_LARGE   = -4,   // exceeds what the tables can hold
} nptbuild_result;

// ---------------------------------------------------------------------
//  Layout descriptor: where the three tables live, and what they map.
//
//   pml4_pa / pdpte_pa / pde_pa  must each be 4 KiB aligned PHYSICAL
//                                addresses of one free page.
//   guest_phys_base              host physical address backing GPA 0.
//                                Must be 2 MiB aligned (large-page rule).
//   guest_size                   bytes of guest RAM to map.
// ---------------------------------------------------------------------
typedef struct {
    uint64_t pml4_pa;
    uint64_t pdpte_pa;
    uint64_t pde_pa;
    uint64_t guest_phys_base;
    uint64_t guest_size;
} npt_layout;

// How many bytes of guest RAM a single 2 MB PDE chain can cover before
// needing a deeper tree.  (Here: 512 entries * 2 MiB = 1 GiB.)
#define NPT_MAX_GUEST_WITH_ONE_PDPTE \
    (NPT_ENTRIES_PER_TABLE * (uint64_t)NPT_LARGE_PAGE_SIZE)

// Fill the tables.  Returns NPTBUILD_OK or an error.
// Writes only to the three pages named in `lay`; never touches CPU state.
int npt_build(const npt_layout* lay);

const char* nptbuild_str(int rc);

#endif // NEXOS_SVM_NPT_BUILD_H
