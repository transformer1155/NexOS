// =====================================================================
//  test_vmcb_init.cpp  -  verify the initialised VMCB against APM2
// ---------------------------------------------------------------------
//  This inspects the built VMCB field by field and checks every property
//  that would otherwise only surface as a #VMFAIL on real hardware.
//
//  It also DUMPS the whole structure, because with no test machine the
//  hex dump is the artifact a reviewer (or a future debugger) compares
//  against the APM tables.
// =====================================================================
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "svm/svm_vmcb.h"
#include "svm/svm_bitmaps.h"

extern "C" {
#include "svm/svm_vmcb_init.h"
#include "svm/svm_probe.h"
}

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
        else { printf("ok  : %s\n", msg); }                  \
    } while (0)

// ---------------------------------------------------------------------
static const char* efer_name(uint64_t e)
{
    static char buf[128];
    buf[0] = 0;
    if (e & (1ull<<8))  strcat(buf, "LME ");
    if (e & (1ull<<10)) strcat(buf, "LMA ");
    if (e & (1ull<<11)) strcat(buf, "NXE ");
    if (e & (1ull<<12)) strcat(buf, "SVME ");
    if (!buf[0]) strcpy(buf, "(none)");
    return buf;
}

static void dump_bytes(const char* label, const void* p, uint32_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    printf("  %s (%u bytes)\n", label, (unsigned)n);
    for (uint32_t i = 0; i < n; i += 16) {
        printf("    %04X:", i);
        for (uint32_t j = 0; j < 16 && i + j < n; j++)
            printf(" %02X", b[i + j]);
        printf("\n");
    }
}

int main(void) {
    printf("=== VMCB init verification ===\n\n");

    // Real backing memory for the bitmaps and the NPT root.  The VMCB
    // requires page-aligned PHYSICAL addresses for all three, so the test
    // must supply genuinely aligned memory -- a plain `static` struct lands
    // wherever the linker puts it (observed at ...E0), and the initialiser
    // correctly refused it.  That refusal is the behaviour we want; the
    // fix belongs here in the test harness.
    static struct svm_iopm  iopm  __attribute__((aligned(4096)));
    static struct svm_msrpm msrpm __attribute__((aligned(4096)));
    static uint64_t ncr3[512] __attribute__((aligned(4096)));

    printf("[inputs]\n");
    printf("  iopm   @ 0x%llX (%u bytes, aligned=%s)\n",
           (unsigned long long)(uintptr_t)&iopm, (unsigned)sizeof(iopm),
           (((uintptr_t)&iopm & 0xFFF) == 0) ? "yes" : "no");
    printf("  msrpm  @ 0x%llX (%u bytes)\n",
           (unsigned long long)(uintptr_t)&msrpm, (unsigned)sizeof(msrpm));
    printf("  ncr3   @ 0x%llX\n\n", (unsigned long long)(uintptr_t)ncr3);

    // Fill the bitmaps maximally interceptive first.
    iopm_fill_all(&iopm);
    msrpm_fill_all(&msrpm);

    // ---- bitmap contents must be all-ones ----
    printf("[bitmaps after fill_all]\n");
    {
        int all_ff = 1;
        for (uint32_t i = 0; i < IOPM_BYTES; i++) if (iopm.bytes[i] != 0xFF) { all_ff = 0; break; }
        CHECK(all_ff, "IOPM: every port intercepted");
    }
    {
        int all_ff = 1;
        for (uint32_t i = 0; i < MSRPM_BYTES; i++) if (msrpm.bytes[i] != 0xFF) { all_ff = 0; break; }
        CHECK(all_ff, "MSRPM: every MSR read+write intercepted");
    }

    // ---- build the VMCB ----
    struct svm_vmcb v;
    vmcbinit_params p;
    memset(&p, 0x00, sizeof(p));
    p.iopm_pa          = (uint64_t)(uintptr_t)&iopm;
    p.msrpm_pa         = (uint64_t)(uintptr_t)&msrpm;
    p.ncr3_pa          = (uint64_t)(uintptr_t)ncr3;
    p.guest_rip        = 0x0000000000100000ull;   // conventional Linux entry
    p.guest_rsp        = 0x0000000000800000ull;   // inside the guest's RAM
    p.guest_cr3        = 0x0000000000100000ull;   // guest's own PGD (GPA)
    p.guest_ram_bytes  = 128ull * 1024 * 1024;

    int rc = vmcb_init_linux64(&v, &p);
    printf("\n[build]\n  result = %d (%s)\n\n", rc, vmcbinit_str(rc));
    CHECK(rc == VMCBINIT_OK, "vmcb_init_linux64 succeeded");

    struct svm_vmcb_control* c = &v.control;
    struct svm_vmcb_save*    s = &v.save;

    // ---- intercept words ----
    printf("[intercepts]\n");
    printf("  word0 CR        = 0x%08X\n", c->intercepts[0]);
    printf("  word1 DR        = 0x%08X\n", c->intercepts[1]);
    printf("  word2 EXC       = 0x%08X\n", c->intercepts[2]);
    printf("  word3 general   = 0x%08X\n", c->intercepts[3]);
    printf("  word4 general2  = 0x%08X\n", c->intercepts[4]);
    printf("  word5 general3  = 0x%08X\n\n", c->intercepts[5]);

    CHECK((c->intercepts[3] & SVM_I3_IOIO) != 0, "IOIO intercepted (serial/UART path)");
    CHECK((c->intercepts[3] & SVM_I3_MSR)  != 0, "MSR access intercepted");
    CHECK((c->intercepts[3] & SVM_I3_HLT)  != 0, "HLT intercepted (guest idle)");
    CHECK((c->intercepts[3] & SVM_I3_CPUID)!= 0, "CPUID intercepted (hide host features)");
    CHECK((c->intercepts[3] & SVM_I3_SHUTDOWN)!=0, "SHUTDOWN intercepted (triple fault)");
    CHECK((c->intercepts[4] & SVM_I4_EFER) != 0, "EFER write intercepted");
    CHECK((c->intercepts[4] & SVM_I4_VMRUN)!= 0, "VMRUN intercepted (no nested hypervisor)");
    CHECK((c->intercepts[0] & SVM_IC_CR3_WRITE) != 0, "CR3 write intercepted");
    CHECK((c->intercepts[0] & SVM_IC_CR0_WRITE) != 0, "CR0 write intercepted");
    CHECK((c->intercepts[2] & SVM_EXC_PAGE_FAULT) == 0,
          "PAGE FAULT NOT intercepted (guest must handle its own #PF)");

    // ---- bitmaps / NPT wiring ----
    printf("[control wiring]\n");
    printf("  iopm_base_pa  = 0x%llX\n", (unsigned long long)c->iopm_base_pa);
    printf("  msrpm_base_pa = 0x%llX\n", (unsigned long long)c->msrpm_base_pa);
    printf("  nested_ctl    = 0x%llX\n", (unsigned long long)c->nested_ctl);
    printf("  nested_cr3    = 0x%llX\n", (unsigned long long)c->nested_cr3);
    printf("  asid          = %u\n\n", c->asid);

    CHECK(c->iopm_base_pa  == (uint64_t)(uintptr_t)&iopm,  "iopm_base_pa points at the IOPM");
    CHECK(c->msrpm_base_pa == (uint64_t)(uintptr_t)&msrpm, "msrpm_base_pa points at the MSRPM");
    CHECK(c->nested_cr3    == (uint64_t)(uintptr_t)ncr3,   "nested_cr3 points at the nPML4E");
    CHECK((c->nested_ctl & SVM_NESTED_CTL_NP_ENABLE) != 0, "NP_ENABLE set (nested paging on)");
    CHECK((c->nested_ctl & ~SVM_NESTED_CTL_NP_ENABLE) == 0,
          "no unexpected bits in nested_ctl (SEV must stay off)");
    CHECK(c->asid != 0, "ASID is non-zero (0 is reserved)");

    // ---- EFER ----
    printf("[save area: long mode state]\n");
    printf("  efer  = 0x%llX  -> %s\n", (unsigned long long)s->efer, efer_name(s->efer));
    printf("  cr0   = 0x%llX\n", (unsigned long long)s->cr0);
    printf("  cr4   = 0x%llX\n", (unsigned long long)s->cr4);
    printf("  cr3   = 0x%llX\n", (unsigned long long)s->cr3);
    printf("  rip   = 0x%llX\n", (unsigned long long)s->rip);
    printf("  rsp   = 0x%llX\n", (unsigned long long)s->rsp);
    printf("  rflg  = 0x%llX\n", (unsigned long long)s->rflags);
    printf("  cpl   = %u\n\n", s->cpl);

    CHECK((s->efer & EFER_LME) != 0, "EFER.LME set (long mode enable)");
    CHECK((s->efer & EFER_LMA) != 0, "EFER.LMA set (long mode active)");
    CHECK((s->efer & EFER_NXE) != 0, "EFER.NXE set (no-execute support)");
    // The single most dangerous confusion: guest SVME must be CLEAR.
    CHECK((s->efer & EFER_SVME) == 0,
          "EFER.SVME CLEAR in guest (SVME belongs to the HOST, not the guest)");

    CHECK((s->cr0 & (1ull<<0))  != 0, "CR0.PE set (protected mode)");
    CHECK((s->cr0 & (1ull<<31)) != 0, "CR0.PG set (paging on)");
    CHECK((s->cr4 & (1ull<<5))  != 0, "CR4.PAE set (required for long mode)");
    CHECK(s->cr3 == p.guest_cr3, "CR3 = guest's own page-table root");

    // ---- segments ----
    printf("[save area: segments]\n");
    printf("  cs.sel=%04X attrib=%04X limit=%08X base=%llX\n",
           s->cs.selector, s->cs.attrib, s->cs.limit, (unsigned long long)s->cs.base);
    printf("  ss.sel=%04X attrib=%04X\n", s->ss.selector, s->ss.attrib);
    printf("  tr.sel=%04X attrib=%04X limit=%08X\n",
           s->tr.selector, s->tr.attrib, s->tr.limit);
    printf("\n");
    CHECK((s->cs.attrib & SEG_ATTRIB_P) != 0, "CS attrib present");
    CHECK((s->cs.attrib & SEG_ATTRIB_L) != 0, "CS attrib L=1 (64-bit code segment)");
    CHECK((s->cs.attrib & SEG_ATTRIB_DB) == 0,
          "CS attrib D/B clear (L and D/B are mutually exclusive)");
    CHECK(s->cs.attrib == SVM_ATTRIB_CODE64, "CS attrib equals SVM_ATTRIB_CODE64");
    CHECK((s->cs.base) == 0,             "CS base 0 (flat)");
    CHECK((s->ds.attrib & SEG_ATTRIB_P) != 0, "DS attrib present");
    CHECK(s->ds.attrib == SVM_ATTRIB_DATA64, "DS attrib equals SVM_ATTRIB_DATA64");
    CHECK((s->tr.attrib & SEG_ATTRIB_P) != 0, "TR attrib present");
    CHECK((s->tr.attrib & SEG_ATTRIB_S) == 0, "TR is a SYSTEM segment (S=0)");
    CHECK((s->tr.limit) >= 0x67,         "TR limit >= 0x67 (long-mode TSS minimum)");

    // ---- PAT must be the architectural reset value ----
    CHECK(s->g_pat == 0x0007040600070406ull, "g_pat is the legal reset value");

    // ---- rflags reserved bit ----
    CHECK((s->rflags & 0x2) != 0, "RFLAGS bit1 (reserved, always 1) set");

    // ---- fail-closed paths ----
    printf("[fail-closed]\n");
    {
        vmcbinit_params bad = p;
        bad.iopm_pa += 0x800;
        CHECK(vmcb_init_linux64(&v, &bad) == VMCBINIT_ERR_ALIGN, "misaligned iopm rejected");
    }
    {
        vmcbinit_params bad = p;
        bad.ncr3_pa += 0x4;
        CHECK(vmcb_init_linux64(&v, &bad) == VMCBINIT_ERR_ALIGN, "misaligned ncr3 rejected");
    }
    {
        vmcbinit_params bad = p;
        bad.guest_rip = 0;
        CHECK(vmcb_init_linux64(&v, &bad) == VMCBINIT_ERR_BAD_ARG, "zero guest_rip rejected");
    }
    {
        vmcbinit_params bad = p;
        bad.guest_ram_bytes = 0;
        CHECK(vmcb_init_linux64(&v, &bad) == VMCBINIT_ERR_BAD_ARG, "zero guest RAM rejected");
    }
    CHECK(vmcb_init_linux64(NULL, &p) == VMCBINIT_ERR_NULL, "NULL vmcb rejected");

    // ---- hex dump of the control area (the reviewable artifact) ----
    printf("\n[control area dump]\n");
    dump_bytes("control area", c, sizeof(*c));

    printf("\n=== %s ===\n", g_fail == 0 ? "ALL PASS" : "TEST FAILED");
    return g_fail == 0 ? 0 : 1;
}