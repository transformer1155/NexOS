// =====================================================================
//  svm_npt_build.c  -  NPT construction (see header for the safety note)
// ---------------------------------------------------------------------
//  NOTHING PRIVILEGED HERE.  Pure stores into three ordinary pages.
//
//  Because the tables are at known physical addresses and, in NexOS's
//  flat identity-mapped model, PA is directly dereferenceable, we write
//  through the same address.  On a host with a real MMU offset the
//  caller must supply an alias -- that is the caller's contract, not
//  something this file can hide.
// =====================================================================
#include "svm_npt_build.h"

// ---------------------------------------------------------------------
//  Local helpers (no libc in the freestanding kernel build)
// ---------------------------------------------------------------------
static void npt_zero(void* p, uint64_t n)
{
    uint8_t* d = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

static void npt_store_entry(uint64_t table_pa, uint32_t index, uint64_t value)
{
    // Tables are written through their physical address (flat model).
    uint64_t* t = (uint64_t*)(uintptr_t)table_pa;
    t[index] = value;
}

// ---------------------------------------------------------------------
int npt_build(const npt_layout* lay)
{
    if (!lay) return NPTBUILD_ERR_NULL;

    // ---- validate alignment (low 12 bits must be clear) ----
    if ((lay->pml4_pa        & 0xFFFull) != 0) return NPTBUILD_ERR_ALIGN;
    if ((lay->pdpte_pa       & 0xFFFull) != 0) return NPTBUILD_ERR_ALIGN;
    if ((lay->pde_pa         & 0xFFFull) != 0) return NPTBUILD_ERR_ALIGN;
    // A 2 MiB large page must start on a 2 MiB boundary.
    if ((lay->guest_phys_base & (NPT_LARGE_PAGE_SIZE - 1)) != 0)
        return NPTBUILD_ERR_ALIGN;

    if (lay->guest_size < NPT_PAGE_SIZE) return NPTBUILD_ERR_TOO_SMALL;

    // ---- how many 2 MiB PDEs are needed ----
    uint64_t count = (lay->guest_size + NPT_LARGE_PAGE_SIZE - 1)
                     / NPT_LARGE_PAGE_SIZE;

    // A single nPDPTE covers 512 * 2 MiB = 1 GiB.  Beyond that a deeper
    // tree would be required; refuse rather than silently mis-map.
    if (count > NPT_ENTRIES_PER_TABLE) return NPTBUILD_ERR_TOO_LARGE;

    // ---- clear all three pages ----
    npt_zero((void*)(uintptr_t)lay->pml4_pa,  NPT_PAGE_SIZE);
    npt_zero((void*)(uintptr_t)lay->pdpte_pa, NPT_PAGE_SIZE);
    npt_zero((void*)(uintptr_t)lay->pde_pa,   NPT_PAGE_SIZE);

    // ---- nPML4E[0] -> nPDPTE ----
    //   A table-pointer entry carries the next table's PA plus flags.
    //   NPT tables are walked by the CPU in host physical space, and
    //   nested paging permissions do NOT use the U/S or R/W bits the way
    //   normal paging does: the guest's own page tables already decide
    //   user/supervisor, so the NPT leaf controls only physical access.
    //   We mark tables present + writable so the guest may write its RAM.
    npt_store_entry(lay->pml4_pa, 0,
                    NPT_ENTRY_PFN(lay->pdpte_pa) | NPT_PRESENT | NPT_WRITE);

    // ---- nPDPTE[0] -> nPDE ----
    npt_store_entry(lay->pdpte_pa, 0,
                    NPT_ENTRY_PFN(lay->pde_pa) | NPT_PRESENT | NPT_WRITE);

    // ---- nPDE[i]: 2 MiB large pages covering the guest ----
    for (uint64_t i = 0; i < count; i++) {
        uint64_t hpa = lay->guest_phys_base + i * NPT_LARGE_PAGE_SIZE;
        // PS=1 (NPT_LARGE) marks this as a leaf mapping 2 MiB.
        npt_store_entry(lay->pde_pa, (uint32_t)i,
                        NPT_ENTRY_PFN(hpa) | NPT_PRESENT | NPT_WRITE | NPT_LARGE);
    }

    return NPTBUILD_OK;
}

const char* nptbuild_str(int rc)
{
    switch (rc) {
        case NPTBUILD_OK:              return "ok";
        case NPTBUILD_ERR_NULL:        return "null layout pointer";
        case NPTBUILD_ERR_ALIGN:       return "table base or guest base misaligned";
        case NPTBUILD_ERR_TOO_SMALL:   return "guest memory smaller than one page";
        case NPTBUILD_ERR_TOO_LARGE:   return "guest memory exceeds one nPDPTE (1 GiB)";
        default:                       return "unrecognised result";
    }
}