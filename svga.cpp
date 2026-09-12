// =====================================================================
//  svga.cpp  -  VMware SVGA-II 2D accelerator + hardware cursor
// ---------------------------------------------------------------------
//  The first real GPU driver in NexOS.  Why SVGA-II first:
//    * it is the only accelerated device QEMU offers that keeps a plain
//      linear framebuffer, so the whole existing GOP/VBE draw path keeps
//      working and this driver is purely additive;
//    * its register interface is I/O-port based (BAR0), so nothing has to
//      be mapped into the 32-bit kernel's address space to talk to it;
//    * it has a documented FIFO with 2D commands AND a real hardware
//      cursor with an ARGB image and a hotspot -- which is the piece the
//      CURSOR_HW backend has been waiting for.
//
//  Design rules followed here:
//    * The display mode is NOT set: the firmware already did that and the
//      compositor draws into the linear framebuffer.  This driver only
//      accelerates and provides the cursor plane.
//    * Everything is fail-safe.  Each step is verified before the next one
//      is allowed to rely on it, and any failure returns 0 so the GUI falls
//      back to its painted software cursor.  A machine without the device,
//      or with a device that answers unexpectedly, loses only speed -- never
//      the pointer.
//    * Where a choice could be wrong in a way that is invisible in the
//      source (e.g. whether the device takes a guest-RAM FIFO or its own
//      BAR), the driver measures it and logs what it saw rather than
//      assuming.
// =====================================================================
#include "svga.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

// ---------------------------------------------------------------------
//  Port I/O
// ---------------------------------------------------------------------
static inline void sv_outb(u16 port, u8 v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline void sv_outl(u16 port, u32 v) {
    __asm__ __volatile__("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline u32 sv_inl(u16 port) {
    u32 v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline u8 sv_inb(u16 port) {
    u8 v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

// ---------------------------------------------------------------------
//  Serial logging (self-contained; this file must not depend on libc)
// ---------------------------------------------------------------------
static void sv_putc(char c) {
    int guard = 0;
    while (!(sv_inb(0x3FD) & 0x20) && ++guard < 1000000) { }
    sv_outb(0x3F8, (u8)c);
}
static void sv_puts(const char* s) { while (*s) sv_putc(*s++); }
static void sv_puthex(u32 v) {
    const char* h = "0123456789ABCDEF";
    sv_putc('0'); sv_putc('x');
    for (int i = 28; i >= 0; i -= 4) sv_putc(h[(v >> i) & 0xF]);
}
static void sv_putdec(int v) {
    char b[12]; int n = 0;
    if (v < 0) { sv_putc('-'); v = -v; }
    if (v == 0) b[n++] = '0';
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) sv_putc(b[--n]);
}

// ---------------------------------------------------------------------
//  PCI configuration space
// ---------------------------------------------------------------------
static u32 sv_pci_read32(u8 bus, u8 dev, u8 fn, u8 off) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)fn << 8) | (off & 0xFCu);
    sv_outl(0xCF8, addr);
    return sv_inl(0xCFC);
}
static void sv_pci_write32(u8 bus, u8 dev, u8 fn, u8 off, u32 val) {
    u32 addr = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11)
             | ((u32)fn << 8) | (off & 0xFCu);
    sv_outl(0xCF8, addr);
    sv_outl(0xCFC, val);
}

#define SVGA_VENDOR_VMWARE 0x15ADu
#define SVGA_DEVICE_II     0x0405u

// ---------------------------------------------------------------------
//  Register indices (SVGA-II)
// ---------------------------------------------------------------------
#define R_ID              0
#define R_ENABLE          1
#define R_WIDTH           2
#define R_HEIGHT          3
#define R_MAX_WIDTH       4
#define R_MAX_HEIGHT      5
#define R_DEPTH           6
#define R_BITS_PER_PIXEL  7
#define R_BYTES_PER_LINE  12
#define R_FB_START        13
#define R_FB_OFFSET       14
#define R_VRAM_SIZE       15
#define R_FB_SIZE         16
#define R_CAPABILITIES    17
#define R_MEM_START       18
#define R_MEM_SIZE        19
#define R_CONFIG_DONE     20
#define R_SYNC            21
#define R_BUSY            22
#define R_CURSOR_ID       24
#define R_CURSOR_X        25
#define R_CURSOR_Y        26

#define SVGA_ID_2         0x00B00000u   // 0x900000 | (2 << 20)

// FIFO registers, as dword indices into the FIFO block
#define F_MIN                 0
#define F_MAX                 1
#define F_NEXT_CMD            2
#define F_STOP                3
#define F_CURSOR_ON           9
#define F_CURSOR_X            10
#define F_CURSOR_Y            11
#define F_CURSOR_COUNT        12
#define F_CURSOR_LAST_UPDATED 13
#define F_NUM_REGS            16

// Commands
#define C_UPDATE               1
#define C_RECT_FILL            2
#define C_RECT_COPY            3
#define C_DISPLAY_CURSOR       20
#define C_MOVE_CURSOR          21
#define C_DEFINE_ALPHA_CURSOR  22

// ---------------------------------------------------------------------
//  Driver state
// ---------------------------------------------------------------------
static u16  sv_io        = 0;      // BAR0 I/O port base
static int  sv_ok        = 0;      // 1 once the FIFO is usable
static int  sv_probed    = 0;

static u8*  sv_fifo_base = 0;      // FIFO block, byte-addressed
static u32  sv_fifo_size = 0;      // bytes
static u32  sv_fifo_min  = 0;      // first usable byte offset
static u32  sv_fifo_max  = 0;      // end of the ring
static u32  sv_next      = 0;      // byte offset of the next command

static u32  sv_fb_off    = 0;
static u32  sv_fb_size   = 0;
static u32  sv_vram      = 0;
static u32  sv_bpp       = 0;

// The mode the kernel is actually drawing, supplied by the caller before
// init.  See svga_mode_hint() in the header for why this matters.
static int  sv_hint_w    = 0;
static int  sv_hint_h    = 0;
static int  sv_hint_bpp  = 0;
static int  sv_hint_pitch = 0;

void svga_mode_hint(int w, int h, int bpp, int pitch) {
    sv_hint_w = w; sv_hint_h = h; sv_hint_bpp = bpp; sv_hint_pitch = pitch;
}

// See svga_allow_enable() in the header.  Off by default: the driver's job
// right now is to tell us what the hardware is, not to disturb a display
// whose format we do not yet agree with.
static int sv_allow_enable = 0;
void svga_allow_enable(int on) { sv_allow_enable = on; }

// The kernel heap lives in the first 32 MiB and is identity-mapped, so a
// kmalloc'd address is also its physical address -- which is what the device
// needs for SVGA_REG_MEM_START.
extern "C" void* kmalloc(u32 size);

// ---------------------------------------------------------------------
//  Register access.  Both the index and the value port are 4-byte
//  registers; narrower accesses are not decoded by the device.
// ---------------------------------------------------------------------
static u32 sv_reg_read(u32 idx) {
    sv_outl((u16)(sv_io + 0), idx);
    return sv_inl((u16)(sv_io + 1));
}
static void sv_reg_write(u32 idx, u32 val) {
    sv_outl((u16)(sv_io + 0), idx);
    sv_outl((u16)(sv_io + 1), val);
}

// ---------------------------------------------------------------------
//  FIFO helpers
// ---------------------------------------------------------------------
static inline volatile u32* sv_fifo_reg(u32 dword_index) {
    return (volatile u32*)(sv_fifo_base + dword_index * 4);
}

// Bytes still free in the ring before we would run into the device's
// read pointer (STOP).
static u32 sv_fifo_space(void) {
    u32 stop = *sv_fifo_reg(F_STOP);
    if (stop < sv_fifo_min || stop > sv_fifo_max) stop = sv_fifo_max;
    if (sv_next >= stop) return stop - sv_fifo_min;
    return stop - sv_next;
}

static void sv_fifo_put(u32 v) {
    *(volatile u32*)(sv_fifo_base + sv_next) = v;
    sv_next += 4;
    if (sv_next >= sv_fifo_max) sv_next = sv_fifo_min;
}

// Publish the commands and wait for the device to drain them.
static void sv_fifo_commit(void) {
    *sv_fifo_reg(F_NEXT_CMD) = sv_next;
    sv_reg_write(R_SYNC, 1);
    int guard = 0;
    while (sv_reg_read(R_BUSY) != 0 && ++guard < 2000000) { }
}

// ---------------------------------------------------------------------
//  Cursor image: a classic arrow with an outline, blended by the device.
//  Generated procedurally so the driver carries no bitmap blob.
// ---------------------------------------------------------------------
#define CUR_W 32
#define CUR_H 32
#define CUR_HOT_X 1
#define CUR_HOT_Y 1
static u32 sv_cursor_img[CUR_W * CUR_H];

// 12 columns of the arrow, 19 rows tall.
static const u16 sv_arrow[CUR_H] = {
    0x1000, 0x1800, 0x1C00, 0x1E00, 0x1F00, 0x1F80, 0x1FC0, 0x1FE0,
    0x1FF0, 0x1FF8, 0x1F00, 0x1B00, 0x1980, 0x0980, 0x08C0, 0x00C0,
    0x0040, 0x0040, 0x0000
};

static void sv_cursor_build(void) {
    for (int i = 0; i < CUR_W * CUR_H; i++) sv_cursor_img[i] = 0;

    // Outline: one opaque black pixel around every filled pixel.
    for (int y = 0; y < CUR_H; y++) {
        u16 row = sv_arrow[y];
        if (!row) continue;
        for (int x = 0; x < 12; x++) {
            if (!(row & (0x8000u >> x))) continue;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int ox = x + dx, oy = y + dy;
                    if (ox < 0 || oy < 0 || ox >= CUR_W || oy >= CUR_H) continue;
                    sv_cursor_img[oy * CUR_W + ox] = 0xFF000000u;
                }
            }
        }
    }
    // Fill: opaque white over the outline.
    for (int y = 0; y < CUR_H; y++) {
        u16 row = sv_arrow[y];
        if (!row) continue;
        for (int x = 0; x < 12; x++) {
            if (!(row & (0x8000u >> x))) continue;
            sv_cursor_img[y * CUR_W + x] = 0xFFFFFFFFu;
        }
    }
}

// ---------------------------------------------------------------------
//  Probe
// ---------------------------------------------------------------------
static int sv_pci_find(void) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            u32 id = sv_pci_read32((u8)bus, (u8)dev, 0, 0);
            if ((id & 0xFFFFu) == 0xFFFFu) continue;
            if ((id & 0xFFFFu) != SVGA_VENDOR_VMWARE) continue;
            if ((id >> 16)       != SVGA_DEVICE_II)      continue;

            // BAR0 is the I/O register window.
            u32 bar0 = sv_pci_read32((u8)bus, (u8)dev, 0, 0x10);
            if ((bar0 & 0x1u) == 0) {
                sv_puts("[SVGA] BAR0 is not an I/O BAR; refusing\n");
                return 0;
            }
            sv_io = (u16)(bar0 & 0xFFFCu);

            // Enable I/O decoding.  Firmware usually does this already, but
            // doing it ourselves removes a whole class of "the device never
            // answers" failures.
            u32 cmd = sv_pci_read32((u8)bus, (u8)dev, 0, 0x04);
            if (!(cmd & 0x1u)) sv_pci_write32((u8)bus, (u8)dev, 0, 0x04, cmd | 0x1u);

            sv_puts("[SVGA] found SVGA-II at ");
            sv_putdec(bus); sv_putc(':'); sv_putdec(dev);
            sv_puts(" iobase="); sv_puthex(sv_io); sv_putc('\n');
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------
//  Device bring-up: negotiate the revision and read the display facts.
// ---------------------------------------------------------------------
static int sv_device_init(void) {
    u32 id = sv_reg_read(R_ID);
    sv_puts("[SVGA] device id="); sv_puthex(id);
    sv_reg_write(R_ID, SVGA_ID_2);
    u32 id2 = sv_reg_read(R_ID);
    sv_puts(" negotiated="); sv_puthex(id2); sv_putc('\n');
    if (id2 < SVGA_ID_2) {
        sv_puts("[SVGA] device predates SVGA-II; stopping\n");
        return 0;
    }

    sv_reg_write(R_ENABLE, 0);
    sv_reg_write(R_CONFIG_DONE, 0);

    sv_vram    = sv_reg_read(R_VRAM_SIZE);
    sv_fb_off  = sv_reg_read(R_FB_OFFSET);
    sv_fb_size = sv_reg_read(R_FB_SIZE);
    sv_bpp     = sv_reg_read(R_BITS_PER_PIXEL);

    sv_puts("[SVGA] fb_start="); sv_puthex(sv_reg_read(R_FB_START));
    sv_puts(" vram=");  sv_puthex(sv_vram);
    sv_puts(" fb_off=");      sv_puthex(sv_fb_off);
    sv_puts(" fb_size=");     sv_puthex(sv_fb_size);
    sv_puts(" bpp=");         sv_putdec((int)sv_bpp);
    sv_puts("\n[SVGA] mode ");
    sv_putdec((int)sv_reg_read(R_WIDTH));  sv_putc('x');
    sv_putdec((int)sv_reg_read(R_HEIGHT));
    sv_puts(" max ");
    sv_putdec((int)sv_reg_read(R_MAX_WIDTH)); sv_putc('x');
    sv_putdec((int)sv_reg_read(R_MAX_HEIGHT));
    sv_puts(" pitch="); sv_putdec((int)sv_reg_read(R_BYTES_PER_LINE));
    sv_puts(" caps=");  sv_puthex(sv_reg_read(R_CAPABILITIES));
    sv_putc('\n');
    return 1;
}

// ---------------------------------------------------------------------
//  FIFO bring-up
//
//  Where the FIFO lives is implementation-defined and the two cases need
//  completely different handling, so the driver asks rather than assumes:
//
//    * guest RAM -- the heap is identity-mapped and far below the device's
//      32-bit DMA limit, so a kmalloc'd pointer is also the physical address
//      the device needs.  Cheapest, and no page tables are touched.
//    * device-owned MMIO -- QEMU puts the FIFO in BAR2, up near 4 GiB.  The
//      32-bit kernel only identity-maps the first 32 MiB, so that region has
//      to be mapped into a virtual window before a single byte of it can be
//      read.  Writing there unmapped would page-fault.
//
//  So: offer guest RAM, read back what the device actually stored, and only
//  fall back to mapping if it kept an address of its own.  If neither works
//  the driver stops -- an unrecognised layout is exactly the situation where
//  proceeding blind could scribble over live memory.
// ---------------------------------------------------------------------
extern "C" int vmm_map_page(unsigned int virt, unsigned int phys, unsigned int flags);

// Scratch window for the FIFO mapping.  Deliberately below the kernel's
// 0xF0000000 high-framebuffer window so the two can never collide.
#define SV_MMIO_WIN 0xE0000000u

static u8* sv_map_mmio(u32 phys, u32 size) {
    if (phys & 0xFFFu) return 0;                       // must be page aligned
    u32 pages = (size + 4095u) >> 12;
    if (pages == 0 || pages > 4096u) return 0;         // sanity: <= 16 MiB
    for (u32 i = 0; i < pages; i++) {
        if (!vmm_map_page(SV_MMIO_WIN + i * 4096u, phys + i * 4096u, 0x3u))
            return 0;                                  // present | rw
    }
    return (u8*)SV_MMIO_WIN;
}
static int sv_fifo_init(void) {
    u32 want0     = sv_reg_read(R_MEM_SIZE);   // the device's stated FIFO size
    u32 dev_start = sv_reg_read(R_MEM_START);
    if (want0 < 4096u)   want0 = 4096u;
    if (want0 > (1u << 22)) want0 = (1u << 22);
    sv_puts("[SVGA] device mem_size="); sv_putdec((int)want0);
    sv_puts(" mem_start=");             sv_puthex(dev_start); sv_putc('\n');

    // --- attempt 1: FIFO in guest RAM (no page tables touched) -----------
    u32 alloc = want0;
    if (alloc > (1u << 20)) alloc = (1u << 20);        // cap at 1 MiB
    void* mem = kmalloc(alloc);
    if (mem) {
        u32 phys = (u32)(unsigned long)mem;
        if (phys + alloc <= 0x02000000u) {
            sv_reg_write(R_MEM_START, phys);
            sv_reg_write(R_MEM_SIZE,  alloc);
            u32 rb = sv_reg_read(R_MEM_START);
            if (rb == phys) {
                sv_fifo_base = (u8*)mem;
                sv_fifo_size = sv_reg_read(R_MEM_SIZE);
                if (sv_fifo_size < 4096u) sv_fifo_size = 4096u;
                sv_puts("[SVGA] fifo in guest RAM at "); sv_puthex(phys); sv_putc('\n');
            } else {
                dev_start = rb;                        // the device kept its own
            }
        }
    }

    // --- attempt 2: the device owns the FIFO address, so map its MMIO -----
    if (!sv_fifo_base) {
        if (dev_start == 0) {
            sv_puts("[SVGA] no usable FIFO address; stopping\n");
            return 0;
        }
        u8* win = sv_map_mmio(dev_start, want0);
        if (!win) {
            sv_puts("[SVGA] could not map the device FIFO at ");
            sv_puthex(dev_start); sv_puts("; stopping\n");
            return 0;
        }
        sv_fifo_base = win;
        sv_fifo_size = want0;
        sv_puts("[SVGA] fifo MMIO phys="); sv_puthex(dev_start);
        sv_puts(" -> va=");  sv_puthex(SV_MMIO_WIN);
        sv_puts(" size=");   sv_putdec((int)sv_fifo_size); sv_putc('\n');
    }

    // Initialise the ring: MIN starts after the register block and
    // NEXT_CMD/STOP must sit inside [MIN, MAX).
    sv_fifo_min = F_NUM_REGS * 4;
    sv_fifo_max = sv_fifo_size;
    if (sv_fifo_max <= sv_fifo_min + 64) {
        sv_puts("[SVGA] FIFO too small; stopping\n");
        return 0;
    }
    *sv_fifo_reg(F_MIN)      = sv_fifo_min;
    *sv_fifo_reg(F_MAX)      = sv_fifo_max;
    *sv_fifo_reg(F_NEXT_CMD) = sv_fifo_min;
    *sv_fifo_reg(F_STOP)     = sv_fifo_min;
    sv_next = sv_fifo_min;

    if (!sv_allow_enable) {
        // Everything above is read-only or harmless register pokes; enabling
        // is the step that takes the scanout away from the firmware mode.
        // Stop here and report, leaving the display exactly as it was.
        sv_puts("[SVGA] audit complete: device NOT enabled (display untouched);\n");
        sv_puts("[SVGA]   call svga_allow_enable(1) once the pixel format is agreed\n");
        return 0;
    }

    // Hand the device the mode the rest of the kernel is drawing.
    //
    // While ENABLE=1 the SVGA device owns the scanout and takes the surface
    // size from its OWN width/height registers.  Those power up at 640x480
    // here, while the firmware had set a 1280x720 VBE mode -- so enabling
    // without fixing them up leaves the host presenting a 0x0 surface: the
    // guest keeps drawing into the linear framebuffer and nothing reaches
    // the screen (measured: a screendump comes back as "P6 0 0 255", i.e.
    // 11 bytes).  Programming the mode first keeps the two consistent.
    if (sv_hint_w > 0 && sv_hint_h > 0) {
        sv_reg_write(R_WIDTH,  (u32)sv_hint_w);
        sv_reg_write(R_HEIGHT, (u32)sv_hint_h);
        if (sv_hint_bpp == 16 || sv_hint_bpp == 24 || sv_hint_bpp == 32)
            sv_reg_write(R_BITS_PER_PIXEL, (u32)sv_hint_bpp);
        if (sv_hint_pitch > 0)
            sv_reg_write(R_BYTES_PER_LINE, (u32)sv_hint_pitch);

        sv_puts("[SVGA] mode programmed ");
        sv_putdec(sv_hint_w); sv_putc('x'); sv_putdec(sv_hint_h);
        sv_puts(" bpp=");   sv_putdec(sv_hint_bpp);
        sv_puts(" pitch="); sv_putdec(sv_hint_pitch);
        sv_puts(" (readback ");
        sv_putdec((int)sv_reg_read(R_WIDTH)); sv_putc('x');
        sv_putdec((int)sv_reg_read(R_HEIGHT));
        sv_puts(" bpp="); sv_putdec((int)sv_reg_read(R_BITS_PER_PIXEL));
        sv_puts(")\n");
    } else {
        sv_puts("[SVGA] no mode hint; leaving the device mode untouched\n");
    }

    sv_reg_write(R_CONFIG_DONE, 1);
    sv_reg_write(R_ENABLE, 1);
    return 1;
}

int svga_init(void) {
    if (sv_probed) return sv_ok;
    sv_probed = 1;
    sv_ok = 0;

    if (!sv_pci_find())     return 0;
    if (!sv_device_init())  return 0;
    if (!sv_fifo_init())    return 0;

    sv_ok = 1;
    sv_puts("[SVGA] 2D acceleration + hardware cursor ready\n");
    return 1;
}

int svga_ready(void) { return sv_ok; }

// ---------------------------------------------------------------------
//  Commands
// ---------------------------------------------------------------------
void svga_fill_rect(int x, int y, int w, int h, unsigned int packed_color) {
    if (!sv_ok || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (sv_fifo_space() < 6 * 4 + 64) sv_fifo_commit();

    sv_fifo_put(C_RECT_FILL);
    sv_fifo_put(packed_color);
    sv_fifo_put((u32)x);
    sv_fifo_put((u32)y);
    sv_fifo_put((u32)w);
    sv_fifo_put((u32)h);
    sv_fifo_commit();
}

void svga_update(int x, int y, int w, int h) {
    if (!sv_ok || w <= 0 || h <= 0) return;
    if (sv_fifo_space() < 5 * 4 + 64) sv_fifo_commit();

    sv_fifo_put(C_UPDATE);
    sv_fifo_put((u32)x);
    sv_fifo_put((u32)y);
    sv_fifo_put((u32)w);
    sv_fifo_put((u32)h);
    sv_fifo_commit();
}

int svga_cursor_define(int w, int h, int hx, int hy, const unsigned int* argb) {
    if (!sv_ok) return 0;
    if (w <= 0 || h <= 0 || w > 64 || h > 64 || !argb) return 0;
    u32 n = (u32)w * (u32)h;
    if (sv_fifo_space() < (6 + n + 8) * 4 + 64) sv_fifo_commit();

    sv_fifo_put(C_DEFINE_ALPHA_CURSOR);
    sv_fifo_put(1);                     // cursor id
    sv_fifo_put((u32)hx);
    sv_fifo_put((u32)hy);
    sv_fifo_put((u32)w);
    sv_fifo_put((u32)h);
    for (u32 i = 0; i < n; i++) sv_fifo_put(argb[i]);
    sv_fifo_commit();
    return 1;
}

void svga_cursor_move(int x, int y) {
    if (!sv_ok) return;
    if (x < -5000) x = -5000;
    if (y < -5000) y = -5000;
    if (sv_fifo_space() < 3 * 4 + 64) sv_fifo_commit();

    sv_fifo_put(C_MOVE_CURSOR);
    sv_fifo_put((u32)x);
    sv_fifo_put((u32)y);
    sv_fifo_commit();
}

void svga_cursor_show(int on) {
    if (!sv_ok) return;
    if (sv_fifo_space() < 3 * 4 + 64) sv_fifo_commit();

    sv_fifo_put(C_DISPLAY_CURSOR);
    sv_fifo_put(1);                     // cursor id
    sv_fifo_put(on ? 1u : 0u);
    sv_fifo_commit();
}

// ---------------------------------------------------------------------
//  Self-verification.  Run once after bring-up so the serial log says
//  whether the device actually processed the cursor commands: if it did,
//  it mirrors the pointer into SVGA_FIFO_CURSOR_X/Y and moves
//  SVGA_FIFO_CURSOR_COUNT, so reading those back is real evidence rather
//  than a claim.
// ---------------------------------------------------------------------
int svga_cursor_selftest(void) {
    if (!sv_ok) return 0;

    sv_cursor_build();
    svga_cursor_define(CUR_W, CUR_H, CUR_HOT_X, CUR_HOT_Y, sv_cursor_img);

    u32 count_before = *sv_fifo_reg(F_CURSOR_COUNT);
    svga_cursor_move(320, 240);
    svga_cursor_show(1);
    u32 count_after = *sv_fifo_reg(F_CURSOR_COUNT);
    u32 back_x = *sv_fifo_reg(F_CURSOR_X);
    u32 back_y = *sv_fifo_reg(F_CURSOR_Y);

    // Two independent signs that the device consumed the commands: it bumps
    // its cursor counter, and it mirrors the position we asked for.
    int acked = (count_after != count_before) || (back_x == 320u && back_y == 240u);

    sv_puts("[SVGA] cursor selftest: count ");
    sv_putdec((int)count_before); sv_puts(" -> "); sv_putdec((int)count_after);
    sv_puts(" devpos "); sv_putdec((int)back_x);
    sv_putc(',');        sv_putdec((int)back_y);
    sv_puts(acked ? "  ACCEPTED\n" : "  NO-ACK (caller must keep the painted arrow)\n");
    return acked;
}
