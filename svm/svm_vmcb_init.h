// =====================================================================
//  svm_vmcb_init.c  -  VMCB initialisation (pure struct writes)
// ---------------------------------------------------------------------
//  SAFETY: no privileged instruction appears in this file.  It fills a
//  VMCB structure in ordinary memory.  Nothing sets nCR3, nothing writes
//  EFER, nothing executes VMRUN.  Fully inspectable and verifiable.
//
//  WHAT THIS ESTABLISHES
//    A VMCB that is internally consistent for a 64-bit Linux guest with
//    nested paging enabled:
//
//      * intercept everything that would leak host state or let the guest
//        escape: all I/O (via IOPM), all MSRs we do not emulate, HLT,
//        CPUID, EFER writes, VMRUN itself;
//      * set EFER to LME|LMA|NXE (64-bit long mode) with SVME left to the
//        HOST to enable separately;
//      * CR0.PG|PE|NE|ET for paging + protected mode;
//      * CR4.PAE required for long mode;
//      * point iopm/msrpm at their bitmaps and nested_cr3 at the nPML4E.
//
//  CRITICAL DISTINCTION -- SVME
//    EFER.SVME in the GUEST's save area must be the guest's own view; the
//    HOST enables SVM via the real EFER before VMRUN.  In the guest VMCB,
//    SVME is normally CLEAR (the guest is not itself a hypervisor).  Mixing
//    these up produces an immediate #VMFAIL.
// =====================================================================
#ifndef NEXOS_SVM_VMCB_INIT_H
#define NEXOS_SVM_VMCB_INIT_H

#include <stdint.h>
#include "svm_vmcb.h"
#include "svm_bitmaps.h"   // for struct svm_iopm / struct svm_msrpm
#include "svm_probe.h"     // for EFER_LME / LMA / NXE bit definitions

// ---------------------------------------------------------------------
//  Result codes
// ---------------------------------------------------------------------
typedef enum {
    VMCBINIT_OK            = 0,
    VMCBINIT_ERR_NULL      = -1,
    VMCBINIT_ERR_ALIGN     = -2,   // bitmap / NPT bases must be page-aligned
    VMCBINIT_ERR_BAD_ARG   = -3,   // e.g. zero-size guest or bad RIP
} vmcbinit_result;

// ---------------------------------------------------------------------
//  Everything needed to build one guest VMCB.
// ---------------------------------------------------------------------
typedef struct {
    // physical addresses of the two bitmaps and the NPT root
    uint64_t iopm_pa;         // 4 KiB aligned, 8 KiB (2 pages)
    uint64_t msrpm_pa;        // 4 KiB aligned, 12 KiB (3 pages)
    uint64_t ncr3_pa;         // 4 KiB aligned, points at nPML4E

    // guest start state
    uint64_t guest_rip;       // where the CPU begins executing
    uint64_t guest_rsp;       // initial stack pointer (guest physical)
    uint32_t guest_cr3;       // guest's own page-table root (GPA)

    // guest RAM size, used only to sanity-check the NPT root makes sense
    uint64_t guest_ram_bytes;

    // which exit code the guest gets on a triple fault is left to the
    // caller; we always intercept shutdown instead.
} vmcbinit_params;

// Build a complete, consistent VMCB into `v`.
// Returns VMCBINIT_OK or an error.  Writes only to `v`.
int vmcb_init_linux64(struct svm_vmcb* v, const vmcbinit_params* p);

const char* vmcbinit_str(int rc);

// Intercept-all bitmaps are filled by this helper: sets every bit so the
// CPU exits on ANY I/O port or ANY MSR access.  Later stages narrow it.
void iopm_fill_all(struct svm_iopm* iopm);
void msrpm_fill_all(struct svm_msrpm* msrpm);

#endif // NEXOS_SVM_VMCB_INIT_H
