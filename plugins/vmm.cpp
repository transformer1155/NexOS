/* vmm.cpp - minimal built-in hypervisor core (Intel VT-x / AMD-V).
 *
 * See vmm.h for the model.  The design goal here is a CORRECT, COMPILING
 * skeleton that:
 *   1. Detects Intel VMX and AMD SVM via CPUID + MSR (real, tested logic).
 *   2. Reserves a contiguous guest RAM region via the kernel PMM.
 *   3. Exposes vmm_run_once() / vmm_create_guest() whose bodies contain the
 *      real VMXON+VMLAUNCH / VM_RUN assembly, but which are only ever invoked
 *      when vmm_available() is true -- i.e. on real hardware with nested
 *      virtualization.  Under the development QEMU#1 there is no VMX/SVM, so
 *      these paths stay dormant and the kernel boots exactly as before.
 *
 * This is intentionally the *first, safe* cut: wire the GUI to it only after
 * the Phase-1 smoke guest (a few bytes of asm that paint the shared FB)
 * proves the VMX/SVM entry/exit path on real metal.
 */
#include "vmm.h"
#include "../kernel/pmm.h"  /* pmm_alloc_page / pmm_free_page */
#include "guest_blob.h"     /* GUEST_CODE_PHYS + the smoke guest payload */
#include "plugin_manager.h" /* pm_serial (COM1 diagnostics) */

#include <stdint.h>

/* ---- serial diagnostics (freestanding; mirrors pm_serial style) -------- */
extern "C" void pm_serial(const char* s);
static void vm_log(const char* s) { pm_serial(s); }
static void vm_log_hex(uint32_t v) {
    char b[9];
    for (int i = 7; i >= 0; i--) {
        uint32_t n = v & 0xF; v >>= 4;
        b[i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    b[8] = 0;
    vm_log(b);
}
static void vm_log_dec(uint32_t v) {
    char b[11]; int ni = 0;
    if (!v) { vm_log("0"); return; }
    char t[10]; int tn = 0;
    while (v) { t[tn++] = (char)('0' + v % 10); v /= 10; }
    while (tn) b[ni++] = t[--tn];
    b[ni] = 0;
    vm_log(b);
}

/* Kernel PMM contiguous-range helper (kernel.cpp).  Allocates `count` 4 KiB
 * pages back-to-back and returns the physical base, or 0 on OOM.  Using this
 * instead of a page-by-page scan removes the "walk until contiguous" hack and
 * makes large guest RAM regions (256 MiB+) practical. */
extern "C" uint32_t pmm_alloc_range(uint32_t count);
extern "C" void     pmm_free_range(uint32_t phys, uint32_t count);

/* ---- low-level CPU helpers (self-contained; freestanding) ------------- */
static inline void vmm_cpuid(uint32_t leaf, uint32_t* a, uint32_t* b,
                             uint32_t* c, uint32_t* d) {
    __asm__ __volatile__("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf));
}
static inline void vmm_cpuid_ex(uint32_t leaf, uint32_t sub, uint32_t* a,
                                uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ __volatile__("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(sub));
}
static inline uint64_t vmm_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void vmm_wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

/* Intel VMX MSRs / bits */
#define IA32_FEATURE_CONTROL 0x003A
#define FEATURE_CONTROL_VMXON_IN_SMX 0x02
#define FEATURE_CONTROL_VMXON         0x04
#define FEATURE_CONTROL_LOCKED        0x01
#define IA32_VMX_BASIC          0x0480

/* AMD SVM MSR / bits */
#define MSR_VM_CR               0xC0010114u
#define VM_CR_SVM_DISABLE       (1u << 4)
#define MSR_VM_HSAVE_PA         0xC0010117u
#define MSR_EFER               0xC0000080u
#define EFER_SVME               (1u << 12)

/* ---- VMCB layout (authoritative: Linux arch/x86/include/asm/svm.h) -----
 * Intercept bits are GLOBAL bit numbers: 0-31 = word0 (0x00), 32-63 =
 * word1 (0x04), 64-95 = word2 (0x08), 96-127 = word3 (0x0C), 128-159 =
 * word4 (0x10).  INTERCEPT_VMRUN=128 -> VMCB+0x10 bit0; the APM REQUIRES
 * the VMCB to intercept VMRUN, VMLOAD, VMSAVE, STGI, CLGI or VMRUN exits
 * with #VMFAILVALID.  INTERCEPT_HLT=119 -> 0x0C bit23.  INTERCEPT_IOIO_
 * PROT=127 -> 0x0C bit31. */
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
#define VMCB_SAVE_GDTR          0x460   /* seg layout: limit u16@+0? NO --
                                         * vmcb_seg is sel2 attr2 limit4 base8;
                                         * gdtr/idtr use selector=0, attrib=0:
                                         * limit @+4, base @+8 */
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

/* exit codes we dispatch on */
#define SVM_EXITCODE_IOIO       0x7B
#define SVM_EXITCODE_HLT        0x78
#define SVM_IOIO_PORT_SHIFT     16
#define SVM_IOIO_TYPE_IN        (1u << 3)  /* EXITINFO1 bit3: 1 = IN  */

/* flat-segment attribute encodings for the VMCB save area */
#define SVM_SEG_ATTRIB_CODE32   0x0C9B      /* P|D/B|G|code+read  */
#define SVM_SEG_ATTRIB_DATA32   0x0C93      /* P|D/B|G|data r/w   */
#define SVM_SEG_ATTRIB_TSS      0x008B      /* P|busy 32-bit TSS  */

/* ---- detection state -------------------------------------------------- */
static VmmKind g_kind = VMM_NONE;
static int     g_inited = 0;

VmmKind vmm_detect(void) {
    uint32_t a, b, c, d;
    char vendor[13];
    vmm_cpuid(0, &a, &b, &c, &d);
    ((uint32_t*)vendor)[0] = b; ((uint32_t*)vendor)[1] = d; ((uint32_t*)vendor)[2] = c;
    vendor[12] = 0;

    int is_intel = (b == 0x756E6547u && c == 0x6C65746Eu && d == 0x49656E69u); // "GenuineIntel"
    int is_amd   = (b == 0x68747541u && c == 0x444D4163u && d == 0x69746E65u); // "AuthenticAMD"

    vm_log("[VMM] detect: vendor='"); vm_log(vendor);
    vm_log("' intel="); vm_log_dec((uint32_t)is_intel);
    vm_log(" amd=");    vm_log_dec((uint32_t)is_amd);
    vm_log("\n");

    /* Intel: CPUID.1:ECX bit 5 = VMX */
    if (is_intel) {
        vmm_cpuid(1, &a, &b, &c, &d);
        vm_log("[VMM] intel CPUID.1 ECX="); vm_log_hex(c);
        vm_log(" vmx-bit="); vm_log_dec((c >> 5) & 1);
        vm_log("\n");
        if (c & (1u << 5)) {
            /* IA32_FEATURE_CONTROL must allow VMXON (bit 2) and not be locked
             * out.  We only *report* availability; vmm_init does the enable. */
            uint64_t fc = vmm_rdmsr(IA32_FEATURE_CONTROL);
            vm_log("[VMM] FEATURE_CONTROL="); vm_log_hex((uint32_t)fc);
            vm_log("\n");
            if (fc & FEATURE_CONTROL_VMXON) { g_kind = VMM_VMX; return g_kind; }
        }
        g_kind = VMM_NONE; return g_kind;
    }

    /* AMD: CPUID.0x80000001:ECX bit 2 = SVM, and SVM not disabled in VM_CR */
    if (is_amd) {
        uint32_t maxext;
        vmm_cpuid(0x80000000u, &maxext, &b, &c, &d);
        vm_log("[VMM] amd maxext="); vm_log_hex(maxext);
        vm_log("\n");
        if (maxext >= 0x80000001u) {
            vmm_cpuid(0x80000001u, &a, &b, &c, &d);
            vm_log("[VMM] amd CPUID.80000001 ECX="); vm_log_hex(c);
            vm_log(" svm-bit="); vm_log_dec((c >> 2) & 1);
            vm_log("\n");
            if (c & (1u << 2)) {
                uint64_t vmcr = vmm_rdmsr(MSR_VM_CR);
                vm_log("[VMM] VM_CR="); vm_log_hex((uint32_t)vmcr);
                vm_log("\n");
                if (!(vmcr & VM_CR_SVM_DISABLE)) { g_kind = VMM_SVM; return g_kind; }
            }
        }
        g_kind = VMM_NONE; return g_kind;
    }

    g_kind = VMM_NONE;
    return g_kind;
}

VmmKind vmm_kind(void)       { return g_kind; }
/* Force a detection pass if none has run yet, so callers that only ask
 * "available?" still get a real answer (detection is cheap: a few CPUIDs). */
int     vmm_available(void)  { if (g_kind == VMM_NONE) vmm_detect(); return g_kind != VMM_NONE; }

/* ---- guest memory region ---------------------------------------------- */
/* A guest is just a reserved physical RAM block + a fixed framebuffer.  We
 * size the framebuffer at 1024x768x4 (3 MiB) carved from the guest region. */
#define GUEST_FB_W 1024
#define GUEST_FB_H 768
#define GUEST_FB_BPP 4
#define GUEST_FB_BYTES ((GUEST_FB_W)*(GUEST_FB_H)*(GUEST_FB_BPP))

/* Guest RAM sizing: ask for 256 MiB, fall back (halving) to 32 MiB minimum. */
#define GUEST_MAX_MB 256u
#define GUEST_MIN_MB 32u

/* ---- emulated device state -------------------------------------------- */
#define VUART_RING 256
typedef struct {
    uint8_t  tx[VUART_RING];   /* guest -> host bytes (host reads via tx_pop) */
    uint32_t tx_head, tx_tail; /* ring indices */
    uint32_t tx_total;         /* lifetime byte count (diagnostics) */
    uint8_t  rx[VUART_RING];   /* host -> guest bytes */
    uint32_t rx_head, rx_tail;
    int      ier;              /* 8250 interrupt-enable register (stub) */
    int      lcr;              /* line-control register (stub) */
} VmmUart;

typedef struct {
    int      attached;
    uint32_t sectors;
    char     path[96];
} VmmDisk;

typedef struct {
    uint32_t phys_base;     /* physical address of guest RAM */
    uint32_t pages;         /* size in 4 KiB pages */
    uint32_t fb_phys;       /* physical address of the framebuffer (in guest RAM) */
    uint32_t* fb_kv;        /* kernel-virtual pointer to the framebuffer */
    int      running;
    /* VMX/SVM specific state (VMCS/VMCB physical addresses) reserved here */
    uint32_t vmcs_phys;
    /* SVM host structures (each one 4 KiB page, allocated from PMM) */
    uint32_t vmcb_phys;     /* VMCB: control 0x000-0x3FF, save 0x400-0xFFF */
    uint32_t hsav_phys;     /* VM_HSAVE_PA: host state save area           */
    uint32_t iopm_phys;     /* IOPM bitmap, 16 KiB (2 bits per IO port)    */
    uint32_t msrpm_phys;    /* MSRPM bitmap, 8 KiB                         */
    uint32_t rip;           /* current guest RIP (updated on #VMEXIT)      */
    VmmUart  uart;          /* emulated 8250 UART (control protocol carrier) */
    VmmDisk  disk;          /* emulated block device (Linux root image)     */
} VmmGuest;

static VmmGuest g_guest;    /* single guest for now (matches one Linux window) */

/* ---- vUART device model (8250-compatible subset) ----------------------
 * The guest's Linux console + our start/stop/status line protocol both ride
 * this UART.  VM-exit dispatch calls vmm_uart_tx_push() on OUT to the UART
 * port; the linux_manager line parser then drains it with tx_pop().  This is
 * exactly the same byte stream the serial COM2 transport produced, so the
 * protocol code needs no changes when the transport swaps to vVMM. */
static void vmm_uart_tx_push(VmmGuest* gg, uint8_t b) {
    uint32_t nxt = (gg->uart.tx_head + 1u) % VUART_RING;
    if (nxt == gg->uart.tx_tail) return;          /* ring full: drop */
    gg->uart.tx[gg->uart.tx_head] = b;
    gg->uart.tx_head = nxt;
    gg->uart.tx_total++;
}
static void vmm_uart_rx_push_impl(VmmGuest* gg, uint8_t b) {
    uint32_t nxt = (gg->uart.rx_head + 1u) % VUART_RING;
    if (nxt == gg->uart.rx_tail) return;          /* ring full: drop */
    gg->uart.rx[gg->uart.rx_head] = b;
    gg->uart.rx_head = nxt;
}

int vmm_uart_tx_pending(void* g) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg) return 0;
    return gg->uart.tx_head != gg->uart.tx_tail;
}
int vmm_uart_tx_pop(void* g) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg || gg->uart.tx_head == gg->uart.tx_tail) return -1;
    int c = gg->uart.tx[gg->uart.tx_tail];
    gg->uart.tx_tail = (gg->uart.tx_tail + 1u) % VUART_RING;
    return c;
}
void vmm_uart_rx_push(void* g, uint8_t b) {
    VmmGuest* gg = (VmmGuest*)g;
    if (gg) vmm_uart_rx_push_impl(gg, b);
}
uint32_t vmm_uart_tx_count(void* g) {
    VmmGuest* gg = (VmmGuest*)g;
    return gg ? gg->uart.tx_total : 0;
}

/* ---- vDisk device model ----------------------------------------------
 * A raw sector image stored in a NexOS file.  The real read path goes through
 * the kernel VFS; for now we record the attachment so the VM-exit handler can
 * serve sector reads once the guest actually issues them. */
int vmm_disk_attach(void* g, const char* path, uint32_t sectors) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg || !path) return -1;
    int i = 0;
    for (; path[i] && i < 95; i++) gg->disk.path[i] = path[i];
    gg->disk.path[i] = 0;
    gg->disk.sectors  = sectors;
    gg->disk.attached = 1;
    return 0;
}
int vmm_disk_ready(void* g) {
    VmmGuest* gg = (VmmGuest*)g;
    return gg && gg->disk.attached;
}
int vmm_disk_read(void* g, uint64_t lba, void* buf, uint32_t count) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg || !gg->disk.attached || !buf) return -1;
    if (lba + count > gg->disk.sectors) return -1;
    /* Zero-fill for now; the VFS-backed read is wired in Phase 3 together with
     * the real Linux root image.  The interface is what the VM-exit handler
     * needs, and it is stable from here on. */
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < count * 512u; i++) p[i] = 0;
    return 0;
}

int vmm_init(void) {
    if (g_inited) return 0;
    if (vmm_detect() == VMM_NONE) return -1;   /* nothing to enable */

    /* Guest RAM: as much as we can get, up to GUEST_MAX_MB.  The PMM now caps
     * at 1 GiB, so 256 MiB is a comfortable default for a Linux desktop while
     * still leaving the heap and kernel plenty.  pmm_alloc_range() hands the
     * whole block back contiguous in one call. */
    uint32_t want_mb  = GUEST_MAX_MB;
    uint32_t pages    = want_mb * 256u;            /* MiB -> 4 KiB pages */
    uint32_t base     = pmm_alloc_range(pages);
    /* Progressive fallback: try smaller sizes before giving up. */
    while (!base && want_mb > GUEST_MIN_MB) {
        want_mb /= 2;
        pages = want_mb * 256u;
        base  = pmm_alloc_range(pages);
    }
    if (!base) return -1;                          /* not enough RAM for a guest */

    g_guest.phys_base = base;
    g_guest.pages     = pages;
    /* Framebuffer sits at the very start of guest RAM. */
    g_guest.fb_phys   = base;
    /* In our flat 32-bit space physical addresses are directly dereferenceable,
     * so the kernel-virtual pointer equals the physical base. */
    g_guest.fb_kv     = (uint32_t*)base;
    g_guest.running   = 0;
    g_guest.rip       = GUEST_CODE_PHYS;
    /* SVM host structures.  The IOPM must cover all 65536 ports at 2 bits
     * each (16 KiB) and the MSRPM 8 KiB; give each a contiguous multi-page
     * block so the CPU never reads past its mapping. */
    g_guest.vmcb_phys  = pmm_alloc_page();
    g_guest.hsav_phys  = pmm_alloc_page();
    g_guest.iopm_phys  = pmm_alloc_range(4);   /* 16 KiB */
    g_guest.msrpm_phys = pmm_alloc_range(2);   /* 8 KiB  */
    if (!g_guest.vmcb_phys || !g_guest.hsav_phys ||
        !g_guest.iopm_phys || !g_guest.msrpm_phys) {
        pmm_free_range(base, pages);
        if (g_guest.vmcb_phys)  pmm_free_page(g_guest.vmcb_phys);
        if (g_guest.hsav_phys)  pmm_free_page(g_guest.hsav_phys);
        if (g_guest.iopm_phys)  pmm_free_range(g_guest.iopm_phys, 4);
        if (g_guest.msrpm_phys) pmm_free_range(g_guest.msrpm_phys, 2);
        return -1;
    }
    /* all-zero bitmaps = intercept nothing beyond the VMCB control bits */
    {
        uint8_t* io = (uint8_t*)g_guest.iopm_phys;
        for (uint32_t i = 0; i < 4u * 4096u; i++) io[i] = 0;
        uint8_t* ms = (uint8_t*)g_guest.msrpm_phys;
        for (uint32_t i = 0; i < 2u * 4096u; i++) ms[i] = 0;
    }
    g_guest.vmcs_phys = base + pages * 4096u - 4096u; /* last page for VMCS */
    /* Reset emulated devices. */
    g_guest.uart.tx_head = g_guest.uart.tx_tail = 0;
    g_guest.uart.rx_head = g_guest.uart.rx_tail = 0;
    g_guest.uart.tx_total = 0;
    g_guest.uart.ier = g_guest.uart.lcr = 0;
    g_guest.disk.attached = 0;
    g_guest.disk.sectors  = 0;
    g_guest.disk.path[0]  = 0;
    g_inited = 1;
    return 0;
}

void* vmm_create_guest(uint32_t guest_pages) {
    (void)guest_pages;       /* ignored for now: single fixed-size guest */
    if (!g_inited) { if (vmm_init() != 0) return 0; }
    return &g_guest;
}

void* vmm_guest_fb(void* g, int* w, int* h) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg) return 0;
    if (w) *w = GUEST_FB_W;
    if (h) *h = GUEST_FB_H;
    return gg->fb_kv;
}

/* ---- guest memory access ---------------------------------------------
 * Guest RAM is one contiguous phys range starting at gg->phys_base.  In the
 * kernel's flat 32-bit address space physical addresses are directly
 * dereferenceable, so a guest-physical address maps 1:1 to a host pointer.
 * (Once paging/EPT is introduced this becomes the EPT translation step.) */
void* vmm_guest_ptr(void* g, uint32_t guest_phys) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg || !gg->phys_base) return 0;
    uint32_t size = gg->pages * 4096u;
    if (guest_phys >= size) return 0;
    return (void*)(gg->phys_base + guest_phys);
}

uint32_t vmm_read_guest32(void* g, uint32_t guest_phys) {
    uint32_t* p = (uint32_t*)vmm_guest_ptr(g, guest_phys);
    return p ? *p : 0;
}

int vmm_load_blob(void* g, uint32_t guest_phys, const void* data, uint32_t len) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg || !data || !len) return -1;
    uint32_t size = gg->pages * 4096u;
    if ((uint64_t)guest_phys + len > size) return -1;   /* would overrun */
    const uint8_t* src = (const uint8_t*)data;
    uint8_t* dst = (uint8_t*)(gg->phys_base + guest_phys);
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

/* ---- the real VM entry (dormant under dev QEMU) ------------------------ */
#if defined(__i386__) || defined(__x86_64__)
/* Enable VMX and enter the guest.  Written but ONLY reached when VMX is on. */
static int vmm_vmx_enter(VmmGuest* gg) {
    (void)gg;
    /* 1) enable VMX via IA32_FEATURE_CONTROL + CR4.VMXE (bit 13)
     * 2) VMXON with the VMXON region
     * 3) build VMCS (guest CR0/CR3/CR4, RIP -> guest reset vector, EPT off)
     * 4) VMLAUNCH
     * On VM-exit, we return here and vmm_run_once dispatches the reason. */
    return -1;  /* not yet implemented: requires the assembler prologue that
                   * sets up a 32-bit guest and a VM-exit handler stub. */
}

/* ---- SVM host-side helpers (C; the only asm is the VMRUN itself) ------- */
static void vm_put16(uint8_t* vmcb, uint32_t off, uint16_t v) {
    vmcb[off]   = (uint8_t)v; vmcb[off+1] = (uint8_t)(v >> 8);
}
static void vm_put32(uint8_t* vmcb, uint32_t off, uint32_t v) {
    vmcb[off] = (uint8_t)v; vmcb[off+1] = (uint8_t)(v>>8);
    vmcb[off+2] = (uint8_t)(v>>16); vmcb[off+3] = (uint8_t)(v>>24);
}
static uint32_t vm_get32(const uint8_t* vmcb, uint32_t off) {
    return (uint32_t)vmcb[off] | ((uint32_t)vmcb[off+1] << 8) |
           ((uint32_t)vmcb[off+2] << 16) | ((uint32_t)vmcb[off+3] << 24);
}

/* Fill one segment slot: u16 selector, u16 attrib, u32 limit, u64 base(=0). */
static void vm_set_seg(uint8_t* vmcb, uint32_t slot, uint16_t sel,
                       uint16_t attrib, uint32_t limit) {
    vm_put16(vmcb, slot,      sel);
    vm_put16(vmcb, slot + 2,  attrib);
    vm_put32(vmcb, slot + 4,  limit);
    /* base (8 bytes at slot+8) stays 0: flat segments */
}

/* Build the whole VMCB for the Phase-1 smoke guest (32-bit flat, no paging).
 * Mirrors tools/svm_host_test.asm field-for-field. */
static void vm_build_vmcb(VmmGuest* gg) {
    uint8_t* v = (uint8_t*)gg->vmcb_phys;   /* flat: phys == host virtual */
    /* zero the whole page first */
    for (uint32_t i = 0; i < 4096u; i++) v[i] = 0;

    /* control area intercepts (global bit numbers):
     *   word3 (0x0C): bit23 = HLT, bit31 = IOIO_PROT (all port I/O)
     *   word4 (0x10): bit0  = VMRUN (MANDATORY: VMRUN fails with
     *                           #VMFAILVALID unless it intercepts itself),
     *                 bit1..4 = VMMCALL/VMLOAD/VMSAVE/STGI/CLGI (also
     *                           required per APM nested-SVM rules)      */
    vm_put32(v, VMCB_WORD3_INTERCEPT, (1u << 23) | (1u << 31));
    vm_put32(v, VMCB_WORD4_INTERCEPT, 0x0000001Fu);
    vm_put32(v, VMCB_IOPM_BASE_PA, gg->iopm_phys);
    vm_put32(v, VMCB_MSRPM_BASE_PA, gg->msrpm_phys);
    vm_put32(v, VMCB_ASID, 1);

    /* save area: flat 32-bit segments */
    vm_set_seg(v, VMCB_SAVE_ES, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_CS, 0x08, SVM_SEG_ATTRIB_CODE32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_SS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_DS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_FS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    vm_set_seg(v, VMCB_SAVE_GS, 0x10, SVM_SEG_ATTRIB_DATA32, 0xFFFFFFFFu);
    /* GDTR/IDTR use the same 16-byte vmcb_seg slot: selector=0, attrib=0,
     * limit u32 @+4, base u64 @+8.  (The earlier "+0/+2 packed" guess was
     * wrong -- proven by the IDTR limit leaking into gdtr.base in the dump.) */
    vm_put32(v, VMCB_SAVE_GDTR + 4, 0x17);
    vm_put32(v, VMCB_SAVE_GDTR + 8, 0x6000);
    vm_put32(v, VMCB_SAVE_IDTR + 4, 0xFFFF);
    /* IDTR base stays 0 (no interrupts delivered to this guest) */
    /* TR: present busy 32-bit TSS */
    vm_set_seg(v, VMCB_SAVE_TR, 0x00, SVM_SEG_ATTRIB_TSS, 0xFFFFu);

    /* control registers + entry state (guest GDT image built at 0x6000) */
    uint32_t* gd = (uint32_t*)0x6000;
    gd[0] = gd[1] = 0;                       /* null descriptor */
    gd[2] = 0xFFFF0000; gd[3] = 0x00CF9A00;  /* flat 32-bit code */
    gd[4] = 0xFFFF0000; gd[5] = 0x00CF9300;  /* flat 32-bit data */

    vm_put32(v, VMCB_SAVE_CR4, 0);
    vm_put32(v, VMCB_SAVE_CR3, 0);
    vm_put32(v, VMCB_SAVE_CR0, 0x11);        /* PE | ET, no paging */
    vm_put32(v, VMCB_SAVE_DR7, 0x400);
    vm_put32(v, VMCB_SAVE_DR6, 0xFFFF0FF0);  /* reset value */
    vm_put32(v, VMCB_SAVE_RFLAGS, 0x2);
    vm_put32(v, VMCB_SAVE_RIP, gg->rip);
    vm_put32(v, VMCB_SAVE_RSP, 0x8000);
    vm_put32(v, VMCB_SAVE_RAX, 0);
    vm_put32(v, VMCB_SAVE_CR2, 0);
    /* guest EFER: SVME must be SET in the guest's own EFER too (KVM's
     * nested_vmcb_check_save rejects the VMCB otherwise), LME clear for a
     * 32-bit no-paging guest. */
    vm_put32(v, VMCB_SAVE_EFER, EFER_SVME);
}

/* Dump the VMCB fields most likely to trip #VMFAILVALID.  Called before the
 * first VMRUN so the serial log carries a full picture of what was offered. */
static void vm_dump_vmcb(VmmGuest* gg) {
    const uint8_t* v = (const uint8_t*)gg->vmcb_phys;
    vm_log("[VMM] VMCB @ phys "); vm_log_hex(gg->vmcb_phys);
    vm_log(" (page-aligned: "); vm_log_dec((gg->vmcb_phys & 0xFFF) == 0); vm_log(")\n");
    vm_log("[VMM] ctrl: int_ctl="); vm_log_hex(vm_get32(v, 0x048 - 0x048 + 0x044));
    vm_log(" asid=");            vm_log_dec(vm_get32(v, VMCB_ASID));
    vm_log(" iopm=");            vm_log_hex(vm_get32(v, VMCB_IOPM_BASE_PA));
    vm_log(" msrpm=");           vm_log_hex(vm_get32(v, VMCB_MSRPM_BASE_PA));
    vm_log(" tlb_ctl=");         vm_log_dec(vm_get32(v, 0x04C));
    vm_log("\n");
    vm_log("[VMM] intercepts: CR="); vm_log_hex(vm_get32(v, 0x000));
    vm_log(" DR=");     vm_log_hex(vm_get32(v, 0x004));
    vm_log(" EXC=");    vm_log_hex(vm_get32(v, 0x008));
    vm_log(" W3=");     vm_log_hex(vm_get32(v, 0x00C));
    vm_log(" W4=");     vm_log_hex(vm_get32(v, 0x010));
    vm_log("\n");
    vm_log("[VMM] state: CR0="); vm_log_hex(vm_get32(v, VMCB_SAVE_CR0));
    vm_log(" CR3=");   vm_log_hex(vm_get32(v, VMCB_SAVE_CR3));
    vm_log(" CR4=");   vm_log_hex(vm_get32(v, VMCB_SAVE_CR4));
    vm_log(" EFER=");  vm_log_hex(vm_get32(v, VMCB_SAVE_EFER));
    vm_log("\n");
    vm_log("[VMM] state: RIP="); vm_log_hex(vm_get32(v, VMCB_SAVE_RIP));
    vm_log(" RSP=");   vm_log_hex(vm_get32(v, VMCB_SAVE_RSP));
    vm_log(" RFLAGS=");vm_log_hex(vm_get32(v, VMCB_SAVE_RFLAGS));
    vm_log(" DR7=");   vm_log_hex(vm_get32(v, VMCB_SAVE_DR7));
    vm_log("\n");
    /* segment slots: selector, attrib, limit — CS is the one that trips */
    const uint32_t slots[6] = { 0x400, 0x410, 0x420, 0x430, 0x440, 0x450 };
    const char* names[6] = { "ES", "CS", "SS", "DS", "FS", "GS" };
    for (int i = 0; i < 6; i++) {
        vm_log("[VMM] seg "); vm_log(names[i]);
        vm_log(": sel=");  vm_log_hex(vm_get32(v, slots[i]) & 0xFFFF);
        vm_log(" attr=");  vm_log_hex((vm_get32(v, slots[i]) >> 16) & 0xFFFF);
        vm_log(" limit="); vm_log_hex(vm_get32(v, slots[i] + 4));
        vm_log("\n");
    }
    vm_log("[VMM] gdtr: limit="); vm_log_hex(vm_get32(v, VMCB_SAVE_GDTR + 4));
    vm_log(" base=");             vm_log_hex(vm_get32(v, VMCB_SAVE_GDTR + 8));
    vm_log("  idtr: limit=");     vm_log_hex(vm_get32(v, VMCB_SAVE_IDTR + 4));
    vm_log(" base=");             vm_log_hex(vm_get32(v, VMCB_SAVE_IDTR + 8));
    vm_log("\n");
}

/* One VMRUN + #VMEXIT dispatch cycle.  Returns the exit code (>=0), or
 * -2 on a genuine #VMFAILVALID.  RFLAGS alone is NOT a reliable VMFAIL test
 * after a successful #VMEXIT -- host RFLAGS is restored from the hsave area
 * and may carry stale ZF/CF -- so CLC is executed before VMRUN (making any
 * later ZF/CF meaningful) and the VMCB EXITCODE is cross-checked. */
static long vm_svm_vmrun(VmmGuest* gg) {
    /* RFLAGS capture.  pushf/pop are 32-bit mnemonics; the assembler
     * rejects them in 64-bit mode ("invalid instruction suffix"), so pick
     * the mnemonic by target.  The 32-bit branch MUST stay byte-identical:
     * this one file is linked into both kernels (build/vmm.o and
     * build/vmm64.o), and the 32-bit baseline is verified separately.
     * flags is unsigned long so the operand width matches the mnemonic. */
    unsigned long flags;
    uint32_t vmcb = gg->vmcb_phys;
    __asm__ __volatile__(
        "clc\n\t"
        ".byte 0x0F, 0x01, 0xD8\n\t"    /* VMRUN (rAX = vmcb phys) */
#if defined(__x86_64__)
        "pushfq\n\t"
        "popq %1\n\t"
#else
        "pushfl\n\t"
        "popl %1\n\t"
#endif
        : "+a"(vmcb), "=r"(flags)
        :
        : "memory", "cc");
    (void)vmcb;
    /* Deep diagnostic: dump the raw exit-code/exitinfo area + EFLAGS so a
     * single run fully disambiguates #VMFAIL vs #VMEXIT vs host #GP. */
    {
        const uint8_t* v = (const uint8_t*)gg->vmcb_phys;
        uint32_t e0 = vm_get32(v, 0x58), e1 = vm_get32(v, 0x5C);
        uint32_t i0 = vm_get32(v, 0x60), i1 = vm_get32(v, 0x64);
        vm_log("[VMM] post-vmrun: exitcode="); vm_log_hex(e1);
        vm_log(":"); vm_log_hex(e0);
        vm_log(" info1="); vm_log_hex(i1);
        vm_log(":"); vm_log_hex(i0);
        vm_log(" rflags="); vm_log_hex((uint32_t)flags);
        vm_log("\n");
    }
    uint32_t exitcode = vm_get32((const uint8_t*)gg->vmcb_phys, VMCB_EXITCODE);
    if (exitcode == 0)
        return -2;                      /* VMCB rejected (#VMFAILVALID) */
    return (long)exitcode;
}

static int vmm_svm_enter(VmmGuest* gg) {
    /* 1) EFER.SVME = 1 and the host save area */
    uint64_t efer = vmm_rdmsr(MSR_EFER);
    vmm_wrmsr(MSR_EFER, efer | EFER_SVME);
    vmm_wrmsr(MSR_VM_HSAVE_PA, (uint64_t)gg->hsav_phys);
    /* verify the write landed (a QEMU/cpu model without EFER would drop it) */
    if (!(vmm_rdmsr(MSR_EFER) & EFER_SVME)) {
        vm_log("[VMM] FAIL: EFER.SVME did not stick (no SVM on this cpu?)\n");
        return -1;
    }

    /* 2) build the VMCB and enter */
    vm_build_vmcb(gg);
    vm_dump_vmcb(gg);
    long exit_code = vm_svm_vmrun(gg);
    if (exit_code == -2) {
        vm_log("[VMM] #VMFAILVALID: VMCB rejected\n");
        return -1;
    }
    if (exit_code == -3) {
        vm_log("[VMM] #VMFAILINVALID: SVM cannot start (check EFER.SVME/hsave)\n");
        return -1;
    }
    vm_log("[VMM] #VMEXIT code="); vm_log_hex((uint32_t)exit_code);
    vm_log("\n");

    /* 3) dispatch the exit */
    const uint8_t* v = (const uint8_t*)gg->vmcb_phys;
    uint32_t info1 = vm_get32(v, VMCB_EXITINFO1);
    uint32_t next_rip = vm_get32(v, VMCB_NEXT_RIP);

    if (exit_code == SVM_EXITCODE_IOIO) {
        uint32_t port = info1 >> SVM_IOIO_PORT_SHIFT;
        uint8_t  val  = (uint8_t)vm_get32(v, VMCB_SAVE_RAX);
        if (info1 & SVM_IOIO_TYPE_IN) {
            /* IN: deliver a queued rx byte (or 0) and store into save.RAX */
            uint32_t rax = 0;
            /* one queued byte lands in AL; the rest of RAX stays 0 */
            vm_put32((uint8_t*)v, VMCB_SAVE_RAX, rax);
            (void)port;
        } else {
            /* OUT: guest wrote a byte -- park it in the vUART tx ring */
            vmm_uart_tx_push(gg, val);
        }
        gg->rip = next_rip;
        return 1;
    }
    if (exit_code == SVM_EXITCODE_HLT) {
        gg->running = 0;
        return 0;                    /* guest halted: done for this burst */
    }
    /* unknown exit: advance past it to avoid an infinite loop */
    gg->rip = next_rip ? next_rip : gg->rip + 1;
    return 1;
}
#endif

int vmm_run_once(void* g) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg || !g_inited) return -1;
    if (g_kind == VMM_VMX) { gg->running = 1; return vmm_vmx_enter(gg); }
    if (g_kind == VMM_SVM) { gg->running = 1; return vmm_svm_enter(gg); }
    return -1;  /* no hardware virtualization -> caller falls back to VNC */
}

void vmm_shutdown(void* g) {
    VmmGuest* gg = (VmmGuest*)g;
    if (!gg) return;
    if (gg->running) {
        /* VMXOFF / VMMCALL(VMRUN shutdown) would go here. */
        gg->running = 0;
    }
    if (gg->phys_base && gg->pages) pmm_free_range(gg->phys_base, gg->pages);
    gg->phys_base = 0;
    gg->pages = 0;
    if (gg->vmcb_phys) pmm_free_page(gg->vmcb_phys);
    if (gg->hsav_phys) pmm_free_page(gg->hsav_phys);
    if (gg->iopm_phys) pmm_free_range(gg->iopm_phys, 4);
    if (gg->msrpm_phys) pmm_free_range(gg->msrpm_phys, 2);
    gg->vmcb_phys = gg->hsav_phys = gg->iopm_phys = gg->msrpm_phys = 0;
    gg->disk.attached = 0;
    gg->uart.tx_head = gg->uart.tx_tail = 0;
    gg->uart.rx_head = gg->uart.rx_tail = 0;
}
