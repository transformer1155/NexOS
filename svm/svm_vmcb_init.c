// =====================================================================
//  svm_vmcb_init.c  -  VMCB construction (pure memory writes)
// ---------------------------------------------------------------------
//  See the header for the safety contract.  Nothing here runs a VM
//  instruction; every line is a store into an ordinary struct.
// =====================================================================
#include "svm_vmcb_init.h"
#include "svm_bitmaps.h"

// ---------------------------------------------------------------------
//  Local helpers (freestanding: no libc)
// ---------------------------------------------------------------------
static void v_zero(void* p, uint64_t n)
{
    uint8_t* d = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

// ---------------------------------------------------------------------
//  Fill both bitmaps so EVERY port and EVERY MSR causes a #VMEXIT.
//
//  Why start maximally restrictive: a Linux guest touches hardware we are
//  not emulating yet.  If we allowed those accesses through, they would
//  hit real host devices and the host would corrupt itself.  Intercepting
//  everything means the guest can only make progress through paths we
//  explicitly implement -- and every unhandled access is a clean exit we
//  can log, not silent damage.
// ---------------------------------------------------------------------
void iopm_fill_all(struct svm_iopm* iopm)
{
    if (!iopm) return;
    // 1 bit per port; all-ones = intercept all 65536 ports.
    for (uint32_t i = 0; i < IOPM_BYTES; i++) iopm->bytes[i] = 0xFF;
}

void msrpm_fill_all(struct svm_msrpm* msrpm)
{
    if (!msrpm) return;
    // 2 bits per MSR in 12 zones.  Writing 0xFF sets ALL intercept bits:
    // each MSR gets "intercept read, intercept write".
    for (uint32_t i = 0; i < MSRPM_BYTES; i++) msrpm->bytes[i] = 0xFF;
}

// ---------------------------------------------------------------------
//  Segment descriptor helper
// ---------------------------------------------------------------------
//  The attribute word layout is defined in svm_vmcb.h (SEG_ATTRIB_*).
//  The earlier helper built this by shifting into bit 20, which silently
//  overflowed a uint16_t and dropped the L (long-mode) bit entirely --
//  caught by the verification test (CS attrib came out 0x2880 instead of
//  the required 0x0A93 granting L=1).
//
//  Use the constants directly so the encoding is stated once, in the
//  header, where a static_assert pins it to the architectural value.
static void set_seg(struct svm_vmcb_seg* s, uint16_t sel, uint16_t attrib,
                    uint32_t limit, uint64_t base)
{
    s->selector = sel;
    s->attrib   = attrib;
    s->limit    = limit;
    s->base     = base;
}

// ---------------------------------------------------------------------
int vmcb_init_linux64(struct svm_vmcb* v, const vmcbinit_params* p)
{
    if (!v || !p) return VMCBINIT_ERR_NULL;

    if ((p->iopm_pa  & 0xFFFull) != 0) return VMCBINIT_ERR_ALIGN;
    if ((p->msrpm_pa & 0xFFFull) != 0) return VMCBINIT_ERR_ALIGN;
    if ((p->ncr3_pa  & 0xFFFull) != 0) return VMCBINIT_ERR_ALIGN;

    if (p->guest_rip == 0)          return VMCBINIT_ERR_BAD_ARG;
    if (p->guest_ram_bytes == 0)    return VMCBINIT_ERR_BAD_ARG;

    // ---- start from a clean slate ----
    v_zero(v, sizeof(*v));

    // =================================================================
    //  CONTROL AREA
    // =================================================================
    struct svm_vmcb_control* c = &v->control;

    // ---- intercept words ----
    //  word0 (CR): intercept every control-register READ and WRITE we do
    //  not emulate.  A Linux guest pokes CR0/CR3/CR4 during boot; each
    //  must come back to us.
    c->intercepts[SVM_IW_CR] =
          SVM_IC_CR0_READ  | SVM_IC_CR3_READ  | SVM_IC_CR4_READ  | SVM_IC_CR8_READ
        | SVM_IC_CR0_WRITE | SVM_IC_CR3_WRITE | SVM_IC_CR4_WRITE | SVM_IC_CR8_WRITE;

    //  word1 (DR): leave debug registers alone for now (a Linux guest
    //  rarely touches them before boot; intercepting adds noise).
    c->intercepts[SVM_IW_DR] = 0;

    //  word2 (exceptions): do NOT intercept ordinary #PF/#GP/#UD -- the
    //  guest must handle those itself or it cannot boot.  We only watch
    //  double fault, because that means "shut down" and should not take
    //  the host with it.
    c->intercepts[SVM_IW_EXCEPTION] = SVM_EXC_DOUBLE_FAULT;

    //  word3: the workhorses.
    c->intercepts[SVM_IW_WORD3] =
          SVM_I3_INTR          // external interrupt
        | SVM_I3_NMI
        | SVM_I3_SMI
        | SVM_I3_INIT
        | SVM_I3_RDTSC         // guest TSC must be virtualisable
        | SVM_I3_PUSHF         // hides host RFLAGS
        | SVM_I3_POPF
        | SVM_I3_CPUID         // guest CPUID must not see host features
        | SVM_I3_IRET
        | SVM_I3_SWINT
        | SVM_I3_HLT           // guest idle
        | SVM_I3_IOIO          // <-- every port access (see IOPM)
        | SVM_I3_MSR           // every MSR access (see MSRPM)
        | SVM_I3_SHUTDOWN;     // triple fault -> clean exit, not host death

    //  word4: virtualization instructions and EFER.
    c->intercepts[SVM_IW_WORD4] =
          SVM_I4_VMRUN         // nested hypervisor attempt
        | SVM_I4_VMMCALL
        | SVM_I4_VMLOAD
        | SVM_I4_VMSAVE
        | SVM_I4_STGI
        | SVM_I4_CLGI
        | SVM_I4_SKINIT
        | SVM_I4_EFER;         // guest EFER writes must be filtered

    //  word5: leave to zero (no intercepts defined here for our plan).
    c->intercepts[SVM_IW_WORD5] = 0;

    // ---- bitmaps (PHYSICAL addresses) ----
    c->iopm_base_pa  = p->iopm_pa;
    c->msrpm_base_pa = p->msrpm_pa;

    // ---- nested paging ----
    //  NP_ENABLE turns on NPT translation; nested_cr3 is the nPML4E root.
    c->nested_ctl = SVM_NESTED_CTL_NP_ENABLE;
    c->nested_cr3 = p->ncr3_pa;

    // ---- misc ----
    c->asid    = 1;                 // one guest for now; 0 is reserved
    c->tlb_ctl = 0;                 // no flush request at first run
    c->clean   = 0;                 // must be zero on entry

    // =================================================================
    //  SAVE AREA  (the guest's initial register state)
    // =================================================================
    struct svm_vmcb_save* s = &v->save;

    // ---- flat 64-bit segments ----
    //  All bases 0, limits ignored in long mode.  Selectors only need to
    //  be self-consistent; the CPU does not use them for address maths
    //  when base == 0 and L == 1.
    set_seg(&s->cs, 0x0008, SVM_ATTRIB_CODE64, 0xFFFFFFFFu, 0);
    set_seg(&s->ds, 0x0010, SVM_ATTRIB_DATA64, 0xFFFFFFFFu, 0);
    set_seg(&s->es, 0x0010, SVM_ATTRIB_DATA64, 0xFFFFFFFFu, 0);
    set_seg(&s->ss, 0x0010, SVM_ATTRIB_DATA64, 0xFFFFFFFFu, 0);
    set_seg(&s->fs, 0x0010, SVM_ATTRIB_DATA64, 0xFFFFFFFFu, 0);
    set_seg(&s->gs, 0x0010, SVM_ATTRIB_DATA64, 0xFFFFFFFFu, 0);

    // GDTR/IDTR: the guest will load its own very early; point at a
    // zero-length descriptor so the first LGDTR/LIDTR is well-defined.
    set_seg(&s->gdtr, 0, 0, 0, 0);
    set_seg(&s->idtr, 0, 0, 0, 0);
    set_seg(&s->ldtr, 0, 0, 0, 0);

    // TR must look like a valid 64-bit TSS descriptor: type 1001b = 0xB,
    // P=1, S=0.
    set_seg(&s->tr, 0, SVM_ATTRIB_TSS64, 0x67u, 0);

    s->cpl  = 0;                    // guest starts at ring 0 (kernel)
    s->vmpl = 0;

    // ---- EFER ----
    //  LME + LMA + NXE give 64-bit long mode with no-execute.
    //  SVME is deliberately CLEAR here: that flag belongs to the HOST's
    //  EFER (set before VMRUN).  A guest SVME=1 is nonsense and would
    //  make the CPU reject the VMCB.
    s->efer = EFER_LME | EFER_LMA | EFER_NXE;

    // ---- CR0 / CR3 / CR4 ----
    //  CR0: PE (bit0) + ET (bit4) + NE (bit5) + PG (bit31).
    s->cr0 = (1ull << 0) | (1ull << 4) | (1ull << 5) | (1ull << 31);
    s->cr3 = (uint64_t)(uint32_t)p->guest_cr3;
    //  CR4: PAE (bit5) is mandatory for long mode.
    s->cr4 = (1ull << 5);

    // ---- entry point and stack ----
    s->rip    = p->guest_rip;
    s->rsp    = p->guest_rsp;
    //  RFLAGS: reserved bit 1 set; all interrupts off inside the guest
    //  until it installs its own IDT.
    s->rflags = 0x0000000000000002ull;

    s->rax = 0;                    // guest starts with no FYI in RAX

    //  PAT: the standard 64-bit reset value.  The CPU requires a legal
    //  PAT pattern; anything else is rejected at VMRUN.
    s->g_pat = 0x0007040600070406ull;

    return VMCBINIT_OK;
}

const char* vmcbinit_str(int rc)
{
    switch (rc) {
        case VMCBINIT_OK:          return "ok";
        case VMCBINIT_ERR_NULL:    return "null vmcb or params pointer";
        case VMCBINIT_ERR_ALIGN:   return "iopm/msrpm/ncr3 not page-aligned";
        case VMCBINIT_ERR_BAD_ARG: return "bad guest_rip or guest_ram_bytes";
        default:                   return "unrecognised result";
    }
}