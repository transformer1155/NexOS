/* ============================================================================
 * svm_driver.c - NexOS hot-loadable AMD SVM (Secure Virtual Machine) driver
 * ----------------------------------------------------------------------------
 * A real AMD-V implementation, ported field-for-field from the proven code in
 * plugins/vmm.cpp (which is the authoritative version in this tree).  Nothing
 * here is re-invented: every MSR number, VMCB offset, segment attribute,
 * intercept word and the VMRUN instruction encoding is copied from there.
 *
 * Source provenance (plugins/vmm.cpp):
 *   L66-74    rdmsr / wrmsr inline asm                    -> svm_rdmsr/svm_wrmsr
 *   L84-88    AMD SVM MSRs + bits (VM_CR/SVM_DISABLE/HSAVE_PA/EFER.SVME)
 *   L90-151   full VMCB control-area + save-area layout
 *   L143-146  SVM_EXITCODE_IOIO / HLT / IOIO fields
 *   L148-151  SVM_SEG_ATTRIB_CODE32 / DATA32 / TSS
 *   L189-205  AMD detection (CPUID.80000001:ECX bit2 + VM_CR.SVM_DISABLE)
 *   L462-472  vm_put16 / vm_put32 / vm_get32
 *   L475-481  vm_set_seg
 *   L485-539  vm_build_vmcb (32-bit flat, no paging)
 *   L591-619  vm_svm_vmrun (CLC + ".byte 0x0F,0x01,0xD8", exitcode==0 => #VMFAIL)
 *   L621-643  vmm_svm_enter prologue (EFER.SVME, VM_HSAVE_PA, read-back check)
 *   L644-675  #VMEXIT dispatch (IOIO -> vUART tx ring, HLT -> guest done)
 *   L678-684  vmm_run_once entry
 *
 * Deliberate adaptations to a hot-loadable module (documented, not invented):
 *   - vmm.cpp pokes the guest GDT at absolute physical 0x6000 and points the
 *     guest RIP at GUEST_CODE_PHYS (guest_blob.h), both of which live in
 *     kernel-reserved low memory.  A driver must never write 1:1 kernel low
 *     memory, so we allocate ONE extra guest page that holds both the tiny
 *     guest stub and the GDT, and point GDTR/RIP/RSP into it instead.
 *   - vmm.cpp rebuilds the whole VMCB before each of its single-shot runs, so
 *     it only ever updates gg->rip.  Item 9 requires a *continuing* loop, so
 *     svm_handle_exit() writes the resumed RIP straight back into the live
 *     VMCB save area; VMCB_CLEAN is kept at 0 (all state dirty) which is the
 *     conservative, correct choice after modifying save state.
 *   - Page-aligned physical frames come from the loader's pmm_* services
 *     (kmalloc is not guaranteed 4 KiB aligned, and the APM requires physical
 *     page alignment for VMCB and VM_HSAVE_PA).
 *
 * Build:
 *   gcc -m32 -ffreestanding -nostdlib -fno-pic -fno-pie -fno-stack-protector \
 *       -fno-asynchronous-unwind-tables -Os -Wall -Wextra -I. \
 *       -c svm_driver.c -o /tmp/svm.o
 * ==========================================================================*/
#include "driver_contract.h"

/* ---------------------------------------------------------------------------
 * AMD SVM MSR / bits                              (plugins/vmm.cpp L84-88)
 * -------------------------------------------------------------------------*/
#define MSR_VM_CR               0xC0010114u
#define VM_CR_SVM_DISABLE       (1u << 4)
#define MSR_VM_HSAVE_PA         0xC0010117u
#define MSR_EFER                0xC0000080u
#define EFER_SVME               (1u << 12)

/* ---------------------------------------------------------------------------
 * VMCB layout (authoritative: Linux arch/x86/include/asm/svm.h)
 * (plugins/vmm.cpp L97-141, verbatim)
 * -------------------------------------------------------------------------*/
#define VMCB_CR_INTERCEPT       0x000
#define VMCB_DR_INTERCEPT       0x004
#define VMCB_EXC_INTERCEPT      0x008
#define VMCB_WORD3_INTERCEPT    0x00C
#define VMCB_WORD4_INTERCEPT    0x010
#define VMCB_IOPM_BASE_PA       0x030
#define VMCB_MSRPM_BASE_PA      0x038
#define VMCB_ASID               0x048
#define VMCB_TLB_CTL            0x04C
#define VMCB_INT_CTL            0x050
#define VMCB_EXITCODE           0x058   /* u64 */
#define VMCB_EXITINFO1          0x060
#define VMCB_EXITINFO2          0x068
#define VMCB_NEXT_RIP           0x0A0
#define VMCB_EVENT_INJ          0x0A8
#define VMCB_CLEAN              0x0F8
/* save area (base 0x400; struct vmcb_seg = sel2 attr2 limit4 base8 = 16B) */
#define VMCB_SAVE_ES            0x400
#define VMCB_SAVE_CS            0x410
#define VMCB_SAVE_SS            0x420
#define VMCB_SAVE_DS            0x430
#define VMCB_SAVE_FS            0x440
#define VMCB_SAVE_GS            0x450
#define VMCB_SAVE_GDTR          0x460   /* selector=0, attrib=0: limit @+4, base @+8 */
#define VMCB_SAVE_LDTR          0x470
#define VMCB_SAVE_IDTR          0x480
#define VMCB_SAVE_TR            0x490
#define VMCB_SAVE_VMPL          0x4CA   /* 0x400 + 0xCA */
#define VMCB_SAVE_CPL           0x4CB   /* 0x400 + 0xCB */
#define VMCB_SAVE_EFER          0x4D0   /* 0x400 + 0xD0 (after reserved_0xcc[4]) */
#define VMCB_SAVE_CR4           0x548
#define VMCB_SAVE_CR3           0x550
#define VMCB_SAVE_CR0           0x558
#define VMCB_SAVE_DR7           0x560
#define VMCB_SAVE_DR6           0x568
#define VMCB_SAVE_RFLAGS        0x570
#define VMCB_SAVE_RIP           0x578
#define VMCB_SAVE_RSP           0x5D8
#define VMCB_SAVE_RAX           0x5F8   /* 0x400 + 0x1F8 (after s_cet/ssp/isst) */
#define VMCB_SAVE_CR2           0x618
#define VMCB_SAVE_G_PAT         0x638

/* exit codes we dispatch on                       (plugins/vmm.cpp L143-146) */
#define SVM_EXITCODE_IOIO       0x7B
#define SVM_EXITCODE_HLT        0x78
#define SVM_IOIO_PORT_SHIFT     16
#define SVM_IOIO_TYPE_IN        (1u << 3)  /* EXITINFO1 bit3: 1 = IN  */

/* flat-segment attribute encodings                 (plugins/vmm.cpp L148-151) */
#define SVM_SEG_ATTRIB_CODE32   0x0C9B      /* P|D/B|G|code+read  */
#define SVM_SEG_ATTRIB_DATA32   0x0C93      /* P|D/B|G|data r/w   */
#define SVM_SEG_ATTRIB_TSS      0x008B      /* P|busy 32-bit TSS  */

/* ---------------------------------------------------------------------------
 * Driver-local constants
 * -------------------------------------------------------------------------*/
#define SVM_PAGE_SIZE           4096u
#define SVM_LIVE_MAGIC          0x53564D31u   /* "SVM1": self-guard          */

/* IOPM covers all 65536 ports at 2 bits each = 16 KiB; MSRPM = 8 KiB.
 * (plugins/vmm.cpp L368-374 uses exactly these sizes.)                      */
#define SVM_IOPM_PAGES          4u
#define SVM_MSRPM_PAGES         2u
#define SVM_IOPM_BYTES          (SVM_IOPM_PAGES  * SVM_PAGE_SIZE)
#define SVM_MSRPM_BYTES         (SVM_MSRPM_PAGES * SVM_PAGE_SIZE)

/* Guest page layout: stub code at +0, GDT at +0x100, stack top at +0xF00. */
#define SVM_GUEST_STUB_OFF      0x000u
#define SVM_GUEST_GDT_OFF       0x100u
#define SVM_GUEST_GDT_LIMIT     0x17u    /* 3 descriptors * 8 bytes - 1 */
#define SVM_GUEST_STACK_TOP     0xF00u

#define SVM_VMEXIT_BUDGET       64u      /* max VMRUNs per svm_run_loop */
#define SVM_UART_RING           64u

/* ---------------------------------------------------------------------------
 * Driver state.  A single static instance (one SVM unit per machine).
 * The loader's ctx is a DriverRec* and is NEVER reinterpreted here.
 * init() resets this struct in place so a hot reload starts clean.
 * -------------------------------------------------------------------------*/
typedef struct SvmDriver {
    const DriverKernelAPI* api;

    uint32_t magic;           /* SVM_LIVE_MAGIC once init() fully succeeded */

    /* physical frames handed to the CPU (flat NexOS: PA is dereferenceable) */
    uint32_t vmcb_pa;         /* 4 KiB: VMCB (must be page aligned)          */
    uint32_t hsave_pa;        /* 4 KiB: VM_HSAVE_PA host state area          */
    uint32_t iopm_pa;         /* 16 KiB contiguous IOPM bitmap               */
    uint32_t msrpm_pa;        /* 8 KiB contiguous MSRPM bitmap               */
    uint32_t guest_pa;        /* 4 KiB: guest stub + GDT + guest stack       */

    uint32_t rip;             /* guest RIP mirror (updated on every exit)    */
    uint32_t exits;           /* #VMEXITs dispatched in the last run loop    */
    uint32_t halted;          /* guest executed HLT                          */
    uint32_t uart_head;       /* vUART tx ring head (guest OUT bytes)        */
    uint32_t uart_tail;
    uint8_t  uart_tx[SVM_UART_RING];

    int      device_registered;
    int      svme_enabled;    /* EFER.SVME was set by us and must be cleared */
} SvmDriver;

static SvmDriver g_svm;

/* ---------------------------------------------------------------------------
 * Logging (freestanding: no libc, no sprintf).  All output carries "[SVM]".
 * -------------------------------------------------------------------------*/
static void svm_puts(const char* s)
{
    if (s && g_svm.api && g_svm.api->printk) g_svm.api->printk(s);
}

static void svm_puthex(uint32_t v)
{
    char b[11];
    int  i;
    b[0] = '0';
    b[1] = 'x';
    for (i = 0; i < 8; ++i) {
        uint32_t n = (v >> ((7 - i) * 4)) & 0xFu;
        b[2 + i] = (char)(n < 10u ? ('0' + n) : ('A' + (n - 10u)));
    }
    b[10] = 0;
    svm_puts(b);
}

static void svm_putdec(uint32_t v)
{
    char b[12];
    int  n = 0;
    if (v == 0) {
        b[n++] = '0';
    } else {
        char t[10];
        int  tn = 0;
        while (v) { t[tn++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (tn) b[n++] = t[--tn];
    }
    b[n] = 0;
    svm_puts(b);
}

/* ---------------------------------------------------------------------------
 * Low-level CPU helpers                     (plugins/vmm.cpp L66-74, L54-65)
 * -------------------------------------------------------------------------*/
static uint64_t svm_rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void svm_wrmsr(uint32_t msr, uint64_t val)
{
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* CPUID goes through the kernel service (no ring-3 privilege issue, and the
 * loader owns the canonical implementation). */
static void svm_cpuid(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b,
                      uint32_t* c, uint32_t* d)
{
    if (a) *a = 0;
    if (b) *b = 0;
    if (c) *c = 0;
    if (d) *d = 0;
    if (g_svm.api && g_svm.api->cpuid)
        g_svm.api->cpuid(leaf, sub, a, b, c, d);
}

/* ---------------------------------------------------------------------------
 * VMCB field accessors                                  (vmm.cpp L462-481)
 * -------------------------------------------------------------------------*/
static void vm_put16(uint8_t* vmcb, uint32_t off, uint16_t v)
{
    vmcb[off]     = (uint8_t)v;
    vmcb[off + 1] = (uint8_t)(v >> 8);
}

static void vm_put32(uint8_t* vmcb, uint32_t off, uint32_t v)
{
    vmcb[off]     = (uint8_t)v;
    vmcb[off + 1] = (uint8_t)(v >> 8);
    vmcb[off + 2] = (uint8_t)(v >> 16);
    vmcb[off + 3] = (uint8_t)(v >> 24);
}

static uint32_t vm_get32(const uint8_t* vmcb, uint32_t off)
{
    return (uint32_t)vmcb[off] |
           ((uint32_t)vmcb[off + 1] << 8) |
           ((uint32_t)vmcb[off + 2] << 16) |
           ((uint32_t)vmcb[off + 3] << 24);
}

/* Fill one segment slot: u16 selector, u16 attrib, u32 limit, base(=0). */
static void vm_set_seg(uint8_t* vmcb, uint32_t slot, uint16_t sel,
                       uint16_t attrib, uint32_t limit)
{
    vm_put16(vmcb, slot,     sel);
    vm_put16(vmcb, slot + 2, attrib);
    vm_put32(vmcb, slot + 4, limit);
    /* base (8 bytes at slot+8) stays 0: flat segments */
}

static void svm_memzero(uint8_t* p, uint32_t len)
{
    uint32_t i;
    if (!p) return;
    for (i = 0; i < len; ++i) p[i] = 0;
}

/* ---------------------------------------------------------------------------
 * Detection: AMD vendor + CPUID.80000001:ECX bit2 + VM_CR.SVM_DISABLE clear.
 * (plugins/vmm.cpp L189-205)
 *
 * CRITICAL ordering: the MSR read is privileged, so it may only run after
 * CPUID has positively identified AMD-with-SVM.  On any other CPU we bail out
 * with a negative code and never touch an MSR (no #GP, no crash).
 * -------------------------------------------------------------------------*/
static int svm_detect(void)
{
    uint32_t a, b, c, d;
    int is_amd;

    svm_cpuid(0u, 0u, &a, &b, &c, &d);
    is_amd = (b == 0x68747541u && c == 0x444D4163u && d == 0x69746E65u);
    svm_puts("[SVM] detect: vendor-id=");
    svm_puthex(b); svm_puts("/"); svm_puthex(c); svm_puts("/"); svm_puthex(d);
    svm_puts(" amd="); svm_putdec((uint32_t)is_amd); svm_puts("\n");

    if (!is_amd) {
        svm_puts("[SVM] not an AuthenticAMD cpu, refusing\n");
        return DRIVER_ERR_INIT_FAILED;
    }

    svm_cpuid(0x80000000u, 0u, &a, &b, &c, &d);
    svm_puts("[SVM] max-extended-leaf="); svm_puthex(a); svm_puts("\n");
    if (a < 0x80000001u) {
        svm_puts("[SVM] no CPUID.80000001, refusing\n");
        return DRIVER_ERR_INIT_FAILED;
    }

    svm_cpuid(0x80000001u, 0u, &a, &b, &c, &d);
    svm_puts("[SVM] CPUID.80000001 ECX="); svm_puthex(c);
    svm_puts(" svm-bit="); svm_putdec((c >> 2) & 1u); svm_puts("\n");
    if (!(c & (1u << 2))) {
        svm_puts("[SVM] CPUID SVM bit clear (SVM not supported), refusing\n");
        return DRIVER_ERR_INIT_FAILED;
    }

    {
        uint64_t vmcr = svm_rdmsr(MSR_VM_CR);
        svm_puts("[SVM] VM_CR="); svm_puthex((uint32_t)vmcr); svm_puts("\n");
        if (vmcr & VM_CR_SVM_DISABLE) {
            svm_puts("[SVM] VM_CR.SVM_DISABLE set, refusing\n");
            return DRIVER_ERR_INIT_FAILED;
        }
    }
    return DRIVER_OK;
}

/* ---------------------------------------------------------------------------
 * Guest stub: "mov al,'S'; out 0xE9,al; mov al,'V'; out 0xE9,al; hlt"
 * Encoding: B0 ib = MOV AL,imm8 ; E6 ib = OUT imm8,AL ; F4 = HLT.
 * Port 0xE9 is the classic NexOS/QEMU debug port; the OUT traps via the
 * IOIO_PROT intercept and the HLT traps via INTERCEPT_HLT, so one run
 * exercises both dispatch paths and then terminates the loop.
 * -------------------------------------------------------------------------*/
static void svm_build_guest_stub(SvmDriver* s)
{
    uint8_t*  code = (uint8_t*)(uintptr_t)s->guest_pa;
    uint32_t* gd   = (uint32_t*)(uintptr_t)(s->guest_pa + SVM_GUEST_GDT_OFF);
    uint32_t  i;

    svm_memzero(code, SVM_PAGE_SIZE);

    i = SVM_GUEST_STUB_OFF;
    code[i++] = 0xB0u;              /* mov al, 'S'   */
    code[i++] = (uint8_t)'S';
    code[i++] = 0xE6u;              /* out 0xE9, al  */
    code[i++] = 0xE9u;
    code[i++] = 0xB0u;              /* mov al, 'V'   */
    code[i++] = (uint8_t)'V';
    code[i++] = 0xE6u;              /* out 0xE9, al  */
    code[i++] = 0xE9u;
    code[i++] = 0xF4u;              /* hlt           */

    /* GDT image (plugins/vmm.cpp L520-523): null, flat 32 code, flat 32 data */
    gd[0] = 0;              gd[1] = 0;
    gd[2] = 0xFFFF0000u;    gd[3] = 0x00CF9A00u;
    gd[4] = 0xFFFF0000u;    gd[5] = 0x00CF9300u;
}

/* ---------------------------------------------------------------------------
 * Build the whole VMCB for a 32-bit flat, no-paging guest.
 * (plugins/vmm.cpp L485-539, with the GDTR base / RIP moved into our own
 *  guest page instead of kernel low memory 0x6000 / GUEST_CODE_PHYS.)
 * -------------------------------------------------------------------------*/
static void svm_build_vmcb(SvmDriver* s)
{
    uint8_t* v = (uint8_t*)(uintptr_t)s->vmcb_pa;   /* flat: PA dereferenceable */

    svm_memzero(v, SVM_PAGE_SIZE);

    /* control area intercepts (global bit numbers):
     *   word3 (0x0C): bit23 = HLT, bit31 = IOIO_PROT (all port I/O)
     *   word4 (0x10): bit0 VMRUN (MANDATORY: VMRUN fails with #VMFAILVALID
     *                 unless it intercepts itself), bits1-4 VMMCALL/VMLOAD/
     *                 VMSAVE/STGI/CLGI (required per APM nested-SVM rules) */
    vm_put32(v, VMCB_WORD3_INTERCEPT, (1u << 23) | (1u << 31));
    vm_put32(v, VMCB_WORD4_INTERCEPT, 0x0000001Fu);
    vm_put32(v, VMCB_IOPM_BASE_PA,  s->iopm_pa);
    vm_put32(v, VMCB_MSRPM_BASE_PA, s->msrpm_pa);
    vm_put32(v, VMCB_ASID, 1);
    vm_put32(v, VMCB_TLB_CTL, 0);
    vm_put32(v, VMCB_CLEAN, 0);     /* 0 = all state dirty: always reload */

    /* save area: flat 32-bit segments */
    vm_set_seg(v, VMCB_SAVE_ES, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_CS, 0x08, SVM_SEG_ATTRIB_CODE32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_SS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_DS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_FS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_GS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);

    /* GDTR/IDTR use the same 16-byte vmcb_seg slot: selector=0, attrib=0,
     * limit u32 @+4, base u64 @+8. */
    vm_put32(v, VMCB_SAVE_GDTR + 4, SVM_GUEST_GDT_LIMIT);
    vm_put32(v, VMCB_SAVE_GDTR + 8, s->guest_pa + SVM_GUEST_GDT_OFF);
    vm_put32(v, VMCB_SAVE_IDTR + 4, 0xFFFFu);
    /* IDTR base stays 0 (no interrupts delivered to this guest) */

    /* TR: present busy 32-bit TSS */
    vm_set_seg(v, VMCB_SAVE_TR, 0x00, SVM_SEG_ATTRIB_TSS, 0xFFFFu);

    /* control registers + entry state */
    vm_put32(v, VMCB_SAVE_CR4, 0);
    vm_put32(v, VMCB_SAVE_CR3, 0);
    vm_put32(v, VMCB_SAVE_CR0, 0x11u);        /* PE | ET, no paging */
    vm_put32(v, VMCB_SAVE_DR7, 0x400u);
    vm_put32(v, VMCB_SAVE_DR6, 0xFFFF0FF0u);  /* reset value */
    vm_put32(v, VMCB_SAVE_RFLAGS, 0x2u);
    vm_put32(v, VMCB_SAVE_RIP, s->rip);
    vm_put32(v, VMCB_SAVE_RSP, s->guest_pa + SVM_GUEST_STACK_TOP);
    vm_put32(v, VMCB_SAVE_RAX, 0);
    vm_put32(v, VMCB_SAVE_CR2, 0);
    /* guest EFER: SVME must be SET in the guest's own EFER too (KVM's
     * nested_vmcb_check_save rejects the VMCB otherwise), LME clear for a
     * 32-bit no-paging guest. */
    vm_put32(v, VMCB_SAVE_EFER, EFER_SVME);
}

/* Dump the VMCB fields most likely to trip #VMFAILVALID so the serial log
 * carries a full picture of what was offered.  (vmm.cpp L543-584, trimmed) */
static void svm_dump_vmcb(SvmDriver* s)
{
    const uint8_t* v = (const uint8_t*)(uintptr_t)s->vmcb_pa;

    svm_puts("[SVM] VMCB @ "); svm_puthex(s->vmcb_pa);
    svm_puts(" page-aligned=");
    svm_putdec((uint32_t)((s->vmcb_pa & 0xFFFu) == 0u));
    svm_puts("\n");

    svm_puts("[SVM] ctrl: asid=");  svm_putdec(vm_get32(v, VMCB_ASID));
    svm_puts(" iopm=");             svm_puthex(vm_get32(v, VMCB_IOPM_BASE_PA));
    svm_puts(" msrpm=");            svm_puthex(vm_get32(v, VMCB_MSRPM_BASE_PA));
    svm_puts(" tlb_ctl=");          svm_putdec(vm_get32(v, VMCB_TLB_CTL));
    svm_puts("\n");

    svm_puts("[SVM] intercepts: CR=");  svm_puthex(vm_get32(v, VMCB_CR_INTERCEPT));
    svm_puts(" DR=");                   svm_puthex(vm_get32(v, VMCB_DR_INTERCEPT));
    svm_puts(" EXC=");                  svm_puthex(vm_get32(v, VMCB_EXC_INTERCEPT));
    svm_puts(" W3=");                   svm_puthex(vm_get32(v, VMCB_WORD3_INTERCEPT));
    svm_puts(" W4=");                   svm_puthex(vm_get32(v, VMCB_WORD4_INTERCEPT));
    svm_puts("\n");

    svm_puts("[SVM] state: CR0=");  svm_puthex(vm_get32(v, VMCB_SAVE_CR0));
    svm_puts(" CR3=");              svm_puthex(vm_get32(v, VMCB_SAVE_CR3));
    svm_puts(" CR4=");              svm_puthex(vm_get32(v, VMCB_SAVE_CR4));
    svm_puts(" EFER=");             svm_puthex(vm_get32(v, VMCB_SAVE_EFER));
    svm_puts("\n");

    svm_puts("[SVM] state: RIP=");  svm_puthex(vm_get32(v, VMCB_SAVE_RIP));
    svm_puts(" RSP=");              svm_puthex(vm_get32(v, VMCB_SAVE_RSP));
    svm_puts(" RFLAGS=");           svm_puthex(vm_get32(v, VMCB_SAVE_RFLAGS));
    svm_puts(" DR7=");              svm_puthex(vm_get32(v, VMCB_SAVE_DR7));
    svm_puts("\n");

    svm_puts("[SVM] cs: sel=");
    svm_puthex(vm_get32(v, VMCB_SAVE_CS) & 0xFFFFu);
    svm_puts(" attr=");
    svm_puthex((vm_get32(v, VMCB_SAVE_CS) >> 16) & 0xFFFFu);
    svm_puts(" limit=");
    svm_puthex(vm_get32(v, VMCB_SAVE_CS + 4));
    svm_puts(" gdtr: limit=");
    svm_puthex(vm_get32(v, VMCB_SAVE_GDTR + 4));
    svm_puts(" base=");
    svm_puthex(vm_get32(v, VMCB_SAVE_GDTR + 8));
    svm_puts("\n");
}

/* ---------------------------------------------------------------------------
 * vUART tx ring: bytes the guest writes with OUT are parked here, the same
 * carrier protocol vmm.cpp uses (plugins/vmm.cpp L275-281).
 * -------------------------------------------------------------------------*/
static void svm_uart_tx_push(SvmDriver* s, uint8_t b)
{
    uint32_t nxt = (s->uart_head + 1u) % SVM_UART_RING;
    if (nxt == s->uart_tail) return;         /* ring full: drop */
    s->uart_tx[s->uart_head] = b;
    s->uart_head = nxt;
}

/* ---------------------------------------------------------------------------
 * One VMRUN + return the #VMEXIT code (>= 0), or -2 on a genuine
 * #VMFAILVALID.  (plugins/vmm.cpp L586-619)
 *
 * RFLAGS alone is NOT a reliable VMFAIL test after a successful #VMEXIT --
 * host RFLAGS is restored from the hsave area and may carry stale ZF/CF -- so
 * CLC is executed before VMRUN and the VMCB EXITCODE is cross-checked.
 * -------------------------------------------------------------------------*/
static long svm_vmrun(SvmDriver* s)
{
    uint32_t flags = 0;
    uint32_t vmcb  = s->vmcb_pa;

    __asm__ __volatile__(
        "clc\n\t"
        ".byte 0x0F, 0x01, 0xD8\n\t"    /* VMRUN (rAX = VMCB physical addr) */
        "pushfl\n\t"
        "popl %1\n\t"
        : "+a"(vmcb), "=r"(flags)
        :
        : "memory", "cc");
    (void)vmcb;

    {
        const uint8_t* v = (const uint8_t*)(uintptr_t)s->vmcb_pa;
        uint32_t e0 = vm_get32(v, VMCB_EXITCODE);
        uint32_t e1 = vm_get32(v, VMCB_EXITCODE + 4);
        uint32_t i0 = vm_get32(v, VMCB_EXITINFO1);
        uint32_t i1 = vm_get32(v, VMCB_EXITINFO1 + 4);
        svm_puts("[SVM] post-vmrun: exitcode="); svm_puthex(e1);
        svm_puts(":");                          svm_puthex(e0);
        svm_puts(" info1=");                    svm_puthex(i1);
        svm_puts(":");                          svm_puthex(i0);
        svm_puts(" rflags=");                   svm_puthex(flags);
        svm_puts("\n");
    }

    {
        uint32_t exitcode = vm_get32((const uint8_t*)(uintptr_t)s->vmcb_pa,
                                     VMCB_EXITCODE);
        if (exitcode == 0u)
            return -2;                   /* VMCB rejected (#VMFAILVALID) */
        return (long)exitcode;
    }
}

/* ---------------------------------------------------------------------------
 * #VMEXIT dispatch.  (plugins/vmm.cpp L644-675)
 * Returns 1 = keep running, 0 = guest halted.
 *
 * Unlike vmm.cpp (which rebuilt the VMCB for every single-shot run and only
 * tracked gg->rip), we write the resumed RIP straight back into the live VMCB
 * save area so the next VMRUN continues where the guest left off.
 * -------------------------------------------------------------------------*/
static int svm_handle_exit(SvmDriver* s, uint32_t exitcode)
{
    uint8_t* v      = (uint8_t*)(uintptr_t)s->vmcb_pa;
    uint32_t info1   = vm_get32(v, VMCB_EXITINFO1);
    uint32_t next_rip = vm_get32(v, VMCB_NEXT_RIP);

    if (exitcode == SVM_EXITCODE_IOIO) {
        uint32_t port = info1 >> SVM_IOIO_PORT_SHIFT;
        uint32_t rax  = vm_get32(v, VMCB_SAVE_RAX);

        if (info1 & SVM_IOIO_TYPE_IN) {
            /* IN: deliver 0 (no queued rx bytes in this driver yet) */
            vm_put32(v, VMCB_SAVE_RAX, 0);
        } else {
            /* OUT: the guest wrote a byte -- park it in the vUART tx ring */
            svm_uart_tx_push(s, (uint8_t)(rax & 0xFFu));
            svm_puts("[SVM] vmexit IOIO out port=");
            svm_puthex(port);
            svm_puts(" val=");
            svm_puthex(rax & 0xFFu);
            svm_puts("\n");
        }
        s->rip = next_rip ? next_rip : s->rip;
        vm_put32(v, VMCB_SAVE_RIP, s->rip);
        vm_put32(v, VMCB_CLEAN, 0);      /* state modified: force reload */
        return 1;
    }

    if (exitcode == SVM_EXITCODE_HLT) {
        s->halted = 1;
        svm_puts("[SVM] vmexit HLT: guest halted\n");
        return 0;                        /* guest halted: loop is done */
    }

    /* unknown exit: advance past it to avoid an infinite loop */
    s->rip = next_rip ? next_rip : (s->rip + 1u);
    vm_put32(v, VMCB_SAVE_RIP, s->rip);
    vm_put32(v, VMCB_CLEAN, 0);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Run loop: VMRUN until the guest HLTs or the budget is exhausted.
 * Item 9: the VMEXIT dispatcher must be able to *continue* the guest, not
 * just handle a single exit.
 * -------------------------------------------------------------------------*/
static int svm_run_loop(SvmDriver* s, uint32_t budget)
{
    uint32_t i;

    s->halted = 0;
    s->exits  = 0;

    for (i = 0; i < budget && !s->halted; ++i) {
        long exit_code = svm_vmrun(s);

        if (exit_code == -2) {
            svm_puts("[SVM] #VMFAILVALID: VMCB rejected by the CPU\n");
            return DRIVER_ERR_IO;
        }
        if (exit_code < 0) {
            svm_puts("[SVM] VMRUN failed, code=");
            svm_putdec((uint32_t)(-exit_code));
            svm_puts("\n");
            return DRIVER_ERR_IO;
        }

        s->exits++;
        svm_puts("[SVM] #VMEXIT code=");
        svm_puthex((uint32_t)exit_code);
        svm_puts(" (run "); svm_putdec(s->exits); svm_puts(")\n");

        if (svm_handle_exit(s, (uint32_t)exit_code) == 0) break;  /* HLT */
    }

    svm_puts("[SVM] run loop done: exits="); svm_putdec(s->exits);
    svm_puts(" halted="); svm_putdec(s->halted);
    svm_puts("\n");

    if (s->halted) return DRIVER_OK;
    svm_puts("[SVM] vmexit budget exhausted before guest HLT\n");
    return DRIVER_ERR_BUSY;
}

/* ---------------------------------------------------------------------------
 * SVM entry: EFER.SVME, VM_HSAVE_PA, read-back verify, VMCB build, run.
 * (plugins/vmm.cpp L621-643)
 * -------------------------------------------------------------------------*/
static int svm_enter(SvmDriver* s)
{
    uint64_t efer;

    if (!s->api) return DRIVER_ERR_INVALID_ARG;

    /* 1) EFER.SVME = 1 and the host save area */
    efer = svm_rdmsr(MSR_EFER);
    svm_wrmsr(MSR_EFER, efer | (uint64_t)EFER_SVME);
    s->svme_enabled = 1;
    svm_wrmsr(MSR_VM_HSAVE_PA, (uint64_t)s->hsave_pa);

    /* verify the write landed (a QEMU/cpu model without EFER would drop it) */
    if (!(svm_rdmsr(MSR_EFER) & (uint64_t)EFER_SVME)) {
        svm_puts("[SVM] FAIL: EFER.SVME did not stick (no SVM on this cpu?)\n");
        return DRIVER_ERR_INIT_FAILED;
    }
    svm_puts("[SVM] EFER.SVME=1, VM_HSAVE_PA=");
    svm_puthex(s->hsave_pa);
    svm_puts("\n");

    /* 2) build the VMCB and enter */
    svm_build_vmcb(s);
    svm_dump_vmcb(s);

    /* 3) run until HLT (or budget) */
    return svm_run_loop(s, SVM_VMEXIT_BUDGET);
}

/* ---------------------------------------------------------------------------
 * Resource teardown: clear EFER.SVME, release every physical frame.
 * Safe to call on a partially-initialised instance (all fields are zeroed
 * first in init), and idempotent (each pa is cleared after being freed).
 * -------------------------------------------------------------------------*/
static void svm_release_all(SvmDriver* s)
{
    const DriverKernelAPI* api = s->api;

    if (!api) return;

    /* Disable SVM before touching anything the CPU is holding.  This is the
     * step vmm.cpp's vmm_shutdown() forgets; clearing an already-clear bit is
     * a harmless no-op, so this is naturally idempotent. */
    if (s->svme_enabled) {
        if (s->hsave_pa) svm_wrmsr(MSR_VM_HSAVE_PA, 0ull);
        {
            uint64_t efer = svm_rdmsr(MSR_EFER);
            svm_wrmsr(MSR_EFER, efer & ~(uint64_t)EFER_SVME);
        }
        s->svme_enabled = 0;
        svm_puts("[SVM] EFER.SVME=0 (SVM disabled)\n");
    }

    if (s->vmcb_pa && api->pmm_free_page) {
        api->pmm_free_page(s->vmcb_pa);
        s->vmcb_pa = 0;
    }
    if (s->hsave_pa && api->pmm_free_page) {
        api->pmm_free_page(s->hsave_pa);
        s->hsave_pa = 0;
    }
    if (s->iopm_pa && api->pmm_free_range) {
        api->pmm_free_range(s->iopm_pa, SVM_IOPM_PAGES);
        s->iopm_pa = 0;
    }
    if (s->msrpm_pa && api->pmm_free_range) {
        api->pmm_free_range(s->msrpm_pa, SVM_MSRPM_PAGES);
        s->msrpm_pa = 0;
    }
    if (s->guest_pa && api->pmm_free_page) {
        api->pmm_free_page(s->guest_pa);
        s->guest_pa = 0;
    }
    s->rip = 0;
}

/* ---------------------------------------------------------------------------
 * Lifecycle: init
 * -------------------------------------------------------------------------*/
static int svm_init(const DriverKernelAPI* api, void* ctx)
{
    SvmDriver* s = &g_svm;
    int rc;

    /* IMPORTANT: ctx belongs to the loader (its DriverRec).  The driver must
     * NOT reinterpret it -- it owns its own static state. */
    (void)ctx;

    if (!api) return DRIVER_ERR_INVALID_ARG;
    if (api->abi_version != DRV_ABI_VERSION) return DRIVER_ERR_ABI_MISMATCH;
    if (!api->printk || !api->pmm_alloc_page || !api->pmm_free_page ||
        !api->pmm_alloc_range || !api->pmm_free_range || !api->cpuid)
        return DRIVER_ERR_INVALID_ARG;

    /* Reset the static instance in place: a hot reload runs init() again on
     * the same object, and a stale field here is exactly how a double-free
     * happens.  Reset BEFORE binding api so nothing logs into a dead console. */
    {
        uint32_t i;
        uint8_t* p = (uint8_t*)s;
        for (i = 0; i < (uint32_t)sizeof(SvmDriver); ++i) p[i] = 0;
    }
    s->api = api;

    svm_puts("[SVM] init: AMD-V driver binding\n");

    /* 1) detection (negative on a non-AMD / SVM-disabled CPU, no MSR touched) */
    rc = svm_detect();
    if (rc != DRIVER_OK) {
        svm_puts("[SVM] init: detection failed, rc=");
        svm_putdec((uint32_t)(-rc));
        svm_puts("\n");
        s->api = 0;
        return rc;
    }

    /* 2) physical frames the CPU requires: VMCB and VM_HSAVE_PA must be
     *    4 KiB page aligned, so they come from the PMM, not kmalloc. */
    s->vmcb_pa  = api->pmm_alloc_page();
    s->hsave_pa = api->pmm_alloc_page();
    s->iopm_pa  = api->pmm_alloc_range(SVM_IOPM_PAGES);
    s->msrpm_pa = api->pmm_alloc_range(SVM_MSRPM_PAGES);
    s->guest_pa = api->pmm_alloc_page();

    if (!s->vmcb_pa || !s->hsave_pa || !s->iopm_pa ||
        !s->msrpm_pa || !s->guest_pa) {
        svm_puts("[SVM] init: out of physical memory\n");
        svm_release_all(s);          /* frees whatever did succeed */
        s->api = 0;
        return DRIVER_ERR_NO_MEMORY;
    }

    /* Page alignment is an architectural requirement, not a nicety. */
    if ((s->vmcb_pa  & 0xFFFu) || (s->hsave_pa & 0xFFFu) ||
        (s->iopm_pa  & 0xFFFu) || (s->msrpm_pa & 0xFFFu) ||
        (s->guest_pa & 0xFFFu)) {
        svm_puts("[SVM] init: PMM returned a non-page-aligned frame\n");
        svm_release_all(s);
        s->api = 0;
        return DRIVER_ERR_ISOLATION;
    }

    svm_puts("[SVM] frames: vmcb="); svm_puthex(s->vmcb_pa);
    svm_puts(" hsave=");             svm_puthex(s->hsave_pa);
    svm_puts(" iopm=");              svm_puthex(s->iopm_pa);
    svm_puts(" msrpm=");             svm_puthex(s->msrpm_pa);
    svm_puts(" guest=");             svm_puthex(s->guest_pa);
    svm_puts("\n");

    /* 3) all-zero bitmaps = intercept nothing beyond the VMCB control bits */
    svm_memzero((uint8_t*)(uintptr_t)s->iopm_pa,  SVM_IOPM_BYTES);
    svm_memzero((uint8_t*)(uintptr_t)s->msrpm_pa, SVM_MSRPM_BYTES);

    /* 4) guest stub + GDT, then the entry state */
    svm_build_guest_stub(s);
    s->rip = s->guest_pa + SVM_GUEST_STUB_OFF;

    /* 5) EFER.SVME / VM_HSAVE_PA / VMCB / VMRUN / #VMEXIT dispatch */
    rc = svm_enter(s);
    if (rc != DRIVER_OK) {
        svm_puts("[SVM] init: entry/run failed, rolling back\n");
        svm_release_all(s);
        s->api = 0;
        return rc;
    }

    /* 6) publish the device only after the machine is actually live */
    rc = api->register_device("svm", (void*)s);
    if (rc != DRIVER_OK) {
        svm_puts("[SVM] init: register_device failed\n");
        svm_release_all(s);
        s->api = 0;
        return rc;
    }
    s->device_registered = 1;
    s->magic = SVM_LIVE_MAGIC;

    svm_puts("[SVM] Init Success\n");
    return DRIVER_OK;
}

/* ---------------------------------------------------------------------------
 * Lifecycle: unload.  Idempotent: init() sets magic last, unload() clears it
 * first, so a double unload is a no-op instead of a double free.
 * -------------------------------------------------------------------------*/
static void svm_unload(void* ctx)
{
    SvmDriver* s = &g_svm;

    (void)ctx;

    if (s->magic != SVM_LIVE_MAGIC) {
        /* Never initialised, or already unloaded. */
        return;
    }
    s->magic = 0;                    /* fence: a second call stops here */

    if (s->api) {
        if (s->device_registered && s->api->unregister_device)
            s->api->unregister_device("svm");
        s->device_registered = 0;
        svm_puts("[SVM] unload: device unregistered\n");
    }

    /* Clear EFER.SVME first, then release every physical frame. */
    svm_release_all(s);

    s->magic = 0;
    s->api   = 0;
    svm_puts("[SVM] unload: resources released\n");
}

/* ---------------------------------------------------------------------------
 * Descriptor.  The symbol table is filled in-place by the loader, so the
 * header must be writable in the image.
 * -------------------------------------------------------------------------*/
/* Only what this driver actually imports.  Everything else (page allocation,
 * CPUID, device registry) arrives through the DriverKernelAPI vtable, so there
 * is no second, redundant path to the same kernel service. */
static DriverSymbolReq g_svm_symbols[] = {
    { "printk",  0 },
    { 0, 0 }
};

static const char* const g_svm_deps[] = { 0 };   /* no dependencies */

DriverHeader nexos_driver_header = {
    DRV_MAGIC,              /* magic                                  */
    DRV_ABI_VERSION,        /* abi_version                            */
    0x00010000u,            /* version 1.0                            */
    sizeof(DriverHeader),   /* header_size                            */
    "svm",                  /* name                                   */
    "AMD SVM (AMD-V) virtualisation driver, ported from plugins/vmm.cpp",
    g_svm_deps,             /* deps                                   */
    g_svm_symbols,          /* symbols                                */
    1,                      /* symbol_count                           */
    svm_init,               /* init                                   */
    svm_unload,             /* unload                                 */
    0,                      /* suspend (optional)                     */
    0,                      /* resume  (optional)                     */
    0,                      /* flags                                  */
    { 0, 0, 0, 0 }          /* reserved                               */
};
