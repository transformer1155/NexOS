// =====================================================================
//  boot32_min.cpp  -  Minimal 32-bit bootstrap kernel (loader only)
// ---------------------------------------------------------------------
//  ARCHITECTURE NOTE (2026-11-11, user directive):
//  The 64-bit kernel (kernel64.cpp) is now THE operating system: it owns
//  the GUI desktop, MiniCLR/managed apps, the file systems (SFS/MKFS),
//  networking, distnet and the GGUF AI engine (gguf_infer64/gguf_loader64).
//
//  This 32-bit image is no longer a kernel.  Its only jobs are:
//    1. run with the VBE mode + info block stage2.asm left at 0x5000,
//    2. stage kernel64.bin from disk (or the UEFI handoff at 0x7000),
//    3. verify it really landed (magic check, retries),
//    4. call switch_to_64bit() -- which never returns.
//
//  Everything else the old 32-bit kernel did (GUI/CLR/Shell/FS/net/AI)
//  was removed from the build: kernel.elf links only entry.o +
//  switch32to64.o + this file.  That also frees kernel.bin from the
//  EBDA/576-KiB ceiling that the old monolith kept bumping into.
//
//  Staging buffer: fixed physical address 0x400000 (4 MiB).  Rationale:
//    - the blit destination window is 0x100000..0x300000 (2 MiB image),
//      so anything at >= 0x400000 is never overwritten by the blit;
//    - the long-mode page tables live in linker.ld's .lmboot region
//      (>= 0x1800000), also clear of the staging buffer;
//    - the old kernel's heap (kmalloc) was at 0x10000000 -- far above
//      what a freestanding 32-bit loader wants to touch.
// =====================================================================

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t uintptr_t;

// ---- disk / image layout constants (keep in sync with the Makefile) ----
#define KERNEL64_LBA_DISK   2048
#define KERNEL64_SECTORS    4096          // 2 MiB window at LBA 2048..6144
#define K64_STAGING_ADDR    0x00400000u   // 4 MiB physical
#define K64_MAGIC           0x03F8BA66u   // first dword of kernel64.bin

// ---------------------------------------------------------------------
//  Port I/O + serial
// ---------------------------------------------------------------------
static inline uint8_t  inb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p){ uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }

static void serial_init(){
    outb(0x3F8+1, 0x00);   // disable interrupts
    outb(0x3F8+3, 0x80);   // DLAB on
    outb(0x3F8+0, 0x01);   // 115200
    outb(0x3F8+1, 0x00);
    outb(0x3F8+3, 0x03);   // 8N1
    outb(0x3F8+2, 0xC7);
    outb(0x3F8+4, 0x0B);
}
static void serial_putc(char c){
    for (int i = 0; i < 100000; i++){ if (inb(0x3FD) & 0x20) break; }
    outb(0x3F8, (uint8_t)c);
}
static void serial_puts(const char* s){ while (*s) serial_putc(*s++); }
static void serial_hex(uint32_t v){
    static const char* d = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 28; i >= 0; i -= 4) serial_putc(d[(v >> i) & 0xF]);
}
static void serial_puts_dec(uint32_t v){
    char b[11]; int n = 0;
    if (!v){ serial_putc('0'); return; }
    while (v){ b[n++] = '0' + (v % 10); v /= 10; }
    while (n) serial_putc(b[--n]);
}

// Red-on-black error banner straight into VGA text memory: the serial log is
// the primary channel, but on real hardware a visible message helps too.
static void fail_screen(const char* msg){
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x4F20;      // red bg, spaces
    const char* l1 = "NexOS bootstrap: cannot start the 64-bit kernel";
    for (int i = 0; l1[i]; i++) vga[80 + i] = 0x4F00 | (uint8_t)l1[i];
    for (int i = 0; msg[i] && i < 80; i++) vga[160 + i] = 0x4F00 | (uint8_t)msg[i];
}

// ---------------------------------------------------------------------
//  ATA PIO read (primary master, LBA28, with SRST retry) -- the exact
//  semantics of the old kernel's ata_read_sector(): return false leaves
//  the buffer untouched.
// ---------------------------------------------------------------------
static bool ata_wait_bsy(){
    // Generous timeout: under tcg a tight inb poll loop can outpace the QEMU
    // main loop that completes the backing blk_aio for the next PIO sector,
    // so a "short" spin of 100k iterations sometimes times out just before
    // DRQ rises (observed: status 0x58/0xD0 at diagnostic time, no ERR).
    for (int i = 0; i < 10000000; i++){ if (!(inb(0x1F0 + 7) & 0x80)) return true; }
    return false;
}
static bool ata_wait_drq(){
    for (int i = 0; i < 10000000; i++){ if (inb(0x1F0 + 7) & 0x08) return true; }
    return false;
}
static void ata_soft_reset(){
    outb(0x3F6, 0x04);
    for (volatile int i = 0; i < 20000; i++) {}
    outb(0x3F6, 0x00);
    for (volatile int i = 0; i < 500000; i++) {}
}
static bool ata_read_sector(uint32_t lba, uint16_t* buf){
    serial_putc('[');
    for (int attempt = 0; attempt < 3; attempt++){
        if (attempt) ata_soft_reset();
        if (!ata_wait_bsy()) { serial_putc('w'); continue; }
        serial_putc('b');
        outb(0x1F0 + 6, 0xE0 | ((lba >> 24) & 0x0F));
        outb(0x1F0 + 1, 0x00);
        outb(0x1F0 + 2, 1);
        outb(0x1F0 + 3, lba & 0xFF);
        outb(0x1F0 + 4, (lba >> 8) & 0xFF);
        outb(0x1F0 + 5, (lba >> 16) & 0xFF);
        outb(0x1F0 + 7, 0x20);                       // READ SECTORS
        serial_putc('c');
        if (!ata_wait_bsy()) { serial_putc('W'); continue; }
        serial_putc('B');
        if (!ata_wait_drq()) { serial_putc('d'); continue; }
        serial_putc('D');
        if (inb(0x1F0 + 7) & 0x01) { serial_putc('E'); continue; }   // ERR
        for (int i = 0; i < 256; i++) buf[i] = inw(0x1F0);
        serial_putc('R');
        // Drain the completion interrupt: read the alternate status once so
        // QEMU's IDE model sees the IRQ serviced before the next command.
        inb(0x3F6);
        return true;
    }
    return false;
}

// Multi-sector PIO read: one READ SECTORS command for up to 256 sectors,
// then poll DRQ per 512-byte chunk.  This cuts the 2 MiB staging read from
// 4096 commands to 16, which sidesteps the wedged-channel failure seen with
// back-to-back single-sector commands under the VM's QEMU (BSY stuck after
// a few dozen commands, drifting LBA, soft reset not recovering).
static bool ata_read_multi(uint32_t lba, uint16_t count, uint16_t* buf){
    if (count == 0 || count > 256) return false;
    for (int attempt = 0; attempt < 3; attempt++){
        if (attempt) ata_soft_reset();
        if (!ata_wait_bsy()) continue;
        outb(0x1F0 + 6, 0xE0 | ((lba >> 24) & 0x0F));
        outb(0x1F0 + 1, 0x00);
        outb(0x1F0 + 2, (uint8_t)count);
        outb(0x1F0 + 3, lba & 0xFF);
        outb(0x1F0 + 4, (lba >> 8) & 0xFF);
        outb(0x1F0 + 5, (lba >> 16) & 0xFF);
        outb(0x1F0 + 7, 0x20);                       // READ SECTORS
        bool ok = true;
        for (uint16_t s = 0; s < count && ok; s++){
            if (!ata_wait_bsy()) ok = false;
            else if (!ata_wait_drq()) ok = false;
            else if (inb(0x1F0 + 7) & 0x01) ok = false;
            else {
                uint16_t* p = buf + (uint32_t)s * 256;
                for (int i = 0; i < 256; i++) p[i] = inw(0x1F0);
            }
            if (!ok){
                // diagnostics: where and why the burst stalled
                serial_puts("\n[K32] burst stall: sector #");
                serial_puts_dec(s);
                serial_puts(" status=");
                serial_hex(inb(0x1F0 + 7));
                serial_puts(" err=");
                serial_hex(inb(0x1F0 + 1));
                serial_puts("\n");
            }
        }
        inb(0x3F6);                                  // drain IRQ
        if (ok) return true;
    }
    return false;
}

// Mirror the old kernel's ata_find_hdd() pre-read sequence: controller
// presence check + IDENTIFY DEVICE with a full 256-word drain.  The old
// monolith ran this before EVERY bulk read session and never wedged; skipping
// it leaves the channel in whatever state stage2's BIOS hand-off left, and
// bursts then stall at random sectors (status 0x58/0xD0, no ERR).
static bool ata_prepare_channel(){
    uint16_t base = 0x1F0;
    if (!ata_wait_bsy()) return false;
    // controller presence check
    outb(base + 2, 0x55);
    if (inb(base + 2) != 0x55) return false;
    outb(base + 2, 0xAA);
    if (inb(base + 2) != 0xAA) return false;
    // IDENTIFY DEVICE (primary master)
    outb(base + 6, 0xE0);
    outb(base + 1, 0);
    outb(base + 2, 0);
    outb(base + 3, 0);
    outb(base + 4, 0);
    outb(base + 5, 0);
    outb(base + 7, 0xEC);
    for (int j = 0; j < 100000; j++){
        uint8_t st = inb(base + 7);
        if (st & 0x01) return false;          // ERR / ATAPI
        if (st & 0x08){                        // DRQ: drain the full block
            for (int k = 0; k < 256; k++) inw(base);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
//  CPUID
// ---------------------------------------------------------------------
static void cpuid(uint32_t leaf, uint32_t* a, uint32_t* d){
    __asm__ __volatile__("cpuid" : "=a"(*a), "=d"(*d) : "0"(leaf) : "ebx", "ecx");
}

// ---------------------------------------------------------------------
//  UEFI handoff (BOOTX64.EFI preloads kernel64 and leaves a struct)
// ---------------------------------------------------------------------
struct k64_handoff { uint32_t magic, phys, size; };
#define K64_HANDOFF_MAGIC 0x4B36344EUL   // "N46K"

extern "C" void switch_to_64bit(uint32_t stage_phys);   // .attic64/switch32to64.asm

extern "C" void kmain(){
    // Kill all maskable interrupts before touching the ATA channel.  The old
    // monolithic kernel ran "with interrupts disabled" and polled devices; a
    // stray IRQ14 here would vector through whatever IDT stage2 left behind.
    __asm__ __volatile__("cli");
    outb(0x20, 0x11); outb(0xA0, 0x11);          // PIC init (ICW1, cascade)
    outb(0x21, 0x20); outb(0xA1, 0x28);          // ICW2: remap vectors
    outb(0x21, 0x04); outb(0xA1, 0x02);          // ICW3
    outb(0x21, 0x01); outb(0xA1, 0x01);          // ICW4: 8086 mode
    outb(0x21, 0xFF); outb(0xA1, 0xFF);          // OCW1: mask every IRQ line
    outb(0x20, 0x20); outb(0xA0, 0x20);          // clear any pending EOI state

    serial_init();
    serial_puts("\n[K32] minimal bootstrap: staging kernel64...\n");

    // ---- long-mode availability gate (mirror do_switch64) ------------
    {
        uint32_t a = 0, d = 0;
        cpuid(0x80000000u, &a, &d);
        if (a < 0x80000001u) {
            serial_puts("[K32] no extended CPUID -- no long mode\n");
            fail_screen("CPU has no long mode (CPUID.80000001 missing)");
            for (;;) __asm__ __volatile__("hlt");
        }
        cpuid(0x80000001u, &a, &d);
        if (!(d & (1u << 29))) {
            serial_puts("[K32] CPUID.80000001:EDX.LM=0 -- no long mode\n");
            fail_screen("CPU has no long mode (use qemu-system-x86_64)");
            for (;;) __asm__ __volatile__("hlt");
        }
    }

    // ---- staging source: UEFI handoff or disk -------------------------
    volatile struct k64_handoff* h = (volatile struct k64_handoff*)0x7000;
    uint32_t stage = K64_STAGING_ADDR;

    if (h->magic == K64_HANDOFF_MAGIC) {
        stage = h->phys;
        serial_puts("[K32] using EFI-preloaded K64 @");
        serial_hex(stage);
        serial_puts("\n");
    } else {
        serial_puts("[K32] reading kernel64.bin from LBA 2048...\n");
        // Bring the primary channel to a known state first.  The old 32-bit
        // kernel always ran a full hw_detect (ATA IDENTIFY + PIC init) before
        // its first READ SECTORS; jumping straight into a PIO burst from the
        // stage2 hand-off state wedged the channel around LBA 0x82C (BSY
        // stuck, every retry failing at the same offsets).
        ata_soft_reset();
        for (volatile int i = 0; i < 1000000; i++) {}
        ata_prepare_channel();
        ata_soft_reset();
        for (volatile int i = 0; i < 1000000; i++) {}
        uint16_t* w = (uint16_t*)(uintptr_t)stage;
        int bad = 0;
        for (int attempt = 0; attempt < 4; attempt++){
            if (attempt) ata_soft_reset();
            bad = 0;
            w = (uint16_t*)(uintptr_t)stage;
            for (uint32_t blk = 0; blk < KERNEL64_SECTORS / 128; blk++){
                // 128 sectors per command: the ATA spec stores 256 as 0 in
                // the sector-count register, and this QEMU build truncates
                // count=0 back to 0 instead of 256 -- count=256 reads hang.
                if (!ata_read_multi(KERNEL64_LBA_DISK + blk * 128, 128, w)) {
                    bad++;
                    serial_puts("[K32] block read failed at LBA ");
                    serial_hex(KERNEL64_LBA_DISK + blk * 128);
                    serial_puts("\n");
                    break;
                }
                serial_putc('.');
                w += 128 * 256;
            }
            uint32_t magic = *(volatile uint32_t*)(uintptr_t)stage;
            if (!bad && magic == K64_MAGIC) break;
            serial_puts("[K32] staging attempt failed: bad=");
            serial_hex((uint32_t)bad);
            serial_puts(" magic=");
            serial_hex(magic);
            serial_puts(" -- reset + retry\n");
        }
        if (bad || *(volatile uint32_t*)(uintptr_t)stage != K64_MAGIC){
            serial_puts("[K32] FATAL: kernel64.bin could not be staged\n");
            fail_screen("cannot read kernel64.bin from disk (LBA 2048)");
            for (;;) __asm__ __volatile__("hlt");
        }
        serial_puts("[K32] kernel64.bin staged @");
        serial_hex(stage);
        serial_puts("\n");
    }

    // ---- hand off to long mode (never returns) ------------------------
    serial_puts("[K32] entering long mode...\n");
    switch_to_64bit(stage);

    serial_puts("[K32] FATAL: switch_to_64bit returned\n");
    fail_screen("switch_to_64bit returned (internal error)");
    for (;;) __asm__ __volatile__("hlt");
}
