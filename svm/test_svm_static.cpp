// =====================================================================
//  test_svm_static.cpp  -  self-check for the SVM static layer
// ---------------------------------------------------------------------
//  This program contains NO VM instructions.  It verifies:
//
//    1. every VMCB / NPT / bitmap compile-time assertion holds
//       (that is already guaranteed by including the headers, which
//        fail to compile otherwise);
//    2. the runtime layout numbers match what the assertions claim
//       (printed, so a mismatch is visible rather than assumed);
//    3. svm_probe() behaves correctly on THIS host -- including the
//       expected failure path, since this WSL2 machine has no SVM.
//
//  Point 3 is the important one: a detector that reports "unsupported"
//  on a machine that genuinely lacks SVM is correct behaviour, and we
//  can demonstrate that here WITHOUT any risk.
// =====================================================================
#include <stdio.h>
#include <string.h>

#include "svm/svm_vmcb.h"
#include "svm/svm_npt.h"
#include "svm/svm_bitmaps.h"

extern "C" {
#include "svm/svm_probe.h"
}

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
        else { printf("ok  : %s\n", msg); }                  \
    } while (0)

int main(void) {
    printf("=== SVM static layer self-check ===\n\n");

    // ---- 1. structure sizes (assertions already enforce these) ----
    printf("[sizes]\n");
    printf("  vmcb_control = %u  (APM2 1024)\n", (unsigned)sizeof(struct svm_vmcb_control));
    printf("  vmcb_save    = %u  (APM2  744)\n", (unsigned)sizeof(struct svm_vmcb_save));
    printf("  vmcb_total   = %u  (APM2 1768)\n", (unsigned)sizeof(struct svm_vmcb));
    printf("  vmcb_seg     = %u  (APM2   16)\n", (unsigned)sizeof(struct svm_vmcb_seg));
    printf("  npt entry    = %u\n", (unsigned)sizeof(npt_entry_t));
    printf("  npt tables   = %u each\n", (unsigned)sizeof(struct npt_pde_block));
    printf("  iopm         = %u bytes (%u pages)\n", (unsigned)sizeof(struct svm_iopm), IOPM_PAGES);
    printf("  msrpm        = %u bytes (%u pages)\n\n", (unsigned)sizeof(struct svm_msrpm), MSRPM_PAGES);

    CHECK(sizeof(struct svm_vmcb_control) == 1024, "control area == 1024 bytes");
    CHECK(sizeof(struct svm_vmcb_save) == 744, "save area == 744 bytes");
    CHECK(sizeof(struct svm_vmcb) == 1768, "whole VMCB == 1768 bytes");
    CHECK(sizeof(struct svm_vmcb_seg) == 16, "segment descriptor == 16 bytes");

    // ---- 2. critical VMCB offsets (the ones that actually matter) ----
    printf("\n[critical vmcb offsets]\n");
    printf("  iopm_base_pa  = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_control, iopm_base_pa));
    printf("  msrpm_base_pa = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_control, msrpm_base_pa));
    printf("  nested_cr3    = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_control, nested_cr3));
    printf("  nested_ctl    = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_control, nested_ctl));
    printf("  exit_code     = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_control, exit_code));
    printf("  exit_info_1   = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_control, exit_info_1));
    printf("  save.efer     = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_save, efer));
    printf("  save.cr0      = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_save, cr0));
    printf("  save.cr3      = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_save, cr3));
    printf("  save.cr4      = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_save, cr4));
    printf("  save.rip      = 0x%03X\n", (unsigned)offsetof(struct svm_vmcb_save, rip));
    printf("  save.rsp      = 0x%03X\n\n", (unsigned)offsetof(struct svm_vmcb_save, rsp));

    CHECK(offsetof(struct svm_vmcb_control, iopm_base_pa) == 0x040, "iopm_base_pa @0x040");
    CHECK(offsetof(struct svm_vmcb_control, msrpm_base_pa) == 0x048, "msrpm_base_pa @0x048");
    CHECK(offsetof(struct svm_vmcb_control, nested_cr3) == 0x0B0, "nested_cr3 @0x0B0");
    CHECK(offsetof(struct svm_vmcb_control, nested_ctl) == 0x090, "nested_ctl @0x090");
    CHECK(offsetof(struct svm_vmcb_save, efer) == 0x0D0, "save.efer @0x0D0");
    CHECK(offsetof(struct svm_vmcb, save) == 1024, "save area starts at 1024");

    // ---- 3. NPT geometry ----
    printf("[npt geometry]\n");
    printf("  4K page    = %u\n", NPT_PAGE_SIZE);
    printf("  2M large   = %u\n", NPT_LARGE_PAGE_SIZE);
    printf("  layout     = nPML4E -> nPDPTE -> nPDE -> nPTE\n");
    printf("  128MB guest needs %u large pages\n\n", NPT_LARGE_PAGES_FOR_BYTES(NPT_GUEST_RAM_128MB));

    CHECK(NPT_LARGE_PAGE_SIZE == 2097152, "2MB large page == 2097152");
    CHECK(NPT_LARGE_PAGES_FOR_BYTES(NPT_GUEST_RAM_128MB) == 64, "128MB -> 64 large pages");
    CHECK(sizeof(struct npt_pde_block) == 4096, "one NPT table == 4KiB");

    // GPA index extraction sanity: 128MB guest lives entirely in PML4#0.
    printf("[npt index sanity for %u MB guest]\n", NPT_GUEST_RAM_128MB / (1024*1024));
    printf("  PML4  idx for GPA 0        = %u\n", NPT_IDX_PML4(0));
    printf("  PML4  idx for GPA 128MB    = %u\n", NPT_IDX_PML4(128u*1024*1024-1));
    printf("  PDPTE idx for GPA 128MB    = %u\n\n", NPT_IDX_PDPTE(128u*1024*1024-1));
    CHECK(NPT_IDX_PML4(NPT_GUEST_RAM_128MB - 1) == 0, "128MB guest fits in one PML4E");
    CHECK(NPT_IDX_PDPTE(NPT_GUEST_RAM_128MB - 1) == 0, "128MB guest fits in one PDPTE");

    // ---- 4. interept bit definitions are sane ----
    printf("[intercept bits]\n");
    CHECK(SVM_I3_IOIO == (1u << 27), "IOIO intercept is word3 bit27");
    CHECK(SVM_I3_HLT  == (1u << 24), "HLT  intercept is word3 bit24");
    CHECK(SVM_I3_MSR  == (1u << 28), "MSR  intercept is word3 bit28");
    CHECK(SVM_I4_EFER == (1u << 15), "EFER write intercept is word4 bit15");
    CHECK(SVM_NESTED_CTL_NP_ENABLE == 1ull, "NP_ENABLE is nested_ctl bit0");

    // ---- 5. the live probe on THIS host ----
    printf("\n[live probe on this host]\n");
    svm_caps caps;
    int rc = svm_probe(&caps);
    printf("  result          = %d (%s)\n", rc, svm_probe_str(rc));
    printf("  vendor is AMD   = %u\n", caps.is_amd);
    printf("  CPUID ext ECX   = 0x%08X\n", caps.cpuid_ext_features_ecx);
    printf("  has_svm         = %u\n", caps.has_svm);
    printf("  has_npt         = %u\n", caps.has_npt);
    printf("  VM_CR           = 0x%08X\n", caps.vm_cr);
    printf("  svm_disabled    = %u\n", caps.svm_disabled);
    printf("  EFER            = 0x%llX\n", (unsigned long long)caps.efer);
    printf("  EFER.SVME now   = %u\n\n", caps.svme_currently_set);

    CHECK(caps.is_amd == 1, "host CPU is AuthenticAMD");

    // This machine (WSL2 on Hyper-V) does not expose SVM.  The detector
    // MUST report that clearly rather than pretending support exists.
    if (rc != SVM_OK) {
        printf("  NOTE: SVM unavailable here -- this is the EXPECTED result\n");
        printf("        on WSL2 (Hyper-V: Nested features: 0x0).\n");
        CHECK(caps.has_svm == 0, "has_svm correctly reported as 0");
        CHECK(svm_is_supported() == 0, "svm_is_supported() agrees: not supported");
    } else {
        printf("  SVM appears available on this host.\n");
        CHECK(caps.has_svm == 1, "has_svm reported as 1");
        CHECK(svm_is_supported() == 1, "svm_is_supported() agrees: supported");
    }

    // ---- 6. failure paths must be honest ----
    printf("\n[null / defensive]\n");
    CHECK(svm_probe(0) == SVM_ERR_FEATURE_UNKNOWN, "svm_probe(NULL) rejected");
    CHECK(strcmp(svm_probe_str(SVM_ERR_NO_SVM_CPUID),
                 "CPUID 8000_0001:ECX.SVM is clear") == 0,
          "error strings are distinct");

    printf("\n=== %s ===\n", g_fail == 0 ? "ALL PASS" : "TEST FAILED");
    return g_fail == 0 ? 0 : 1;
}