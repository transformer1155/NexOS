// =====================================================================
//  svm_probe.c / svm_probe.h  -  safe AMD SVM capability detection
// ---------------------------------------------------------------------
//  DESIGN RULE (deliberate, and important)
//    This file NEVER executes VMRUN, never writes EFER.SVME, never writes
//    VM_HSAVE_PA, and never calls VMLOAD/VMSAVE/STGI/CLGI.  It only READS
//    CPU state through CPUID and RDMSR, which are harmless on every CPU.
//
//    Why: without a machine that can actually run nested SVM (this WSL2
//    host reports `Hyper-V: Nested features: 0x0`), executing VMRUN does
//    not give a useful error -- it gives a dead machine.  Detection has to
//    be provable by inspection, so it is written to fail closed and to be
//    readable line by line against the AMD APM.
//
//  WHAT "SUPPORTED" MEANS HERE
//    svm_is_supported() only answers "can this CPU run SVM at all".  It
//    does NOT promise that nested SVM is permitted from our current
//    context -- that is a second, separate question, answered by
//    svm_nested_enabled() when available.
// =====================================================================
#ifndef NEXOS_SVM_PROBE_H
#define NEXOS_SVM_PROBE_H

#include <stdint.h>

// ---------------------------------------------------------------------
//  Result codes.  Every failure is a distinct, actionable reason rather
//  than a bare -1, because on a machine with no test path the operator
//  needs to know WHICH prerequisite is missing.
// ---------------------------------------------------------------------
typedef enum {
    SVM_OK                    =  0,
    SVM_ERR_NOT_AMD           = -1,  // CPUID vendor is not AuthenticAMD
    SVM_ERR_NO_SVM_CPUID      = -2,  // CPUID 0x80000001:ECX bit 2 clear
    SVM_ERR_NO_EXT_CPUID      = -3,  // CPUID 0x80000000 lacks 0x80000001
    SVM_ERR_VM_CR_LOCK        = -4,  // VM_CR.SVMDIS = 1, BIOS locked it
    SVM_ERR_EFER_NOT_WRITABLE = -5,  // EFER.SVME cannot be set on this CPU
    SVM_ERR_FEATURE_UNKNOWN   = -6,  // reserved / unexpected state
} svm_probe_result;

// ---------------------------------------------------------------------
//  Facts gathered from CPUID and MSRs.  Filled in by svm_probe().
// ---------------------------------------------------------------------
typedef struct {
    // CPUID 0x80000001:ECX
    uint32_t cpuid_ext_features_ecx;
    // CPUID 0x8000000A (SVM feature leaf) -- only valid when SVM present
    uint32_t svm_features_eax;      // NASID (bits 31:0)
    uint32_t svm_features_edx;      // NPT / LBR / NRIPS / etc.
    // MSR_VM_CR (0xC0010114)
    uint32_t vm_cr;
    // MSR_VM_HSAVE_PA (0xC0010117)
    uint32_t vm_hsave_pa;
    // EFER as currently readable (MSR 0xC0000080)
    uint64_t efer;

    // decoded convenience flags
    uint8_t  is_amd;
    uint8_t  has_svm;               // CPUID extended bit
    uint8_t  svm_disabled;          // VM_CR.SVMDIS
    uint8_t  lock;                  // VM_CR.LOCK
    uint8_t  has_npt;               // CPUID 0x8000000A:EDX bit 0
    uint8_t  has_nrips;             // bit 3
    uint8_t  has_lbr_virt;          // bit 1
    uint8_t  svme_currently_set;    // EFER.SVME right now
    uint32_t nasid;                 // number of ASIDs supported
} svm_caps;

// Probe the CPU.  `out` receives the capability facts.
// Returns SVM_OK, or one of the svm_probe_result errors.
// Reads only CPUID/MSRs; never executes any VM instruction.
int svm_probe(svm_caps* out);

// Convenience: nonzero when svm_probe() would return SVM_OK.
int svm_is_supported(void);

// Human-readable name for a result code (for serial logging).
const char* svm_probe_str(int result);

// ---------------------------------------------------------------------
//  Constants (AMD APM Vol.2 "Secure Virtual Machine" appendices)
// ---------------------------------------------------------------------
#define SVM_CPUID_VENDOR_LEAF      0x00000000u
#define SVM_CPUID_EXT_LEAF         0x80000000u
#define SVM_CPUID_FEAT_LEAF        0x80000001u
#define SVM_CPUID_SVM_LEAF         0x8000000Au

#define SVM_CPUID_EXT_ECX_SVM      (1u << 2)    // CPUID 8000_0001:ECX bit 2
#define SVM_CPUID_SVM_EDX_NPT      (1u << 0)    // CPUID 8000_000A:EDX bit 0
#define SVM_CPUID_SVM_EDX_LBR      (1u << 1)    // LBR virtualization
#define SVM_CPUID_SVM_EDX_NRIPS    (1u << 3)    // next-RIP storage
#define SVM_CPUID_SVM_EDX_FLUSHBY  (1u << 2)    // TLB flush by ASID

#define MSR_VM_CR                  0xC0010114u
#define MSR_VM_HSAVE_PA            0xC0010117u
#define MSR_EFER                   0xC0000080u

#define VM_CR_SVMDIS               (1u << 4)    // bit 4: SVM disabled
#define VM_CR_LOCK                 (1u << 3)    // bit 3: lock (read-only)

#define EFER_SVME                  (1u << 12)   // bit 12: SVM enable
#define EFER_LME                   (1u << 8)    // long mode enable
#define EFER_LMA                   (1u << 10)   // long mode active
#define EFER_NXE                   (1u << 11)   // no-execute enable

#endif // NEXOS_SVM_PROBE_H
