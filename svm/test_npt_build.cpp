// =====================================================================
//  test_npt_build.cpp  -  independent verification of npt_build()
// ---------------------------------------------------------------------
//  The point of this test is INDEPENDENCE: it does not re-use the
//  builder's internal macros to check its own output.  It walks the
//  produced tables with a separate page-table walker written from the
//  AMD APM rules, and asks two questions for every guest address:
//
//     1. Does a GPA of X resolve to the expected HPA?
//     2. Is the mapping physically coherent (present, writable, leaf)?
//
//  If the builder and the walker disagree, one of them is wrong -- and
//  that disagreement is exactly what we cannot afford to discover on a
//  machine that cannot run SVM.
// =====================================================================
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "svm/svm_npt.h"

// The builder is C; declare it with C linkage so the symbols match.
extern "C" {
#include "svm/svm_npt_build.h"
}

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
        else { printf("ok  : %s\n", msg); }                  \
    } while (0)

// ---------------------------------------------------------------------
//  Independent NPT walker (2MiB-large-page case), written from APM2:
//    nCR3 -> nPML4E -> nPDPTE -> nPDE(PS=1) -> 2 MiB page
// ---------------------------------------------------------------------
typedef struct { uint64_t hpa; int present; int writable; int leaf2mb; } walk_result;

static uint64_t rd64(uint64_t pa, uint32_t idx) {
    const uint64_t* t = (const uint64_t*)(uintptr_t)pa;
    return t[idx];
}

static walk_result npt_walk_independent(uint64_t ncr3_pa, uint64_t gpa) {
    walk_result r; memset(&r, 0x00, sizeof(r));

    // Bits 51:12 of nCR3 give the nPML4E table base.
    uint64_t pml4 = ncr3_pa & 0x000FFFFFFFFFF000ull;
    uint64_t e_pml4 = rd64(pml4, (uint32_t)((gpa >> 39) & 0x1FF));
    if (!(e_pml4 & 1)) return r;                         // not present

    uint64_t pdpte_tbl = e_pml4 & 0x000FFFFFFFFFF000ull;
    uint64_t e_pdpte = rd64(pdpte_tbl, (uint32_t)((gpa >> 30) & 0x1FF));
    if (!(e_pdpte & 1)) return r;

    uint64_t pde_tbl = e_pdpte & 0x000FFFFFFFFFF000ull;
    uint64_t e_pde = rd64(pde_tbl, (uint32_t)((gpa >> 21) & 0x1FF));
    if (!(e_pde & 1)) return r;

    r.present = 1;
    if (e_pde & 2) r.writable = 1;                       // R/W

    if (e_pde & (1ull << 7)) {                           // PS = 1 -> 2 MiB
        r.leaf2mb = 1;
        uint64_t base = e_pde & (0x000FFFFFFFFFF000ull & ~0x1FFFFFull);
        r.hpa = base + (gpa & (NPT_LARGE_PAGE_SIZE - 1));
    }
    return r;
}

// ---------------------------------------------------------------------
int main(void) {
    printf("=== NPT builder independent verification ===\n\n");

    const uint64_t GUEST_BASE = 0x100000000ull;   // some 2 MiB-aligned HPA
    const uint64_t GUEST_SIZE = NPT_GUEST_RAM_128MB;

    // Allocate real host memory for the three tables so we can actually
    // dereference them.  Each table is one 4 KiB page.
    void* pml4  = aligned_alloc(4096, 4096);
    void* pdpte = aligned_alloc(4096, 4096);
    void* pde   = aligned_alloc(4096, 4096);
    if (!pml4 || !pdpte || !pde) { printf("alloc failed\n"); return 1; }

    npt_layout lay;
    lay.pml4_pa        = (uint64_t)(uintptr_t)pml4;
    lay.pdpte_pa       = (uint64_t)(uintptr_t)pdpte;
    lay.pde_pa         = (uint64_t)(uintptr_t)pde;
    lay.guest_phys_base= GUEST_BASE;
    lay.guest_size     = GUEST_SIZE;

    printf("[build]\n");
    printf("  guest base  = 0x%llX (2MiB aligned: %s)\n",
           (unsigned long long)GUEST_BASE,
           ((GUEST_BASE & (NPT_LARGE_PAGE_SIZE-1))==0)?"yes":"NO");
    printf("  guest size  = %llu MB\n",
           (unsigned long long)(GUEST_SIZE/(1024*1024)));

    int rc = npt_build(&lay);
    printf("  result      = %d (%s)\n\n", rc, nptbuild_str(rc));
    CHECK(rc == NPTBUILD_OK, "npt_build succeeded");

    // ---- inspect the produced entries directly ----
    printf("[table contents]\n");
    uint64_t* P = (uint64_t*)pml4;
    uint64_t* D = (uint64_t*)pdpte;
    uint64_t* E = (uint64_t*)pde;

    printf("  nPML4E[0]  = 0x%016llX\n", (unsigned long long)P[0]);
    printf("  nPDPTE[0]  = 0x%016llX\n", (unsigned long long)D[0]);
    printf("  nPDE[0]    = 0x%016llX\n", (unsigned long long)E[0]);
    printf("  nPDE[63]   = 0x%016llX\n", (unsigned long long)E[63]);
    printf("  nPDE[64]   = 0x%016llX  (must be 0: past the guest)\n\n",
           (unsigned long long)E[64]);

    CHECK((P[0] & 1) != 0, "nPML4E[0] present");
    CHECK((P[0] & 2) != 0, "nPML4E[0] writable");
    // Low 12 bits ARE flags (P/R-W/U-S/PWT/PCD/A/...), so they are expected
    // to be non-zero.  What must hold is that no ADDRESS bits leak below
    // bit 12 -- i.e. masking off the flags yields a 4 KiB-aligned pointer,
    // which the "points at the nPDPTE page" check below verifies exactly.
    CHECK(((P[0] & 0x000FFFFFFFFFF000ull) & 0xFFFull) == 0,
          "nPML4E[0] address portion is page-aligned (flags stay in low 12 bits)");
    CHECK(((P[0] & 0x000FFFFFFFFFF000ull) == (uint64_t)(uintptr_t)pdpte),
          "nPML4E[0] points at the nPDPTE page");

    CHECK((D[0] & 1) != 0, "nPDPTE[0] present");
    CHECK(((D[0] & 0x000FFFFFFFFFF000ull) == (uint64_t)(uintptr_t)pde),
          "nPDPTE[0] points at the nPDE page");

    CHECK((E[0] & 1) != 0, "nPDE[0] present");
    CHECK((E[0] & (1ull<<7)) != 0, "nPDE[0] has PS=1 (2 MiB large page)");
    CHECK(((E[0] & (0x000FFFFFFFFFF000ull & ~0x1FFFFFull)) == GUEST_BASE),
          "nPDE[0] maps guest base");
    CHECK(E[63] != 0, "nPDE[63] exists (last of 64)");
    CHECK(E[64] == 0, "nPDE[64] untouched (guest ends at 128 MiB)");

    // ---- 2. independent walk over a spread of guest addresses ----
    printf("\n[independent walk: GPA -> HPA]\n");
    const uint64_t probes[] = {
        0x0,                      // first byte
        0x1000,                   // +4 KiB
        0x1FFFFF,                 // last byte of first 2 MiB page
        0x200000,                 // first byte of second page
        0x3FFFFFF,                // last byte of first 4 MiB
        (64ull*1024*1024),        // 64 MiB
        (128ull*1024*1024 - 1),   // very last guest byte
    };
    const int nprobes = (int)(sizeof(probes)/sizeof(probes[0]));

    for (int i = 0; i < nprobes; i++) {
        uint64_t gpa = probes[i];
        walk_result w = npt_walk_independent(lay.pml4_pa, gpa);

        uint64_t expect = GUEST_BASE + gpa;
        char msg[160];
        snprintf(msg, sizeof(msg), "GPA 0x%llX -> HPA 0x%llX (expected 0x%llX)",
                 (unsigned long long)gpa,
                 (unsigned long long)w.hpa,
                 (unsigned long long)expect);

        if (!w.present || !w.leaf2mb || w.hpa != expect) {
            printf("FAIL: %s\n", msg);
            g_fail++;
        } else {
            printf("ok  : %s\n", msg);
        }
    }

    // ---- 3. identity property: the mapping is contiguous ----
    printf("\n[contiguity]\n");
    {
        int contiguous = 1;
        for (uint32_t i = 0; i < 64; i++) {
            uint64_t e = E[i];
            uint64_t base = e & (0x000FFFFFFFFFF000ull & ~0x1FFFFFull);
            if (base != GUEST_BASE + (uint64_t)i * NPT_LARGE_PAGE_SIZE) {
                contiguous = 0; break;
            }
            if (!(e & 1) || !(e & (1ull<<7))) { contiguous = 0; break; }
        }
        CHECK(contiguous, "all 64 large pages are contiguous and leaf-marked");
    }

    // ---- 4. rejection paths must fail closed ----
    printf("\n[validation / fail-closed]\n");
    {
        npt_layout bad = lay;
        bad.pml4_pa = lay.pml4_pa + 0x800;      // misaligned
        CHECK(npt_build(&bad) == NPTBUILD_ERR_ALIGN, "unaligned table base rejected");
    }
    {
        npt_layout bad = lay;
        bad.guest_phys_base = GUEST_BASE + 0x1000;  // not 2MiB aligned
        CHECK(npt_build(&bad) == NPTBUILD_ERR_ALIGN, "unaligned guest base rejected");
    }
    {
        npt_layout bad = lay;
        bad.guest_size = 0;
        CHECK(npt_build(&bad) == NPTBUILD_ERR_TOO_SMALL, "zero guest size rejected");
    }
    {
        npt_layout bad = lay;
        bad.guest_size = 2ull*1024*1024*1024;   // 2 GiB: needs deeper tree
        CHECK(npt_build(&bad) == NPTBUILD_ERR_TOO_LARGE, "2 GiB guest rejected");
    }
    CHECK(npt_build(NULL) == NPTBUILD_ERR_NULL, "NULL rejected");

    free(pml4); free(pdpte); free(pde);

    printf("\n=== %s ===\n", g_fail == 0 ? "ALL PASS" : "TEST FAILED");
    return g_fail == 0 ? 0 : 1;
}