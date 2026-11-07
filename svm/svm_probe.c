// =====================================================================
//  svm_probe.c  -  safe AMD SVM capability detection (no VM instructions)
// ---------------------------------------------------------------------
//  See svm_probe.h for the design rule.  Summary: CPUID + RDMSR only.
//
//  IMPORTANT SAFETY NOTE ON CPUID
//    CPUID is a serialising but FAILURE-SAFE instruction: asking an
//    unsupported leaf returns zeros/garbage but never faults.  It is the
//    correct way to discover support BEFORE touching anything privileged.
//
//  IMPORTANT SAFETY NOTE ON RDMSR
//    RDMSR of a non-existent MSR raises #GP.  VM_CR and VM_HSAVE_PA only
//    exist when the CPU advertises SVM, so we read them ONLY after CPUID
//    has confirmed SVM.  EFER exists on every x86-64 capable CPU, but on a
//    plain 32-bit CPU it may not, so we read it only when the extended
//    CPUID leaf says long mode is available.  This ordering is what makes
//    the function safe to call on arbitrary hardware.
// =====================================================================

#include "svm_probe.h"

// ---- low-level reads (reads only; nothing here writes CPU state) ----
static void cpu_read_cpuid(uint32_t leaf, uint32_t sub,
                           uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(sub));
}

static uint64_t cpu_read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

// ---------------------------------------------------------------------
//  Vendor check
// ---------------------------------------------------------------------
//  CPUID 0x00000000 returns "GenuineIntel"/"AuthenticAMD" in EBX,EDX,ECX.
//  Only AMD implements SVM (Intel's equivalent is VT-x/VMX, a different
//  instruction set), so vendor matters before anything else.
static int cpu_is_amd(uint32_t* max_std_leaf){
    uint32_t a, b, c, d;
    cpu_read_cpuid(SVM_CPUID_VENDOR_LEAF, 0, &a, &b, &c, &d);

    // "Auth" "enti" "cAMD" -> EBX=0x68747541 EDX=0x69746E65 ECX=0x444D4163
    if (b != 0x68747541u || d != 0x69746E65u || c != 0x444D4163u)
        return 0;

    if (max_std_leaf) *max_std_leaf = a;
    return 1;
}

// ---------------------------------------------------------------------
int svm_probe(svm_caps* out)
{
    if (!out) return SVM_ERR_FEATURE_UNKNOWN;

    // Zero the whole struct first so a caller that logs it on failure does
    // not print uninitialised stack garbage.
    {
        char* p = (char*)out;
        for (unsigned i = 0; i < sizeof(*out); i++) p[i] = 0;
    }

    // ---- 1. vendor ----
    uint32_t max_std = 0;
    if (!cpu_is_amd(&max_std)) return SVM_ERR_NOT_AMD;
    out->is_amd = 1;

    // ---- 2. does the extended CPUID range reach 0x80000001? ----
    uint32_t a, b, c, d;
    cpu_read_cpuid(SVM_CPUID_EXT_LEAF, 0, &a, &b, &c, &d);
    if (a < SVM_CPUID_FEAT_LEAF) return SVM_ERR_NO_EXT_CPUID;

    // ---- 3. SVM feature bit (CPUID 8000_0001:ECX bit 2) ----
    cpu_read_cpuid(SVM_CPUID_FEAT_LEAF, 0, &a, &b, &c, &d);
    out->cpuid_ext_features_ecx = c;
    if (!(c & SVM_CPUID_EXT_ECX_SVM)) return SVM_ERR_NO_SVM_CPUID;
    out->has_svm = 1;

    // From here on the CPU advertises SVM, so the SVM MSRs exist.

    // ---- 4. SVM feature leaf 0x8000000A (NASID + feature flags) ----
    cpu_read_cpuid(SVM_CPUID_SVM_LEAF, 0, &a, &b, &c, &d);
    out->svm_features_eax = a;
    out->svm_features_edx = d;
    out->nasid            = a;                     // bits 31:0
    out->has_npt          = (d & SVM_CPUID_SVM_EDX_NPT) ? 1 : 0;
    out->has_lbr_virt     = (d & SVM_CPUID_SVM_EDX_LBR) ? 1 : 0;
    out->has_nrips        = (d & SVM_CPUID_SVM_EDX_NRIPS) ? 1 : 0;

    // ---- 5. MSR_VM_CR: is SVM disabled or locked? ----
    uint64_t vm_cr = cpu_read_msr(MSR_VM_CR);
    out->vm_cr         = (uint32_t)vm_cr;
    out->svm_disabled  = (vm_cr & VM_CR_SVMDIS) ? 1 : 0;
    out->lock          = (vm_cr & VM_CR_LOCK)    ? 1 : 0;

    if (out->svm_disabled) return SVM_ERR_VM_CR_LOCK;

    // ---- 6. MSR_VM_HSAVE_PA: host save area (must be settable) ----
    out->vm_hsave_pa = (uint32_t)cpu_read_msr(MSR_VM_HSAVE_PA);

    // ---- 7. EFER: is SVME currently set / settable? ----
    //  Read only when the long-mode feature bit is advertised, so this
    //  cannot #GP on a CPU without EFER.  (EFER exists iff the CPU
    //  supports AMD64; CPUID 8000_0001:EDX bit 29 reports that.)
    if (d & (1u << 29)) {                 // EDX is reused from the SVM leaf
        // Re-read the feature leaf: `d` above came from 0x8000000A, not
        // 0x80000001.  Read both explicitly to avoid relying on which
        // leaf happened to be in EDX last.
        uint32_t fa, fb, fc, fd2;
        cpu_read_cpuid(SVM_CPUID_FEAT_LEAF, 0, &fa, &fb, &fc, &fd2);
        if (fd2 & (1u << 29)) {
            out->efer = cpu_read_msr(MSR_EFER);
            out->svme_currently_set = (out->efer & EFER_SVME) ? 1 : 0;
        }
    }

    // ---- 8. NPT is required for the Linux-guest plan ----
    //  Reported through has_npt; the caller decides whether that is fatal.
    return SVM_OK;
}

int svm_is_supported(void)
{
    svm_caps caps;
    return svm_probe(&caps) == SVM_OK ? 1 : 0;
}

const char* svm_probe_str(int result)
{
    switch (result) {
        case SVM_OK:                    return "ok";
        case SVM_ERR_NOT_AMD:           return "not an AMD CPU (SVM is AMD-only)";
        case SVM_ERR_NO_SVM_CPUID:      return "CPUID 8000_0001:ECX.SVM is clear";
        case SVM_ERR_NO_EXT_CPUID:      return "extended CPUID leaf 8000_0001 unavailable";
        case SVM_ERR_VM_CR_LOCK:        return "VM_CR.SVMDIS set (SVM disabled by firmware)";
        case SVM_ERR_EFER_NOT_WRITABLE: return "EFER not usable for SVME";
        case SVM_ERR_FEATURE_UNKNOWN:   return "unknown/unexpected CPU state";
        default:                        return "unrecognised result";
    }
}
