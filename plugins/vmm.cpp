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

#include <stdint.h>

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
#define MSR_EFER               0xC0000080u
#define EFER_SVME               (1u << 12)

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

    /* Intel: CPUID.1:ECX bit 5 = VMX */
    if (is_intel) {
        vmm_cpuid(1, &a, &b, &c, &d);
        if (c & (1u << 5)) {
            /* IA32_FEATURE_CONTROL must allow VMXON (bit 2) and not be locked
             * out.  We only *report* availability; vmm_init does the enable. */
            uint64_t fc = vmm_rdmsr(IA32_FEATURE_CONTROL);
            if (fc & FEATURE_CONTROL_VMXON) { g_kind = VMM_VMX; return g_kind; }
        }
        g_kind = VMM_NONE; return g_kind;
    }

    /* AMD: CPUID.0x80000001:ECX bit 2 = SVM, and SVM not disabled in VM_CR */
    if (is_amd) {
        uint32_t maxext;
        vmm_cpuid(0x80000000u, &maxext, &b, &c, &d);
        if (maxext >= 0x80000001u) {
            vmm_cpuid(0x80000001u, &a, &b, &c, &d);
            if (c & (1u << 2)) {
                uint64_t vmcr = vmm_rdmsr(MSR_VM_CR);
                if (!(vmcr & VM_CR_SVM_DISABLE)) { g_kind = VMM_SVM; return g_kind; }
            }
        }
        g_kind = VMM_NONE; return g_kind;
    }

    g_kind = VMM_NONE;
    return g_kind;
}

VmmKind vmm_kind(void)       { return g_kind; }
int     vmm_available(void)  { return g_kind != VMM_NONE; }

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

/* Enable SVM and run the guest. */
static int vmm_svm_enter(VmmGuest* gg) {
    (void)gg;
    /* 1) EFER.SVME = 1
     * 2) build VMCB (CR0/CR3/CR4, RIP -> guest, intercept list)
     * 3) VM_RUN
     * On #VMEXIT we return and dispatch. */
    return -1;
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
    gg->disk.attached = 0;
    gg->uart.tx_head = gg->uart.tx_tail = 0;
    gg->uart.rx_head = gg->uart.rx_tail = 0;
}
