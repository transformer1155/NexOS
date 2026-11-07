// =====================================================================
//  svm_npt.h  -  AMD SVM Nested Page Tables (NPT), static definitions only
// ---------------------------------------------------------------------
//  NPT gives the guest a physical address space that is independent of the
//  host's.  The guest believes its memory starts at GPA 0x00000000; the
//  CPU translates every guest access through these tables to real host
//  physical memory (HPA).
//
//  KEY POINT -- 4-level vs 2-level
//    AMD SVM nested paging uses FOUR levels (nPML4E -> nPDPTE -> nPDE ->
//    nPTE) even when the guest itself runs with PAE/2MB pages, because the
//    nCR3 always points at an nPML4E.  This is a common source of
//    confusion with Intel EPT (also 4 levels) and with ordinary 32-bit
//    paging (2 levels).
//
//  FORMAT OF EACH ENTRY
//    Every NPT entry is a standard x86 page-table entry (64-bit), plus a
//    few nested-specific bits.  The bit positions below are the ones
//    shared with normal paging because AMD defines NPT entries that way.
//
//  NO EXECUTABLE LOGIC HERE
//    Same rule as svm_vmcb.h: pure layout + compile-time checks.  Nothing
//    in this file touches CR3, nCR3, or the CPU.
// =====================================================================
#ifndef NEXOS_SVM_NPT_H
#define NEXOS_SVM_NPT_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#  define NPT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define NPT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define NPT_PACKED __attribute__((packed))

// ---------------------------------------------------------------------
//  Geometry
// ---------------------------------------------------------------------
#define NPT_PAGE_SIZE        4096u
#define NPT_PAGE_SHIFT       12
#define NPT_ENTRIES_PER_TABLE 512u     // 512 * 8 bytes = 4096
#define NPT_ENTRY_SIZE       8u

// A 4-level walk covers 2^48 bytes of guest physical address space.
#define NPT_LEVELS           4u

// 2 MB large page (nPDE.PS = 1): one nPDE maps 2^21 bytes.
#define NPT_LARGE_PAGE_SIZE  (2u * 1024u * 1024u)
#define NPT_LARGE_PAGE_SHIFT 21

// ---------------------------------------------------------------------
//  Common entry bits (x86 page-table / NPT shared encoding)
// ---------------------------------------------------------------------
#define NPT_PRESENT         (1ull << 0)    // P
#define NPT_WRITE           (1ull << 1)    // R/W : 1 = writable
#define NPT_USER            (1ull << 2)    // U/S : 1 = user accessible
#define NPT_PWT             (1ull << 3)    // write-through
#define NPT_PCD             (1ull << 4)    // cache disable
#define NPT_ACCESSED        (1ull << 5)    // A  (set by CPU)
#define NPT_DIRTY           (1ull << 6)    // D  (set by CPU; leaf only)
#define NPT_PAT             (1ull << 7)    // PAT (leaf only in 4K pages)
#define NPT_LARGE           (1ull << 7)    // PS : 1 = large page at PDE level
#define NPT_GLOBAL          (1ull << 8)    // G
#define NPT_NX              (1ull << 63)   // No-eXecute

// ---------------------------------------------------------------------
//  Physical-address mask for an entry pointing at another table/page.
//    Bits 51:12 hold the address; bit 12 must be clear in the table
//    pointers.  With PSE/PAT a 2MB entry stores bits 51:21.
// ---------------------------------------------------------------------
#define NPT_ADDR_MASK_4K    (~0xFFFull & 0x000FFFFFFFFFF000ull)
#define NPT_ADDR_MASK_2M    (~0x1FFFFFull & 0x000FFFFFFFFFF000ull)

#define NPT_PFN_MASK        NPT_ADDR_MASK_4K
#define NPT_ENTRY_PFN(e)    ((e) & NPT_ADDR_MASK_4K)
#define NPT_ENTRY_PRESENT(e) (((e) & NPT_PRESENT) != 0)

// Index extraction for a 4-level walk (guest virtual -> table indices).
// NOTE: the shifts exceed 32 bits, so the operand is cast to 64-bit first.
// Without the cast, NPT_IDX_PML4(x) on a 32-bit `unsigned` shifts by 39 and
// silently yields 0 -- the compiler warns (-Wshift-count-overflow), and a
// 0 index for every address would collapse the whole guest into PML4E[0].
#define NPT_IDX_PML4(gpa)   ((uint32_t)(((uint64_t)(gpa) >> 39) & 0x1FFu))
#define NPT_IDX_PDPTE(gpa)  ((uint32_t)(((uint64_t)(gpa) >> 30) & 0x1FFu))
#define NPT_IDX_PDE(gpa)    ((uint32_t)(((uint64_t)(gpa) >> 21) & 0x1FFu))
#define NPT_IDX_PTE(gpa)    ((uint32_t)(((uint64_t)(gpa) >> 12) & 0x1FFu))

// ---------------------------------------------------------------------
//  Table entries
// ---------------------------------------------------------------------
typedef uint64_t npt_entry_t;

// ---------------------------------------------------------------------
//  One table = 512 entries = one 4 KiB page.  These are aliases of the
//  same layout, kept distinct so the type of a pointer documents which
//  level it belongs to.
// ---------------------------------------------------------------------
struct npt_pml4e_block {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} NPT_PACKED;

struct npt_pdpte_block {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} NPT_PACKED;

struct npt_pde_block {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} NPT_PACKED;

struct npt_pte_block {
    npt_entry_t entries[NPT_ENTRIES_PER_TABLE];
} NPT_PACKED;

// Leftover bits are 52:62 (software-available) and 11:9 (software/IGB).
// Documented here so a future "reserved must be zero" check can use them.
#define NPT_SW_AVAIL_MASK   (0x7FFull << 52)

// =====================================================================
//  COMPILE-TIME VERIFICATION
// =====================================================================
NPT_STATIC_ASSERT(sizeof(npt_entry_t) == NPT_ENTRY_SIZE, "NPT entry must be 8 bytes");
NPT_STATIC_ASSERT(sizeof(struct npt_pml4e_block) == NPT_PAGE_SIZE, "nPML4E block must be 4 KiB");
NPT_STATIC_ASSERT(sizeof(struct npt_pdpte_block) == NPT_PAGE_SIZE, "nPDPTE block must be 4 KiB");
NPT_STATIC_ASSERT(sizeof(struct npt_pde_block)   == NPT_PAGE_SIZE, "nPDE block must be 4 KiB");
NPT_STATIC_ASSERT(sizeof(struct npt_pte_block)   == NPT_PAGE_SIZE, "nPTE block must be 4 KiB");

// 512 entries * 8 bytes == 4096, by construction.
NPT_STATIC_ASSERT(NPT_ENTRIES_PER_TABLE * NPT_ENTRY_SIZE == NPT_PAGE_SIZE,
                  "512 entries x 8 bytes must fill one page");

// A 2 MB large page must be exactly 512 * 4 KiB.
NPT_STATIC_ASSERT(NPT_LARGE_PAGE_SIZE == NPT_ENTRIES_PER_TABLE * NPT_PAGE_SIZE,
                  "2 MiB large page == 512 * 4 KiB");

// ---------------------------------------------------------------------
//  Capacity helper: how many 2MB large pages we need to back N bytes.
//  Pure arithmetic, evaluated at compile time when used with constants.
// ---------------------------------------------------------------------
#define NPT_LARGE_PAGES_FOR_BYTES(bytes) \
    (((bytes) + NPT_LARGE_PAGE_SIZE - 1u) / NPT_LARGE_PAGE_SIZE)

// 128 MiB guest -> 64 large pages.  64 MiB -> 32.
#define NPT_GUEST_RAM_64MB   (64u  * 1024u * 1024u)
#define NPT_GUEST_RAM_128MB  (128u * 1024u * 1024u)
NPT_STATIC_ASSERT(NPT_LARGE_PAGES_FOR_BYTES(NPT_GUEST_RAM_64MB)  == 32u,
                  "64 MiB guest needs 32 large pages");
NPT_STATIC_ASSERT(NPT_LARGE_PAGES_FOR_BYTES(NPT_GUEST_RAM_128MB) == 64u,
                  "128 MiB guest needs 64 large pages");

#endif // NEXOS_SVM_NPT_H
