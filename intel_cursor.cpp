// =====================================================================
//  intel_cursor.cpp  -  Intel iGPU hardware-cursor backend (32-bit kernel)
// ---------------------------------------------------------------------
//  WHY THIS IS AN AUDIT RATHER THAN A CURSOR PROGRAMMER (yet):
//
//  On Intel the cursor plane's base must hold a GRAPHICS address (a GGTT
//  offset), not a physical one.  Producing one requires knowing where the GTT
//  lives and writing a page-table entry into it, and the GTT base moved
//  between generations and cannot be derived reliably from the outside.  Two
//  failure modes are very different in severity:
//
//    * pointing CURSOR_x_BASE at an unmapped graphics address -> cosmetic
//      (a garbled or missing sprite);
//    * writing a PTE at a GUESSED GTT offset -> corrupts the GPU's address
//      space, which can hang the display or the whole GPU.
//
//  Blind register poking on someone's laptop is not an acceptable trade, so
//  this backend stops at the safe, useful part:
//
//    1. locate the Intel display device and its MMIO BAR;
//    2. refuse to touch MMIO the current paging setup cannot reach (the BIOS
//       path identity-maps only the first 32 MiB of physical memory);
//    3. read and report the display state (active pipe, mode timings, plane
//       and cursor registers) over the serial port;
//    4. return 0, so the GUI keeps painting its software cursor.
//
//  That report pins the generation, the active pipe and the register layout --
//  exactly what is needed to place the cursor buffer in memory the GTT already
//  maps, after which intel_cursor_probe() can return 1 and the GUI will stop
//  drawing its own arrow.
//
//  NOTE: the cursor register offsets below (0x70080 for pipe A, +0x40 for
//  pipe B) have been stable from gen2 through gen12, which is why the audit
//  can read them meaningfully even without a device-ID table.
// =====================================================================
#include "intel_cursor.h"

// ---------------------------------------------------------------------
//  Freestanding primitives
// ---------------------------------------------------------------------
static inline void ic_outl(unsigned short port, unsigned int v) {
    __asm__ __volatile__("outl %0, %1" :: "a"(v), "Nd"(port));
}
static inline unsigned int ic_inl(unsigned short port) {
    unsigned int v; __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline unsigned char ic_inb(unsigned short port) {
    unsigned char v; __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static void ic_putc(char c) { ic_outl(0x3F8, (unsigned int)(unsigned char)c); }
static void ic_puts(const char* s) { while (*s) ic_putc(*s++); }
static void ic_puthex(unsigned int v) {
    const char* h = "0123456789ABCDEF";
    ic_putc('0'); ic_putc('x');
    for (int i = 28; i >= 0; i -= 4) ic_putc(h[(v >> i) & 0xF]);
}
static void ic_putdec(int v) {
    char b[12]; int n = 0;
    if (v < 0) { ic_putc('-'); v = -v; }
    if (v == 0) b[n++] = '0';
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) ic_putc(b[--n]);
}

// ---------------------------------------------------------------------
//  PCI config space
// ---------------------------------------------------------------------
static unsigned int ic_pci_read32(unsigned char bus, unsigned char dev,
                                  unsigned char fn, unsigned char off) {
    unsigned int addr = 0x80000000u | ((unsigned)bus << 16) | ((unsigned)dev << 11)
                      | ((unsigned)fn << 8) | (off & 0xFCu);
    ic_outl(0xCF8, addr);
    return ic_inl(0xCFC);
}

// ---------------------------------------------------------------------
//  Is an arbitrary physical MMIO address reachable right now?
//  The 32-bit BIOS path identity-maps only the first 32 MiB with 4 MiB pages,
//  so the usual ~0xF000_0000 display BAR would #PF.  Under UEFI the firmware's
//  4-level tables identity-map MMIO, so any BAR is fine.
// ---------------------------------------------------------------------
static unsigned long long ic_read_msr(unsigned int msr) {
    unsigned int lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}
static int ic_firmware_maps_mmio(void) {
    unsigned int cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    if (!(cr0 & 0x80000000u)) return 0;                 // paging off: nothing mapped
    return (ic_read_msr(0xC0000080u) & 0x100u) ? 1 : 0; // EFER.LME: long mode -> firmware map
}
static int ic_mmio_reachable(unsigned long long pa) {
    if (ic_firmware_maps_mmio()) return 1;
    return pa + 0x100000ULL <= 0x2000000ULL;            // BIOS: first 32 MiB only
}

// ---------------------------------------------------------------------
//  MMIO register offsets (stable gen2..gen12)
// ---------------------------------------------------------------------
#define R_PIPEACONF   0x70008
#define R_PIPEBCONF   0x71008
#define R_HTOTAL_A    0x60000
#define R_VTOTAL_A    0x6000C
#define R_HTOTAL_B    0x61000
#define R_VTOTAL_B    0x6100C
#define R_DSPACNTR    0x70180
#define R_DSPBCNTR    0x71180
#define R_CURACNTR    0x70080
#define R_CURABASE    0x70084
#define R_CURAPOS     0x70088
#define R_CURASIZE    0x7008C
#define R_CURBCNTR    0x700C0
#define R_CURBBASE    0x700C4
#define R_CURBPOS     0x700C8
#define R_CURBSIZE    0x700CC

static int ic_ready = 0;

// ---------------------------------------------------------------------
//  Audit
// ---------------------------------------------------------------------
int intel_cursor_probe(void) {
    ic_ready = 0;

    unsigned char bus = 0, dev = 0, fn = 0;
    int found = 0;
    for (unsigned char b = 0; b < 1 && !found; b++) {
        for (unsigned char d = 0; d < 32 && !found; d++) {
            for (unsigned char f = 0; f < 8; f++) {
                unsigned int id = ic_pci_read32(b, d, f, 0);
                if ((id & 0xFFFFu) == 0xFFFFu) { if (f == 0) break; else continue; }
                if ((id & 0xFFFFu) != 0x8086u) continue;          // Intel vendor
                unsigned int cc = ic_pci_read32(b, d, f, 0x08);
                if (((cc >> 16) & 0xFF00u) != 0x0300u) continue;  // base/sub class 03 00
                bus = b; dev = d; fn = f; found = 1;
                break;
            }
        }
    }
    if (!found) return 0;                       // not an Intel display: silent (QEMU etc.)

    unsigned int id      = ic_pci_read32(bus, dev, fn, 0x00);
    unsigned int classre = ic_pci_read32(bus, dev, fn, 0x08);
    unsigned int bar0lo  = ic_pci_read32(bus, dev, fn, 0x10);
    unsigned int bar2lo  = ic_pci_read32(bus, dev, fn, 0x18);
    unsigned int rev     = classre & 0xFFu;

    ic_puts("[INTEL] display ");
    ic_puthex(id & 0xFFFFu); ic_putc(':'); ic_puthex((id >> 16) & 0xFFFFu);
    ic_puts(" rev="); ic_putdec((int)rev);
    ic_puts(" at "); ic_putdec(bus); ic_putc(':');
    ic_putdec(dev); ic_putc('.'); ic_putdec(fn); ic_putc('\n');

    // BAR0 is GTTMMADR on gen4+ (MMIO registers at offset 0).
    int is64 = (bar0lo & 0x06u) == 0x04u;
    unsigned long long bar0 = bar0lo & 0xFFFFFFF0u;
    if (is64) {
        unsigned int bar0hi = ic_pci_read32(bus, dev, fn, 0x14);
        bar0 |= ((unsigned long long)bar0hi) << 32;
    }
    ic_puts("[INTEL] BAR0(MMIO)="); ic_puthex((unsigned)(bar0 >> 32));
    ic_puthex((unsigned)(bar0 & 0xFFFFFFFFu));
    ic_puts(is64 ? " (64-bit)\n" : " (32-bit)\n");
    ic_puts("[INTEL] BAR2(ap) ="); ic_puthex(bar2lo & 0xFFFFFFF0u); ic_putc('\n');

    if (!bar0) { ic_puts("[INTEL] BAR0 empty -> no MMIO, keep software cursor\n"); return 0; }
    if (!ic_mmio_reachable(bar0)) {
        ic_puts("[INTEL] MMIO above the 32 MiB identity map (BIOS paging) -> "
                "not touching it, keep software cursor\n");
        return 0;
    }

    volatile unsigned int* mmio = (volatile unsigned int*)(unsigned long)bar0;

    unsigned int pcA = mmio[R_PIPEACONF / 4], pcB = mmio[R_PIPEBCONF / 4];
    unsigned int htA = mmio[R_HTOTAL_A / 4],  vtA = mmio[R_VTOTAL_A / 4];
    unsigned int htB = mmio[R_HTOTAL_B / 4],  vtB = mmio[R_VTOTAL_B / 4];

    // Sanity gate: a live pipe with plausible timings means we really are in
    // the display MMIO window (random MMIO would not decode to two sane modes).
    unsigned int hA = (htA & 0xFFFFu) + 1u, vA = (vtA & 0xFFFFu) + 1u;
    unsigned int hB = (htB & 0xFFFFu) + 1u, vB = (vtB & 0xFFFFu) + 1u;
    int saneA = (hA > 320u && hA < 8192u && vA > 200u && vA < 4096u);
    int saneB = (hB > 320u && hB < 8192u && vB > 200u && vB < 4096u);
    if (!saneA && !saneB) {
        ic_puts("[INTEL] register sanity check failed -> wrong MMIO window, "
                "keep software cursor\n");
        return 0;
    }

    ic_puts("[INTEL] pipe A: conf="); ic_puthex(pcA);
    ic_puts((pcA & 0x80000000u) ? " enabled" : " off");
    ic_puts("  mode="); ic_putdec((int)hA); ic_putc('x'); ic_putdec((int)vA); ic_putc('\n');
    ic_puts("[INTEL] pipe B: conf="); ic_puthex(pcB);
    ic_puts((pcB & 0x80000000u) ? " enabled" : " off");
    ic_puts("  mode="); ic_putdec((int)hB); ic_putc('x'); ic_putdec((int)vB); ic_putc('\n');

    ic_puts("[INTEL] plane A ctl="); ic_puthex(mmio[R_DSPACNTR / 4]);
    ic_puts("  plane B ctl=");       ic_puthex(mmio[R_DSPBCNTR / 4]); ic_putc('\n');
    ic_puts("[INTEL] cursor A ctl="); ic_puthex(mmio[R_CURACNTR / 4]);
    ic_puts(" base="); ic_puthex(mmio[R_CURABASE / 4]);
    ic_puts(" pos=");  ic_puthex(mmio[R_CURAPOS / 4]);
    ic_puts(" size="); ic_puthex(mmio[R_CURASIZE / 4]); ic_putc('\n');
    ic_puts("[INTEL] cursor B ctl="); ic_puthex(mmio[R_CURBCNTR / 4]);
    ic_puts(" base="); ic_puthex(mmio[R_CURBBASE / 4]);
    ic_puts(" pos=");  ic_puthex(mmio[R_CURBPOS / 4]);
    ic_puts(" size="); ic_puthex(mmio[R_CURBSIZE / 4]); ic_putc('\n');

    // The cursor base is a GGTT offset; without a GTT-mapped buffer for our own
    // ARGB image we cannot programme it safely, so we deliberately stop here.
    ic_puts("[INTEL] hardware cursor NOT enabled (needs a GTT-mapped cursor "
            "buffer) -> software cursor stays\n");
    return 0;
}

void intel_cursor_move(int x, int y) {
    (void)x; (void)y;          // no-op until probe() can return 1
}

void intel_cursor_show(int on) {
    (void)on;
}
