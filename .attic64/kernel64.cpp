// =====================================================================
//  kernel64.cpp  -  Freestanding C++ 64-bit kernel with shell, mouse & scrollback
// ---------------------------------------------------------------------
//  Runs in 64-bit long mode, no standard library.
//
//  Features:
//   * VGA text output at 0xB8000 with a scrollback history buffer.
//   * PS/2 keyboard driver (Scan Code Set 1) + arrow keys.
//   * PS/2 mouse driver (Intellimouse 4-byte packets, wheel support).
//   * Mouse wheel / Up-Down arrows scroll back through history.
//   * Mini command shell with directory navigation (cd/mkdir/pwd).
//   * ATA PIO disk driver for persistent storage.
//   * MKFS: custom writable file system with directory support.
//   * SFS:  compatible read-only file system (pre-built by Makefile).
//   * MBR partition table reader + FAT32 file system reader.
//   * .sh script execution from MKFS or SFS.
//   * Path separators: both / and \ are accepted.
//
//  Build:
//    g++ -m32 -ffreestanding -fno-exceptions -fno-rtti -nostdlib
//        -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -O2 -c
// =====================================================================

#include <stdint.h>
#ifndef NULL
#define NULL 0
#endif
#include "clr.h"           // MiniCLR: managed (C#) shell host
#include "win32.h"          // Win32/Win64 subsystem: PE loader, registry, API shims
#include "ai_model.h"       // open-source model recognition + registry
#include "ai_env.h"         // VM vs bare-metal detection
#include "gguf_infer.h"     // real transformer inference over GGUF weights
#include "perm.h"            // Y/N permission prompt engine (security doc v1.0, 3.3)
#include "memory_adapter.h"  // ggml memory backend (Route A, phase 0)
#include "file_adapter.h"    // GGUF blob I/O backend (Route A, phase 0)
#include "gguf_loader.h"     // new self-contained GGUF parser (Route A, phase 0)
#include "gguf_fixture.h"    // minimal valid GGUF fixture for the adapter self-test
#include "knowledge_base.h"  // verified-fact KB (truth-from-practice + authoritative fallback)
#include "ai_plugin.h"        // plugin catalogue / manager (Settings > Plugins + `plugin` cmd)
#include "smp64.h"            // Stage 8.1: SMP / multiprocessor bring-up

// Phase 0 ggml adapter self-test (declared here, defined just before kmain64).
void gguf_adapter_selftest(void);

// =====================================================================
//  Port I/O helpers
// =====================================================================
static inline uint8_t  inb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p){ uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p){ uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(uint16_t p,uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p,uint16_t v){ __asm__ __volatile__("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl(uint16_t p,uint32_t v){ __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }

// =====================================================================
//  Serial debug output (port 0x3F8)  -  for UEFI boot tracing
// =====================================================================
void serial_puts(const char* s){ while(*s) outb(0x3F8, (uint8_t)*s++); }
void serial_hex(uint32_t v){
    const char* h = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) outb(0x3F8, (uint8_t)h[(v >> i) & 0xF]);
}
void serial_puthex(uint8_t b){
    const char* h = "0123456789ABCDEF";
    outb(0x3F8, (uint8_t)h[(b >> 4) & 0xF]);
    outb(0x3F8, (uint8_t)h[b & 0xF]);
}
void serial_dec(uint32_t v){
    char b[12]; int i = 0;
    if (v == 0) { outb(0x3F8, '0'); return; }
    while (v > 0 && i < 11) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) outb(0x3F8, (uint8_t)b[--i]);
}
void serial_putdec(uint32_t v){ serial_dec(v); }  // alias used by SFS probe

// =====================================================================
//  Tiny libc (freestanding)
// =====================================================================
static int   strlen_(const char* s){ int n=0; while(s[n]) n++; return n; }
static int   strcmp_(const char* a,const char* b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static int   strncmp_(const char* a,const char* b,int n){ while(n>0&&*a&&*a==*b){a++;b++;n--;} return n==0?0:(unsigned char)*a-(unsigned char)*b; }
static bool  startswith_(const char* s,const char* prefix){ while(*prefix){ if(*s!=*prefix) return false; s++; prefix++; } return true; }
static void* memset_(void* d,int v,int n){ unsigned char* p=(unsigned char*)d; while(n--) *p++=(unsigned char)v; return d; }
static void* memcpy_(void* d,const void* s,int n){ unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s; while(n--) *dp++=*sp++; return d; }

// Freestanding memcpy/memset/memmove for compiler-generated builtin calls (-O2)
extern "C" void* memcpy(void* d, const void* s, unsigned long n){
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    while(n--) *dp++=*sp++; return d;
}
extern "C" void* memset(void* d, int v, unsigned long n){
    unsigned char* p=(unsigned char*)d; while(n--) *p++=(unsigned char)v; return d;
}
extern "C" void* memmove(void* d, const void* s, unsigned long n){
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    if(dp<sp){ while(n--) *dp++=*sp++; }
    else { dp+=n; sp+=n; while(n--) *--dp=*--sp; }
    return d;
}

// =====================================================================
//  AI Engine interface (implemented in ai_engine.cpp)
// =====================================================================
extern "C" {
    int   ai_init(const char* model_path);
    char* ai_generate(const char* prompt, uint32_t max_tokens);
    void  ai_cleanup(void);
    int   ai_get_info(char* buf, int bufsize);
    int   ai_set_mode(int mode);
    int   ai_transformer_test(void);
    void  agent_init(void);
    void  agent_plan(const char* goal);
    int   agent_run(const char* goal, char* output, int outsize);
    int   agent_get_status(char* buf, int bufsize);
    void  agent_abort(void);
    void  agent_set_confirm(int on);
}
static bool g_ai_initialized = false;
static bool g_plugin_inited  = false;

// =====================================================================
//  Network stack interface (implemented in net.cpp)
// =====================================================================
extern "C" {
    int  net_init(void);
    void net_poll(void);
    int  net_status(char* buf, int bufsize);
    // Browser HTTP client API (implemented in net.cpp).  The 32-bit kernel
    // wires these straight into its GuiCallbacks; we do the same for the
    // 64-bit kernel so the managed BrowserApp can fetch real pages.
    int  browser_navigate(const char* url);
    int  browser_status(void);
    int  browser_get_page(char* buf, int bufsize);
    void browser_reset(void);
    int  net_http_get(const char* url, char* out, int outsize);
    int  net_agent_remote(const char* prompt, const char* url, char* out, int outsize);
    void net_set_agent_remote_url(const char* url);
    void net_set_agent_api_key(const char* key);
    void net_set_agent_model(const char* model);
    const char* net_ip_str(void);
}
static bool g_net_initialized = false;

// =====================================================================
//  32-bit kernel switcher (implemented in switch64to32.asm)
// =====================================================================
extern "C" void switch_to_32bit(void);

#define KERNEL32_LBA     33       // LBA where kernel.bin is stored on disk
#define KERNEL32_SECTORS 1024     // 512KB max (1024*512); 32-bit kernel.bin is ~427KB / 835 sectors -- 256 was too small and left the image truncated -> silent hang on switch back
#define KERNEL32_ADDR    0x10000  // Load address: 64KB (matches linker.ld)

// =====================================================================
//  ATA PIO disk driver (primary master, LBA28)
//  QEMU exposes the raw image here. Used to persist the command file.
// =====================================================================
static uint8_t  ata_last_status;   // last status reg read (for diagnostics)
static uint8_t  ata_last_err;       // 0 = ok, else stage where it failed

// Wait until the device is not busy (BSY clear).  Returns false on timeout
// or if the ERR bit latches.
static bool ata_nobsy(int to){
    for(int i=0;i<to;i++){
        uint8_t s = inb(0x1F7);
        if(!(s & 0x80)) return true;
        if(s & 0x01) return false;   // ERR
    }
    return false;
}
// Wait until data is ready to transfer (BSY clear AND DRQ set).
static bool ata_drq(int to){
    for(int i=0;i<to;i++){
        uint8_t s = inb(0x1F7);
        if((s & 0x88) == 0x08) return true;   // !BSY && DRQ
        if(s & 0x01) return false;            // ERR
    }
    return false;
}

static void ata_read_sector(uint32_t lba, uint16_t* buf){
    ata_last_err = 0;
    // 1) device must be idle before we touch any registers
    if(!ata_nobsy(50000)) { ata_last_status = inb(0x1F7); ata_last_err = 1; return; }
    // 2) select drive + LBA mode, THEN wait again: selecting a device can
    //    assert BSY, and any register write while BSY is set is dropped by the
    //    controller -- which is exactly why the old driver always read LBA0
    //    (the LBA registers never latched and the drive used its default CHS).
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    if(!ata_nobsy(50000)) { ata_last_status = inb(0x1F7); ata_last_err = 2; return; }
    // 3) now the parameters will actually latch
    outb(0x1F1, 0x00);
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)(lba & 0xFF));
    outb(0x1F4, (uint8_t)((lba>>8) & 0xFF));
    outb(0x1F5, (uint8_t)((lba>>16) & 0xFF));
    outb(0x1F7, 0x20);                 // READ SECTORS
    // 4) wait for the data to be ready (BSY clear + DRQ set)
    if(!ata_drq(2000000)) { ata_last_status = inb(0x1F7); ata_last_err = 3; return; }
    __asm__ __volatile__("cld; rep insw" :: "d"((uint16_t)0x1F0), "D"(buf), "c"(256) : "memory");
    ata_last_status = inb(0x1F7);      // final status / clear IRQ
}

// Multi-sector PIO read: issues ONE READ SECTORS command for `count` sectors
// (count<=128).  Drastically cuts per-sector command overhead vs calling
// ata_read_sector in a loop (776964 single-sector commands for the model).
// Uses `rep insw` (string I/O) so QEMU can transfer the whole block in one
// shot instead of one inw per word.
static void ata_read_multi(uint32_t lba, uint8_t count, uint16_t* buf){
    ata_last_err = 0;
    if(!ata_nobsy(50000)) { ata_last_status = inb(0x1F7); ata_last_err = 1; return; }
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    if(!ata_nobsy(50000)) { ata_last_status = inb(0x1F7); ata_last_err = 2; return; }
    outb(0x1F1, 0x00);
    outb(0x1F2, count);
    outb(0x1F3, (uint8_t)(lba & 0xFF));
    outb(0x1F4, (uint8_t)((lba>>8) & 0xFF));
    outb(0x1F5, (uint8_t)((lba>>16) & 0xFF));
    outb(0x1F7, 0x20);                 // READ SECTORS
    if(!ata_drq(2000000)) { ata_last_status = inb(0x1F7); ata_last_err = 3; return; }
    // One rep insw for the whole batch (count*256 words).
    __asm__ __volatile__("cld; rep insw" :: "d"((uint16_t)0x1F0), "D"(buf), "c"((uint32_t)count*256u) : "memory");
    ata_last_status = inb(0x1F7);
}

// =====================================================================
//  IDE Bus Master DMA (BM-DMA) for bulk model loading.
//  QEMU's PIIX3 (00:01.1) exposes a BM interface; DMA copies disk data
//  straight into RAM with ~zero per-byte CPU cost, vs millions of PIO
//  inw/insw that TCG emulates painfully slowly.
// =====================================================================
static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    uint32_t addr = 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    uint32_t addr = 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    return (uint16_t)(inl(0xCFC) >> ((reg&3)*8));
}

// Returns the BM interface IO base (e.g. 0xC000), or 0 if not found.
static uint16_t ata_bm_base(void){
    // PIIX3 IDE is typically 00:01.1, class 0x0101 (IDE). Scan a few slots.
    for (uint8_t dev=0; dev<4; dev++){
        uint32_t id = pci_cfg_read32(0, dev, 1, 0);
        if (id == 0xFFFFFFFFu) continue;
        uint32_t cls = pci_cfg_read32(0, dev, 1, 8);
        uint8_t class_base = (uint8_t)(cls >> 24);
        uint8_t class_sub  = (uint8_t)(cls >> 16);
        if (class_base == 0x01 && class_sub == 0x01){
            // Enable PCI Bus Mastering so the BM-DMA controller is live.
            uint16_t cmd = pci_cfg_read16(0, dev, 1, 0x04);
            if (!(cmd & 0x04)){
                cmd |= 0x04;
                // write back via config dword (16-bit write at 0x04)
                uint32_t cur = pci_cfg_read32(0, dev, 1, 0x04);
                cur = (cur & 0xFFFF0000u) | (uint32_t)cmd | 0x04u;
                // use 0xCF8/0xCFC dword write
                uint32_t addr = 0x80000000u | ((uint32_t)0<<16) | ((uint32_t)dev<<11) | ((uint32_t)1<<8) | (0x04 & 0xFC);
                outl(0xCF8, addr);
                outl(0xCFC, cur);
            }
            uint16_t bm = pci_cfg_read16(0, dev, 1, 0x20);
            if (bm & 1) return (uint16_t)(bm & 0xFFF0);
        }
    }
    return 0;
}

// Single PRDT entry: 32-bit phys addr, 32-bit byte count (0 => 64KiB), flags.
static uint32_t g_bm_base = 0;
// PRDT is a static (identity-mapped) buffer so its phys==virt address is
// always valid regardless of PMM init order.
static uint32_t g_prdt[4] __attribute__((aligned(16)));
static uint32_t g_prdt_phys = 0;

// Read up to 64KiB (128 sectors) via BM-DMA into a physically-contiguous
// buffer `buf_phys` (which, in identity-mapped long mode, is also the VA).
static bool ata_read_dma_block(uint32_t lba, uint8_t* buf_phys){
    if (!g_bm_base) return false;
    uint16_t bm = (uint16_t)g_bm_base;
    // Program PRDT: one entry, 64KiB, EOT (bit0).
    volatile uint32_t* prdt = (volatile uint32_t*)(uintptr_t)g_prdt_phys;
    prdt[0] = (uint32_t)(uintptr_t)buf_phys;
    prdt[1] = 0x00010000u;   // 64KiB explicit (byte count; 0x10000 = 65536)
    prdt[2] = 0x00000001u;   // EOT
    outl(bm+4, g_prdt_phys); // PRD table address
    outb(bm+2, 0x06);        // clear BM status (INTR|ERROR)
    // Enable DMA transfer mode on the device (SET FEATURES, subcmd 0x03),
    // once. Without this the drive answers READ DMA with a PIO DRQ and the
    // bus master never sees data -> BM error.
    static bool dma_enabled = false;
    if (!dma_enabled){
        if(ata_nobsy(50000)){
            outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
            if(ata_nobsy(50000)){
                outb(0x1F1, 0x03);   // DMA mode
                outb(0x1F2, 0x00);
                outb(0x1F3, 0x00);
                outb(0x1F4, 0x00);
                outb(0x1F5, 0x00);
                outb(0x1F7, 0xEF);   // SET FEATURES
                // wait for completion
                uint32_t w=0; while(!(inb(0x1F7)&0x40) && w<2000000u){w++;}
                dma_enabled = true;
            }
        }
    }
    // Issue READ DMA (LBA28, 128 sectors = 64KiB).
    if(!ata_nobsy(50000)) { ata_last_err = 11; return false; }
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    if(!ata_nobsy(50000)) { ata_last_err = 12; return false; }
    outb(0x1F1, 0x00);
    outb(0x1F2, 128);
    outb(0x1F3, (uint8_t)(lba & 0xFF));
    outb(0x1F4, (uint8_t)((lba>>8) & 0xFF));
    outb(0x1F5, (uint8_t)((lba>>16) & 0xFF));
    outb(0x1F7, 0xC8);       // READ DMA
    outb(bm+0, 0x01);        // start bus master read (bit0=start, bit3=0 => device->memory)
    // One-time diagnostic on first call.
    static bool dbg_done = false;
    if (!dbg_done){
        dbg_done = true;
        serial_puts("[DMA-DBG] bmcmd="); serial_hex(inb(bm+0));
        serial_puts(" bmstat="); serial_hex(inb(bm+2));
        serial_puts(" ide="); serial_hex(inb(0x1F7));
        serial_puts(" prdt0="); serial_hex(prdt[0]);
        serial_puts("\n");
    }
    // Poll BM status (bit0 = active -> clears on completion) with timeout.
    uint32_t t = 0;
    while (inb(bm+2) & 0x01) { if (++t > 4000000u) { outb(bm+0, 0x00); ata_last_err = 13; if(!dbg_done){} serial_puts("[DMA-DBG] TIMEOUT bmstat="); serial_hex(inb(bm+2)); serial_puts("\n"); return false; } }
    outb(bm+0, 0x00);        // stop BM
    uint8_t st = inb(0x1F7);
    ata_last_status = st;
    if (inb(bm+2) & 0x02) { ata_last_err = 14; serial_puts("[DMA-DBG] BM-ERROR bmstat="); serial_hex(inb(bm+2)); serial_puts("\n"); return false; } // BM error
    return true;
}

// =====================================================================
//  virtio-blk (legacy) for fast bulk model loading.
//  Unlike IDE PIO (TCG-emulated byte-by-byte, ~100KB/s), virtio moves
//  data through an in-RAM descriptor ring: QEMU memcpy's the disk blocks
//  straight into our buffer, so a single request can pull the whole 379MB
//  model at memory speed.
// =====================================================================
extern "C" void* big_alloc(uint32_t size);
extern "C" void big_free(void* p, uint32_t size);
#define VIRTIO_BLK_T_IN 0u
struct vblk_vq {
    volatile uint8_t* desc;   // descriptor table (16 bytes/entry)
    volatile uint8_t* avail;  // available ring
    volatile uint8_t* used;   // used ring
    uint16_t qsz;
};
static uint16_t g_vblk_io = 0;     // virtio-blk IO port base (BAR0)
static uint8_t  g_vblk_irq = 0;    // unused (polled)
static struct vblk_vq g_vq;
static uint32_t g_vq_phys = 0;
static bool g_vblk_tried = false;   // lazy init guard (after PMM is up)

static uint16_t vpci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    uint32_t addr = 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    return (uint16_t)(inl(0xCFC) >> ((reg&3)*8));
}
static uint32_t vpci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    uint32_t addr = 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void vpci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val){
    uint32_t addr = 0x80000000u | ((uint32_t)bus<<16) | ((uint32_t)dev<<11) |
                    ((uint32_t)fn<<8) | (reg & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static bool virtio_blk_init(void){
    for (uint8_t dev=0; dev<32; dev++){
        uint32_t id = vpci_read32(0, dev, 0, 0);
        if ((id & 0xFFFF) != 0x1AF4) continue;
        uint16_t did = (uint16_t)(id >> 16);
        if (did != 0x1001) continue;   // virtio-blk
        // Enable IO + Bus Master in command register (0x04).
        uint32_t cmd = vpci_read32(0, dev, 0, 0x04);
        cmd |= 0x01u | 0x04u;          // IO space + bus master
        vpci_write32(0, dev, 0, 0x04, cmd);
        uint32_t bar0 = vpci_read32(0, dev, 0, 0x10);
        if (!(bar0 & 1)) { serial_puts("[VBLK] BAR0 not IO\n"); return false; }
        uint16_t base = (uint16_t)(bar0 & 0xFFFC);
        // Device status: reset -> acknowledge -> driver -> features_ok -> driver_ok
        outb(base+0x12, 0);            // reset
        outb(base+0x12, 0x01);         // ACKNOWLEDGE
        outb(base+0x12, 0x01|0x02);    // DRIVER
        uint32_t devfeat = inl(base+0x00);
        // Negotiate NO features (virtio-blk legacy base I/O needs none). Writing
        // back device features verbatim can include bits the device rejects.
        outl(base+0x04, 0);
        outb(base+0x12, 0x01|0x02|0x04); // FEATURES_OK
        // Select queue 0 and read its size.
        outw(base+0x0E, 0);
        uint16_t qsz = inw(base+0x0C);
        if (qsz == 0 || qsz > 8192) { serial_puts("[VBLK] bad qsz\n"); return false; }
        g_vq.qsz = qsz;
        // Allocate physically-contiguous virtqueue (identity-mapped).
        // Legacy layout: descriptor table, then driver area (avail ring),
        // then the device area (used ring) on its OWN 4K page (QEMU requires
        // the used ring to be page-aligned).
        uint32_t desc_sz = (uint32_t)qsz * 16u;
        uint32_t avail_sz = 4u + (uint32_t)qsz * 2u + 2u;   // flags+idx+ring+used_event
        uint32_t used_sz  = 4u + (uint32_t)qsz * 8u + 2u;   // flags+idx+elem(8B)+avail_event
        uint32_t vq = (uint32_t)(uintptr_t)big_alloc( (desc_sz + avail_sz + 8192u + used_sz + 4095u) & ~4095u );
        if (!vq) { serial_puts("[VBLK] no mem\n"); return false; }
        g_vq_phys = vq;
        uint32_t avail_off = vq + desc_sz;
        uint32_t used_off  = (vq + desc_sz + avail_sz + 4095u) & ~4095u;
        g_vq.desc = (volatile uint8_t*)(uintptr_t)vq;
        g_vq.avail = (volatile uint8_t*)(uintptr_t)avail_off;
        g_vq.used  = (volatile uint8_t*)(uintptr_t)used_off;
        // Tell device the queue address (page frame number).
        outl(base+0x08, vq >> 12);
        // Zero the ring state.
        uint32_t total = (used_off + used_sz + 4095u) & ~4095u;
        for (uint32_t i=0; i<total; i++) *(volatile uint8_t*)(uintptr_t)(vq+i) = 0;
        outb(base+0x12, 0x01|0x02|0x04|0x08); // DRIVER_OK
        g_vblk_io = base;   // mark ready only after full setup
        serial_puts("[VBLK] ready io="); serial_hex(base);
        serial_puts(" qsz="); serial_hex(qsz); serial_puts("\n");
        return true;
    }
    return false;
}

// Read `nsect` sectors from `lba` into physically-contiguous `buf` in ONE
// virtio request (only 3 descriptors needed regardless of size).
static bool virtio_blk_read(uint32_t lba, uint8_t* buf, uint32_t nsect){
    if (!g_vblk_io) return false;
    uint16_t base = g_vblk_io;
    // Request header: type(4) + reserved(4) + sector(8) = 16 bytes.
    static uint8_t hdr[16] __attribute__((aligned(16)));
    static uint8_t status __attribute__((aligned(8)));
    status = 0xFF;
    *(volatile uint32_t*)(hdr+0) = VIRTIO_BLK_T_IN;
    *(volatile uint32_t*)(hdr+4) = 0;
    *(volatile uint64_t*)(hdr+8) = (uint64_t)lba;

    volatile uint8_t* d = g_vq.desc;
    uint32_t doff = 0;
    // desc0: header (device reads it) -> NEXT -> 1
    *(volatile uint64_t*)(d+doff+0) = (uint64_t)(uintptr_t)hdr;
    *(volatile uint32_t*)(d+doff+8) = 16;
    *(volatile uint16_t*)(d+doff+12) = 1;          // flags = NEXT
    *(volatile uint16_t*)(d+doff+14) = 1;          // next = 1
    // desc1: data (device writes) -> NEXT -> 2, WRITE flag
    *(volatile uint64_t*)(d+doff+16) = (uint64_t)(uintptr_t)buf;
    *(volatile uint32_t*)(d+doff+24) = nsect*512u;
    *(volatile uint16_t*)(d+doff+28) = (1u<<1) | 1u; // WRITE | NEXT
    *(volatile uint16_t*)(d+doff+30) = 2;          // next = 2
    // desc2: status (device writes) -> no NEXT, WRITE flag
    *(volatile uint64_t*)(d+doff+32) = (uint64_t)(uintptr_t)&status;
    *(volatile uint32_t*)(d+doff+40) = 1;
    *(volatile uint16_t*)(d+doff+44) = (1u<<1);    // WRITE
    *(volatile uint16_t*)(d+doff+46) = 0;

    // Available ring: place head index 0, bump idx.
    volatile uint8_t* av = g_vq.avail;
    uint16_t idx = *(volatile uint16_t*)(av+2);
    *(volatile uint16_t*)(av+4) = 0;               // ring[0] = head desc
    *(volatile uint16_t*)(av+2) = (uint16_t)(idx + 1); // idx++
    outl(base+0x10, 0);                            // notify queue 0 (32-bit write)
    // tiny delay so the device can pick up the notification
    for (volatile uint32_t d=0; d<1000; d++){}

    // Poll used ring idx until it advances past our submission.
    volatile uint8_t* us = g_vq.used;
    uint32_t t = 0;
    while (*(volatile uint16_t*)(us+2) == idx){
        if (++t > 20000000u){
            serial_puts("[VBLK] TIMEOUT devstat="); serial_hex(inb(base+0x12));
            serial_puts(" isr="); serial_hex(inb(base+0x14));
            serial_puts(" usedidx="); serial_hex(*(volatile uint16_t*)(us+2));
            serial_puts(" availidx="); serial_hex(*(volatile uint16_t*)(g_vq.avail+2));
            // used ring elem[0]: id @ us+4 (4B), len @ us+8 (4B)
            serial_puts(" used_e0_id="); serial_hex(*(volatile uint32_t*)(us+4));
            serial_puts(" used_e0_len="); serial_hex(*(volatile uint32_t*)(us+8));
            serial_puts(" desc0="); serial_hex64(*(volatile uint64_t*)d);
            serial_puts("\n");
            return false;
        }
    }
    return status == 0;
}

static void ata_write_sector(uint32_t lba, const uint16_t* buf){
    if(!ata_nobsy(50000)) return;
    outb(0x1F6, 0xE0 | ((lba>>24)&0x0F));
    if(!ata_nobsy(50000)) return;
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t)(lba & 0xFF));
    outb(0x1F4, (uint8_t)((lba>>8) & 0xFF));
    outb(0x1F5, (uint8_t)((lba>>16) & 0xFF));
    outb(0x1F7, 0x30);                 // WRITE SECTORS
    if(!ata_drq(2000000)) return;
    for(int i=0;i<256;i++) outw(0x1F0, buf[i]);
    outb(0x1F7, 0xE7);                 // CACHE FLUSH
    ata_nobsy(50000);
}

// (ata_diag_probe removed — debug-only, no longer needed)

// =====================================================================
//  Diagnostic fault capture (serial + disk sector LBA1500)
//  Replaces color-beacon guessing with explicit step markers and a real
//  64-bit IDT.  Any synchronous fault (#PF/#GP/#UD/#DE/...) routes to
//  fault_common(), which dumps CR2/RIP/step to the serial port and persists
//  a 512-byte record to LBA1500 (genuinely spare: kernel.bin @33 spans
//  33..1056, kernel64 @2048, so 1500 is in the unused gap).
//  The 64-bit kernel never enables IRQs (no STI), so loading this IDT only
//  catches synchronous CPU faults -- no IRQ storm is possible.
// =====================================================================
extern "C" const uint64_t isr_stub_table[256];

struct idt_entry {
    uint16_t offlow;
    uint16_t sel;
    uint16_t ist_flags;
    uint16_t offmid;
    uint32_t offhigh;
    uint32_t rsv;
};
static idt_entry g_idt[256];
static struct { uint16_t limit; uint64_t base; } __attribute__((packed)) g_idt_desc;

// Last diag step reached, so a fault can report where it died.
static uint32_t g_last_step = 0;
static char g_last_step_name[32] = {0};

static void serial_hex64(uint64_t v){
    serial_hex((uint32_t)(v >> 32));
    serial_hex((uint32_t)(v & 0xFFFFFFFFULL));
}

// Mark a named execution step.  Only prints / records when the step id
// changes, so hot loops don't flood the serial log.  Stored so that a later
// fault can report the last step reached before it died.
extern "C" void diag_step(uint32_t id, const char* name){
    if (id != g_last_step) {
        serial_puts("[DIAG] step=");
        serial_dec(id);
        serial_puts(" ");
        serial_puts(name);
        serial_puts("\n");
        g_last_step = id;
        for (int i = 0; i < 31 && name[i]; i++) g_last_step_name[i] = name[i];
        g_last_step_name[31] = 0;

        // Persist progress to LBA1500 so a freeze can be diagnosed from disk
        // without serial or screen colour.  Skip the per-icon mh_* draw calls
        // (steps 300-311) to avoid thrashing the disk in the GUI repaint loop
        // -- those are never the freeze point.
        if (id >= 100 && (id < 300 || id > 311)) {
            // On real UEFI hardware the GOP framebuffer lives above 4GB and
            // issuing ATA PIO writes from long mode there can hard-freeze the
            // kernel (heisenbug; disk-persist is also useless under AHCI, where
            // legacy PIO writes are invalid).  Skip the sector write when the
            // framebuffer is high; serial logging above already captured the
            // step.  BIOS/VBE paths keep the on-disk progress record.
            uint64_t fb64 = *(volatile uint64_t*)((volatile uint8_t*)0x5000 + 0x10);
            if (fb64 <= 0xFFFFFFFFULL) {
                uint16_t rec[256];
                memset_(rec, 0, 512);
                rec[0] = 0x4E45;   // 'NE'
                rec[1] = 0x5844;   // 'XD'
                rec[2] = (uint16_t)id;
                rec[3] = 0;        // vec  = 0 => progress marker, not a fault
                rec[4] = 0;        // err  = 0
                for (int i = 0; i < 30 && name[i]; i++)
                    ((uint8_t*)rec)[28 + i] = (uint8_t)name[i];
                ata_write_sector(1500, rec);
            }
        }
    }
}

// Synchronous fault handler (called from isr_common in entry64.asm).
// Dumps context to serial and writes a 512-byte record to LBA1500.
extern "C" void fault_common(uint32_t intno, uint32_t errcode, uint64_t rip,
                         uint64_t cs, uint64_t rflags, uint64_t cr2){
    serial_puts("[DIAG-FAULT] vec=");
    serial_dec(intno);
    serial_puts(" err=");
    serial_hex(errcode);
    serial_puts(" rip=");
    serial_hex64(rip);
    serial_puts(" cr2=");
    serial_hex64(cr2);
    serial_puts(" step=");
    serial_dec(g_last_step);
    serial_puts(" name=");
    serial_puts(g_last_step_name);
    serial_puts("\n");

    // Persist a 512-byte record to LBA1500 (genuinely spare: kernel.bin @33
    // spans 33..1056, kernel64 @2048, so 1500 is in the unused gap -- writing
    // here does NOT corrupt either kernel image on disk).
    // Layout (little-endian words):
    //   rec[0..1]  = 'NEXD' magic
    //   rec[2]     = last step id
    //   rec[3]     = fault vector
    //   rec[4]     = error code
    //   rec[5..8]  = RIP (64-bit)
    //   rec[9..12] = CR2 (64-bit)
    //   bytes 28..57 = last step name (ASCII)
    uint16_t rec[256];
    memset_(rec, 0, 512);
    rec[0] = 0x4E45;              // 'NE'
    rec[1] = 0x5844;              // 'XD'
    rec[2] = (uint16_t)g_last_step;
    rec[3] = (uint16_t)intno;
    rec[4] = (uint16_t)errcode;
    rec[5] = (uint16_t)(rip & 0xFFFF);
    rec[6] = (uint16_t)((rip >> 16) & 0xFFFF);
    rec[7] = (uint16_t)((rip >> 32) & 0xFFFF);
    rec[8] = (uint16_t)((rip >> 48) & 0xFFFF);
    rec[9]  = (uint16_t)(cr2 & 0xFFFF);
    rec[10] = (uint16_t)((cr2 >> 16) & 0xFFFF);
    rec[11] = (uint16_t)((cr2 >> 32) & 0xFFFF);
    rec[12] = (uint16_t)((cr2 >> 48) & 0xFFFF);
    for (int i = 0; i < 30 && g_last_step_name[i]; i++)
        ((uint8_t*)rec)[28 + i] = (uint8_t)g_last_step_name[i];
    ata_write_sector(1500, rec);

    for (;;) __asm__ __volatile__("cli; hlt");
}

// Build the IDT from the stub table exported by entry64.asm and load it.
//
// ADDRESSING NOTE: the 64-bit kernel ELF is linked at 0x100000, but the
// 32-bit loader stages the flat binary at a VARIABLE physical address
// (kmalloc'd in the 3-19 MiB heap; e.g. 0x300010 this boot).  Relative code
// is unaffected, but the IDT gate targets are ABSOLUTE, so they must be
// rebased by the real load delta.  We recover that delta with a PC-relative
// call/pop (gives this function's ACTUAL runtime address) minus its LINK
// address (from the relocation of &build_idt).  g_idt base is rebased too.
static void build_idt(){
    uint64_t actual_self;
    uint64_t link_self;
    // Get this function's ACTUAL runtime address via a RIP-relative LEA.
    // (Writing the LEA explicitly avoids GCC's flaky optimizer transform of
    // the classic "call 1f; pop %0" idiom into a mis-encoded `lea`.)
    __asm__ __volatile__("lea 0(%%rip), %0" : "=r"(actual_self));
    link_self = (uint64_t)&&idt_self;
idt_self:
    uint64_t delta = actual_self - link_self;
    serial_puts("[K64-IDT] delta=");
    serial_hex64(delta);
    serial_puts("\n");
    for (int i = 0; i < 256; i++) {
        uint64_t a = isr_stub_table[i] + delta;
        g_idt[i].offlow    = (uint16_t)(a & 0xFFFF);
        g_idt[i].sel       = 0x08;
        g_idt[i].ist_flags = 0x8E00;
        g_idt[i].offmid    = (uint16_t)((a >> 16) & 0xFFFF);
        g_idt[i].offhigh   = (uint32_t)(a >> 32);
        g_idt[i].rsv       = 0;
    }
    g_idt_desc.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idt_desc.base  = (uint64_t)g_idt + delta;
    __asm__ __volatile__("lidt %0" :: "m"(g_idt_desc));
}

// =====================================================================
//  VGA text mode 3 setup (80x25, 16 colors)
//  Needed for UEFI boot: OVMF leaves the VGA in a graphics mode.
//  BIOS boot: VGA is already in text mode, so this is harmless.
// =====================================================================
static void vga_set_text_mode(){
    outb(0x3C2, 0x67);
    static const uint8_t seq_data[5] = {0x03, 0x01, 0x03, 0x00, 0x02};
    for (int i = 0; i < 5; i++) { outb(0x3C4, i); outb(0x3C5, seq_data[i]); }
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    static const uint8_t crtc_data[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
        0x00, 0x4F, 0x0E, 0x0F, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
        0xFF
    };
    for (int i = 0; i < 25; i++) { outb(0x3D4, i); outb(0x3D5, crtc_data[i]); }
    static const uint8_t gc_data[9] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
    };
    for (int i = 0; i < 9; i++) { outb(0x3CE, i); outb(0x3CF, gc_data[i]); }
    static const uint8_t ac_data[21] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x0C, 0x00, 0x0F, 0x08, 0x00
    };
    inb(0x3DA);
    for (int i = 0; i < 21; i++) { outb(0x3C0, i); outb(0x3C0, ac_data[i]); }
    outb(0x3C0, 0x20);
}

// =====================================================================
//  Clipboard  -  copy/paste buffer with history (needed by Terminal)
// =====================================================================
#define CLIP_LEN       256
#define CLIP_HIST_MAX  8

// Not static: gui.cpp and mforms.cpp link against these directly, exactly as
// they do in the 32-bit kernel (kernel.cpp:930).
char g_clipboard[CLIP_LEN];
int  g_clipboard_len = 0;
static char g_clip_hist[CLIP_HIST_MAX][CLIP_LEN];
static int  g_clip_hist_count = 0;
static int  g_clip_hist_idx = 0;

void clipboard_set(const char* text, int len){
    if(len >= CLIP_LEN) len = CLIP_LEN - 1;
    memcpy_(g_clipboard, text, len);
    g_clipboard[len] = 0;
    g_clipboard_len = len;
    if(g_clip_hist_count < CLIP_HIST_MAX){
        memcpy_(g_clip_hist[g_clip_hist_count], g_clipboard, len+1);
        g_clip_hist_count++;
    } else {
        for(int i=0; i<CLIP_HIST_MAX-1; i++)
            memcpy_(g_clip_hist[i], g_clip_hist[i+1], CLIP_LEN);
        memcpy_(g_clip_hist[CLIP_HIST_MAX-1], g_clipboard, len+1);
    }
    g_clip_hist_idx = g_clip_hist_count - 1;
}

static void clipboard_hist_prev(){
    if(g_clip_hist_count == 0) return;
    if(g_clip_hist_idx > 0) g_clip_hist_idx--;
    memcpy_(g_clipboard, g_clip_hist[g_clip_hist_idx], CLIP_LEN);
    g_clipboard_len = strlen_(g_clipboard);
}

static void clipboard_hist_next(){
    if(g_clip_hist_count == 0) return;
    if(g_clip_hist_idx < g_clip_hist_count - 1) g_clip_hist_idx++;
    memcpy_(g_clipboard, g_clip_hist[g_clip_hist_idx], CLIP_LEN);
    g_clipboard_len = strlen_(g_clipboard);
}

namespace {

// ----- VGA constants -----
constexpr int VGA_WIDTH  = 80;
constexpr int VGA_HEIGHT = 25;
static uint16_t VGA_SHADOW[VGA_WIDTH * VGA_HEIGHT];
static bool     g_vga_shadow_active = false;

constexpr int SCROLLBACK_LINES = 200;

}

// Non-const pointer so it can be repointed to a shadow buffer in VBE mode.
// External linkage: gui.cpp's fb_console_render references it.
volatile uint16_t* VGA_MEMORY = reinterpret_cast<volatile uint16_t*>(0xB8000);

// Framebuffer console (fbcon) is already compiled into this 64-bit kernel via
// gui64.o (the same fb_console_init/render/clear the 32-bit kernel uses).  We
// just have to *activate* it so the shell draws into the VBE linear framebuffer
// (the LFB that `make play` / the GTK/SDL window actually shows) instead of the
// legacy 0xB8000 VGA text buffer, which those displays do not surface.
// Without this, `switch` from the 32-bit GUI lands in a black screen even
// though the 64-bit kernel is fully running (serial output is fine).
extern "C" void fb_console_init(void);
extern "C" void fb_console_render(void);
extern "C" void fb_console_force_redraw(void);
static bool g_fb_console_mode = false;

namespace {

enum VgaColor : uint8_t {
    BLACK=0, BLUE=1, GREEN=2, CYAN=3, RED=4, MAGENTA=5,
    BROWN=6, LIGHT_GREY=7, WHITE=15, YELLOW=14,
};
inline uint8_t  make_color(VgaColor fg, VgaColor bg){ return (uint8_t)(fg | (bg<<4)); }
inline uint16_t make_entry(unsigned char c, uint8_t color){ return (uint16_t)c | ((uint16_t)color<<8); }

struct Line { int len; char data[VGA_WIDTH]; };

// =====================================================================
//  Terminal  -  VGA output with scrollback history + view scrolling
// =====================================================================

// When g_capturing is true, terminal output is also captured into g_exec_output
// so the managed shell's terminal / exec callbacks can return text to C#.
static char g_exec_output[2048];
static int  g_exec_output_len;
static bool g_capturing;

// ---- SSH output sink (shared with 32-bit kernel; see net.cpp) ----
typedef void (*ssh_out_fn_t)(const char* data, int len);
static ssh_out_fn_t g_ssh_out_fn = 0;

class Terminal {
public:
    void init(){
        m_head=0; m_count=0; m_cur_len=0; m_cur_pos=0; m_prompt_len=0; m_at_bottom=true; m_view=0;
        m_color=make_color(LIGHT_GREY,BLACK);
        clear_screen();
    }
    void set_color(uint8_t c){ m_color=c; }

    void clear_screen(){
        m_head=0; m_count=0; m_cur_len=0; m_cur_pos=0; m_prompt_len=0; m_at_bottom=true; m_view=0;
        for(int i=0;i<VGA_WIDTH*VGA_HEIGHT;i++) VGA_MEMORY[i]=make_entry(' ',m_color);
        render();
    }

    void put_char(char c){
        // Capture output for GUI terminal / exec-command callbacks.
        if(g_capturing && g_exec_output_len < (int)sizeof(g_exec_output)-1)
            g_exec_output[g_exec_output_len++] = c;
        // Forward to an active SSH channel if one is consuming shell output
        if(g_ssh_out_fn) g_ssh_out_fn(&c, 1);
        if(c=='\n'){ commit_line(); return; }
        if(c=='\b'){
            if(m_cur_pos>0){
                // Shift characters left from cursor position
                for(int i=m_cur_pos-1; i<m_cur_len-1; i++)
                    m_cur[i]=m_cur[i+1];
                m_cur_len--;
                m_cur_pos--;
            }
            return;
        }
        if(m_cur_len<VGA_WIDTH){
            // Insert at cursor position: shift characters right
            for(int i=m_cur_len; i>m_cur_pos; i--)
                m_cur[i]=m_cur[i-1];
            m_cur[m_cur_pos++]=c;
            m_cur_len++;
        }
    }
    void write(const char* s){ while(*s) put_char(*s++); }

    void write_dec(int v){
        char b[12]; int i=0;
        if(v==0){ put_char('0'); return; }
        if(v<0){ put_char('-'); v=-v; }
        while(v){ b[i++]='0'+v%10; v/=10; }
        while(i) put_char(b[--i]);
    }

    void write_hex(uint64_t v){
        put_char('0'); put_char('x');
        char b[16]; int i=0;
        if(v==0){ put_char('0'); return; }
        while(v){ uint8_t d=v&0xF; b[i++]=(d<10)?('0'+d):('A'+d-10); v>>=4; }
        while(i) put_char(b[--i]);
    }

    void scroll_view(int delta){
        m_at_bottom=false;
        m_view += delta;
        int bot=bottom_view();
        if(m_view<0) m_view=0;
        if(m_view>bot) { m_view=bot; m_at_bottom=true; }
        render();
    }
    void snap_bottom(){ m_at_bottom=true; render(); }
    bool is_at_bottom() const { return m_at_bottom; }

    // ----- Cursor movement (PowerShell-style) -----
    void cursor_left(){  if(m_cur_pos>m_prompt_len)    m_cur_pos--; render(); }
    void cursor_right(){ if(m_cur_pos<m_cur_len)       m_cur_pos++; render(); }
    void cursor_home(){  m_cur_pos=m_prompt_len; render(); }
    void cursor_end(){   m_cur_pos=m_cur_len; render(); }
    int  cursor_pos() const { return m_cur_pos; }
    // Mark where user input begins (after prompt)
    void begin_input(){ m_prompt_len = m_cur_len; m_cur_pos = m_cur_len; }
    void set_cursor_col(int col){
        int abs_col = m_prompt_len + col;
        if(abs_col < m_prompt_len) abs_col = m_prompt_len;
        if(abs_col > m_cur_len) abs_col = m_cur_len;
        m_cur_pos = abs_col;
        render();
    }
    // Replace entire current input line content (for history recall)
    // 's' is the user input only (prompt is preserved)
    void set_line(const char* s, int len){
        m_cur_len = m_prompt_len;
        m_cur_pos = m_prompt_len;
        for(int i=0; i<len && m_cur_len<VGA_WIDTH-1; i++){
            m_cur[m_cur_len++]=s[i];
        }
        m_cur_pos=m_cur_len;
        render();
    }
    // Get current input line content (user input only, excluding prompt)
    void get_line(char* buf, int* len){
        int n = m_cur_len - m_prompt_len;
        if(n < 0) n = 0;
        for(int i=0; i<n; i++) buf[i]=m_cur[m_prompt_len + i];
        buf[n]=0;
        *len=n;
    }

    // ----- Mouse cursor & text selection -----
    void update_mouse(int dx, int dy){
        m_mouse_visible = true;
        m_mouse_x += dx / 3;
        m_mouse_y -= dy / 3;       // PS/2 Y is inverted
        if(m_mouse_x < 0) m_mouse_x = 0;
        if(m_mouse_x >= VGA_WIDTH)  m_mouse_x = VGA_WIDTH - 1;
        if(m_mouse_y < 0) m_mouse_y = 0;
        if(m_mouse_y >= VGA_HEIGHT) m_mouse_y = VGA_HEIGHT - 1;
        render();
    }
    void mouse_left_down(){
        m_selecting = true;
        m_sel_sx = m_sel_ex = m_mouse_x;
        m_sel_sy = m_sel_ey = m_mouse_y;
        render();
    }
    void mouse_left_drag(){
        if(m_selecting){
            m_sel_ex = m_mouse_x;
            m_sel_ey = m_mouse_y;
            render();
        }
    }
    void mouse_left_up(){
        if(m_selecting){
            m_selecting = false;
            m_has_selection = (m_sel_sx != m_sel_ex) || (m_sel_sy != m_sel_ey);
            if(m_has_selection) copy_selection_to_clipboard();
            render();
        }
    }
    void mouse_click(){
        // Mouse click: if on the current input line, move cursor to mouse X
        m_has_selection = false;
        m_selecting = false;
        snap_bottom();
        // Check if click is on the last screen row (current input line)
        int input_row = (m_count - m_view);
        if(m_mouse_y >= VGA_HEIGHT - 1 || (m_at_bottom && m_mouse_y == input_row)){
            // Click is on or near the input line - set cursor relative to prompt
            int rel = m_mouse_x - m_prompt_len;
            set_cursor_col(rel);
        }
    }
    void clear_selection(){
        m_selecting = false;
        m_has_selection = false;
        render();
    }
    bool has_selection() const { return m_has_selection || m_selecting; }
    bool mouse_visible() const { return m_mouse_visible; }
    void hide_mouse(){ m_mouse_visible = false; render(); }

    void render();

private:
    int bottom_view() const { return (m_count>VGA_HEIGHT-1)?(m_count-(VGA_HEIGHT-1)):0; }
    Line& line_at(int i){ int idx=((m_head-m_count+i)%SCROLLBACK_LINES+SCROLLBACK_LINES)%SCROLLBACK_LINES; return m_lines[idx]; }
    void commit_line(){
        Line& L=m_lines[m_head];
        L.len=m_cur_len;
        for(int i=0;i<m_cur_len;i++) L.data[i]=m_cur[i];
        m_head=(m_head+1)%SCROLLBACK_LINES;
        if(m_count<SCROLLBACK_LINES) m_count++;
        m_cur_len=0;
        m_cur_pos=0;
        m_prompt_len=0;
    }
    void show_cursor(){
        outb(0x3D4,0x0A); outb(0x3D5,0x0E);
        outb(0x3D4,0x0B); outb(0x3D5,0x0F);
    }
    void hide_cursor(){ outb(0x3D4,0x0A); outb(0x3D5,0x2E); }
    void set_cursor_pos(int row,int col){
        uint16_t pos=(uint16_t)(row*VGA_WIDTH+col);
        outb(0x3D4,0x0F); outb(0x3D5,(uint8_t)(pos&0xFF));
        outb(0x3D4,0x0E); outb(0x3D5,(uint8_t)((pos>>8)&0xFF));
    }

    Line    m_lines[SCROLLBACK_LINES];
    int     m_head;
    int     m_count;
    int     m_view;
    bool    m_at_bottom;
    int     m_cur_len;
    int     m_cur_pos;       // cursor position within current line (PowerShell-style)
    int     m_prompt_len;    // length of prompt text (user input starts after this)
    char    m_cur[VGA_WIDTH];
    uint8_t m_color;

    // Mouse cursor & selection
    int     m_mouse_x = 0;
    int     m_mouse_y = 0;
    bool    m_mouse_visible = false;
    bool    m_selecting = false;
    bool    m_has_selection = false;
    int     m_sel_sx=0, m_sel_sy=0;   // selection start
    int     m_sel_ex=0, m_sel_ey=0;   // selection end

    void copy_selection_to_clipboard(){
        // Normalize: ensure start <= end
        int sx=m_sel_sx, sy=m_sel_sy, ex=m_sel_ex, ey=m_sel_ey;
        if(sy > ey || (sy==ey && sx > ex)){
            int tx=sx; sx=ex; ex=tx;
            int ty=sy; sy=ey; ey=ty;
        }
        char buf[CLIP_LEN];
        int blen = 0;
        for(int row=sy; row<=ey && blen<CLIP_LEN-1; row++){
            int col_start = (row==sy) ? sx : 0;
            int col_end   = (row==ey) ? ex : VGA_WIDTH-1;
            // Get the line text for this screen row
            int li = m_view + row;
            if(m_at_bottom && row == (m_count - m_view)){
                // Current input line
                for(int c=col_start; c<=col_end && c<m_cur_len && blen<CLIP_LEN-1; c++)
                    buf[blen++] = m_cur[c];
            } else if(li >= 0 && li < m_count){
                Line& L = line_at(li);
                for(int c=col_start; c<=col_end && c<L.len && blen<CLIP_LEN-1; c++)
                    buf[blen++] = L.data[c];
            }
            if(row < ey && blen < CLIP_LEN-1)
                buf[blen++] = '\n';
        }
        if(blen > 0){
            buf[blen] = 0;
            clipboard_set(buf, blen);
        }
    }
};

// =====================================================================
//  Keyboard  -  PS/2, Scan Code Set 1, with extended (arrow) keys
// =====================================================================
enum KbdType {
    K_NONE, K_CHAR, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_TAB,
    K_CTRL_C, K_CTRL_V, K_CTRL_L, K_CTRL_UP, K_CTRL_DOWN,
    K_PAGEUP, K_PAGEDN, K_HOME, K_END
};
struct KbdEvent { KbdType type; char ch; };

enum ScanCode : uint8_t {
    SC_BACKSPACE=0x0E, SC_TAB=0x0F, SC_ENTER=0x1C, SC_LSHIFT=0x2A,
    SC_RSHIFT=0x36, SC_CAPSLOCK=0x3A, SC_SPACE=0x39,
    SC_LCTRL=0x1D,
};
const char SC_ASCII_NORMAL[128]={
    0,0x1B,'1','2','3','4','5','6','7','8','9','0','-','=',0x08,'\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0,
};
const char SC_ASCII_SHIFT[128]={
    0,0x1B,'!','@','#','$','%','^','&','*','(',')','_','+',0x08,'\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,'7','8','9','-','4','5','6','+','1','2','3','0','.',0,0,0,0,0,
};

class Keyboard {
public:
    KbdEvent process(uint8_t sc){
        KbdEvent e; e.type=K_NONE; e.ch=0;
        if(sc==0xE0){ m_ext=true; return e; }
        bool brk = sc&0x80;
        uint8_t key= sc&0x7F;
        if(brk){
            if(key==SC_LSHIFT||key==SC_RSHIFT) m_shift=false;
            if(key==SC_LCTRL) m_ctrl=false;
            m_ext=false;
            return e;
        }
        if(m_ext){
            m_ext=false;
            if(key==0x48){
                if(m_ctrl) e.type=K_CTRL_UP;
                else       e.type=K_UP;
            }
            else if(key==0x50){
                if(m_ctrl) e.type=K_CTRL_DOWN;
                else       e.type=K_DOWN;
            }
            else if(key==0x4B) e.type=K_LEFT;
            else if(key==0x4D) e.type=K_RIGHT;
            else if(key==0x49) e.type=K_PAGEUP;
            else if(key==0x51) e.type=K_PAGEDN;
            else if(key==0x47) e.type=K_HOME;
            else if(key==0x4F) e.type=K_END;
            return e;
        }
        if(key==SC_LSHIFT||key==SC_RSHIFT){ m_shift=true; return e; }
        if(key==SC_LCTRL){ m_ctrl=true; return e; }
        if(key==SC_CAPSLOCK){ m_caps=!m_caps; return e; }

        // Ctrl+key combinations
        if(m_ctrl){
            if(key==0x2E){ e.type=K_CTRL_C; return e; }   // Ctrl+C
            if(key==0x2F){ e.type=K_CTRL_V; return e; }   // Ctrl+V
            if(key==0x26){ e.type=K_CTRL_L; return e; }   // Ctrl+L
            return e;  // swallow other Ctrl combos
        }

        if(key==SC_ENTER){ e.type=K_CHAR; e.ch='\n'; return e; }
        if(key==SC_BACKSPACE){ e.type=K_CHAR; e.ch='\b'; return e; }
        if(key==SC_TAB){ e.type=K_TAB; return e; }
        if(key==SC_SPACE){ e.type=K_CHAR; e.ch=' '; return e; }
        if(key>=128) return e;
        char c = m_shift?SC_ASCII_SHIFT[key]:SC_ASCII_NORMAL[key];
        if(c==0) return e;
        if(m_caps && c>='a'&&c<='z') c-=32;
        else if(m_caps && c>='A'&&c<='Z') c+=32;
        e.type=K_CHAR; e.ch=c; return e;
    }
private:
    bool m_shift=false, m_caps=false, m_ext=false, m_ctrl=false;
};

// =====================================================================
//  Mouse  -  PS/2 Intellimouse (4-byte packets with wheel / Z axis)
// =====================================================================
struct MouseEvent {
    int  dx;       // X movement delta
    int  dy;       // Y movement delta
    int  dz;       // wheel delta
    bool left;     // left button
    bool right;    // right button
    bool middle;   // middle button
    bool valid;    // complete packet received
};

class Mouse {
public:
    void init(){
        // Disable keyboard + auxiliary ports, then drain any stale output so
        // the probes below start from a clean state.
        outb(0x64,0xAD);
        outb(0x64,0xA7);
        drain();
        outb(0x64,0xA8);          // enable auxiliary (mouse) port
        cmd(0xFF); drain();       // reset mouse (resp: 0xFA,0xAA,0x00)
        // Probe the device id.  A standard PS/2 mouse reports 0x00 and uses
        // 3-byte relative packets.  Intellimouse-wheel devices report >=0x03
        // after the 200,100,80 sample-rate magic, and then need 4-byte packets.
        // Mismatching the packet length is exactly what made a plain 3-byte
        // touchpad desync on real hardware (the next packet's start byte got
        // swallowed as a phantom wheel value).
        uint8_t id = get_id();
        m_pkt_len = 3;
        if (id == 0x00){
            cmd(0xF3); param(200);
            cmd(0xF3); param(100);
            cmd(0xF3); param(80);
            if (get_id() >= 0x03) m_pkt_len = 4;   // wheel -> 4-byte
        }
        cmd(0xF4);                // enable streaming
        serial_puts(m_pkt_len == 4
            ? "[MOUSE] Intellimouse (4-byte packets, wheel)\n"
            : "[MOUSE] standard PS/2 mouse (3-byte packets)\n");
        outb(0x64,0xAE);          // re-enable keyboard controller
    }
    MouseEvent process(uint8_t b){
        MouseEvent ev={0,0,0,false,false,false,false};
        // The first byte of every packet has bit 3 set; bytes 1..n are raw
        // signed deltas that may LEGITIMATELY have bit 3 set (e.g. 0xFE/0xFF
        // for negative movement), so bit 3 only validates the START byte.
        // A stray byte at idle is dropped; once a packet is in progress we
        // accept the next (len-1) bytes unconditionally and resync on the next
        // genuine start byte.  (Using bit 3 mid-packet would wrongly treat
        // 0xFE/0xFF deltas as new packets -> dead mouse.)
        if (m_i == 0 && !(b & 0x08)) return ev;
        m_pkt[m_i++] = b;
        if (m_i >= m_pkt_len){
            m_i = 0;
            ev.valid  = true;
            ev.left   = m_pkt[0] & 0x01;
            ev.right  = m_pkt[0] & 0x02;
            ev.middle = m_pkt[0] & 0x04;
            ev.dx     = (int8_t)m_pkt[1];
            ev.dy     = (int8_t)m_pkt[2];
            ev.dz     = (m_pkt_len >= 4) ? (int8_t)m_pkt[3] : 0;
        }
        return ev;
    }
private:
    // Bounded waits - an absent/wedged i8042 leaves a status bit set forever,
    // so an unbounded spin would hang the boot.  These caps match kernel.cpp.
    void wait_write(){ for(int g=0; (inb(0x64)&0x02) && g<100000; g++){} }
    void wait_read(){  for(int g=0; (inb(0x64)&0x01) && g<100000; g++){} }
    void ack(){ for(int i=0;i<1000;i++){ if((inb(0x64)&0x01) && inb(0x60)==0xFA) return; } }
    void drain(){ for(int g=0; (inb(0x64)&0x01) && g<100000; g++) inb(0x60); }
    void cmd(uint8_t c){ outb(0x64,0xD4); wait_write(); outb(0x60,c); ack(); }
    void param(uint8_t p){ outb(0x64,0xD4); wait_write(); outb(0x60,p); ack(); }
    uint8_t get_id(){ cmd(0xF2); wait_read(); return inb(0x60); }
    uint8_t m_pkt[4];
    int     m_i       = 0;
    int     m_pkt_len = 3;
};

} // namespace

// Global instances
static Terminal  term;
static Keyboard  kbd;
static Mouse     mouse;

// ---- GUI functions (implemented in gui.cpp) ----
// GUI Callback interface - allows kernel to provide data to GUI
// !!! LAYOUT-CRITICAL !!!
// This declaration MUST stay byte-for-byte identical to the authoritative
// definition in gui.cpp (search for "struct GuiCallbacks" there).  gui.cpp
// memcpy's sizeof(GuiCallbacks) out of the pointer handed to
// gui_set_callbacks(), so any missing/reordered member silently shifts every
// following slot and the GUI ends up calling stack garbage.  This copy used
// to be an old 31-slot version (no mkdir/remove/rename, no http_get/session_*)
// which mis-aligned everything from `get_time` onwards and left `read_file`
// unset -- that is why the 64-bit desktop never loaded a single texture.
struct GuiCallbacks {
    uint32_t (*get_total_mem_kb)(void);
    uint32_t (*get_free_pages)(void);
    uint32_t (*get_used_pages)(void);
    uint32_t (*get_total_pages)(void);
    uint32_t (*get_heap_alloc_bytes)(void);
    uint32_t (*get_heap_free_bytes)(void);
    uint32_t (*get_heap_alloc_count)(void);
    uint32_t (*get_heap_free_count)(void);
    void     (*optimize_memory)(void);
    int      (*list_files)(int fs_type, char* buf, int bufsize);
    int      (*read_file)(int fs_type, const char* name, uint8_t* buf, int bufsize);
    int      (*write_file)(int fs_type, const char* name, const uint8_t* buf, int size);
    void     (*get_time)(int* h, int* m, int* s);
    // File-mutation (context-menu actions: new folder / delete / rename).
    int      (*mkdir)(int fs, const char* name);
    int      (*remove)(int fs, const char* name);
    int      (*rename)(int fs, const char* old_name, const char* new_name);
    const char* (*get_os_name)(void);
    bool     (*is_64bit)(void);
    // Browser callbacks
    int      (*browser_navigate)(const char* url);
    int      (*browser_status)(void);
    int      (*browser_get_page)(char* buf, int bufsize);
    void     (*browser_reset)(void);
    // Terminal command execution
    void     (*exec_command)(const char* cmd, char* output, int outsize);
    void     (*shutdown)(void);   // power off
    void     (*reboot)(void);     // restart system
    // Hardware info callbacks
    const char* (*get_cpu_vendor)(void);
    const char* (*get_disk_model)(void);
    uint32_t (*get_disk_size_mb)(void);
    int      (*get_nic_present)(void);
    int      (*get_mouse_present)(void);
    int      (*get_keyboard_present)(void);
    uint32_t (*get_pci_count)(void);
    int      (*get_bga_available)(void);
    int      (*get_vbe_mode_set)(void);
    int      (*get_cpu_64bit_capable)(void);
    // Synchronous HTTP GET for the managed Browser control.
    const char* (*http_get)(const char* url);
    // Session persistence (MKFS data disk).
    int (*session_save)(const char* name, const void* data, int size);
    int (*session_load)(const char* name, void* buf, int bufsize);
    int (*session_clear)(const char* name);
    // Sign-in bridge for the managed lock screen (must match gui.cpp).
    // login_check() is the only entry point into the account database:
    // it commits the session itself and hands back the uid, or -1.
    int         (*login_check)(const char* user, const char* pass);
    int         (*login_uid)(void);
    int         (*user_count)(void);
    const char* (*user_name)(int idx);
};

extern "C" {
    void gui_set_callbacks(const GuiCallbacks* cb);
    int  gui_init(void);
    int  gui_available(void);
    void gui_enter(void);
    int  gui_is_active(void);
    void gui_mouse_move(int dx, int dy);
    void gui_mouse_down(void);
    void gui_mouse_up(void);
    void gui_mouse_down_right(void);
    int  gui_handle_key(char ch);
    int  gui_bga_available(void);
    int  gui_vbe_mode_set_by_bios(void);
    void gui_create_window(int x, int y, int w, int h, const char* title);
    void gui_draw_text(int x, int y, const char* text);
    void gui_fill_rect(int x, int y, int w, int h, uint32_t color);
    int  gui_get_width(void);
    int  gui_get_height(void);
    void gui_render_text_mode(void);
    void gui_tick(void);
    void gui_animate_frame(void);
    void gui_exit(void);
    void gui_render(void);
    // Voice engine bridge: gui_voice_say feeds a recognised phrase to the
    // managed Voice engine; gui_voice_set toggles the master mic switch.
    void gui_voice_say(const char* phrase);
    void gui_voice_set(int on);
    // `gui <app>`: open straight into an app once the desktop is up.  The
    // 32-bit shell has always supported this; the 64-bit one used to ignore
    // its argument, so `gui browser` silently landed on a bare desktop.
    void gui_set_startup_app(int id);
    int  gui_app_id_by_name(const char* n);
}

// Global VBE flag (set from 0x500D in kernel main)
static bool g_vbe_active = false;
// Auto-launch the desktop GUI at the end of boot (default ON).  Set to 0
// (via `nogui`, or by editing this line) to stay in the text shell.
// Initialized at runtime; the freestanding .data load is unreliable here.
static int g_auto_gui;

// =====================================================================
//  File System Layer
// =====================================================================
//  Three file systems coexist on the same disk:
//
//  MKFS (Mini Kernel File System) - custom, writable, with directories
//    LBA 512:     Superblock  (magic "MKFS", file_count, free_lba)
//    LBA 513-528: File table   (16 sectors, 16 entries/sector = 256 max)
//    LBA 529-799: Data area    (271 sectors = 135 KB)
//
//  SFS (Simple File System) - compatible, read-only
//    Pre-built by Makefile from files in sfs_files/ directory.
//    LBA 800:     Superblock  (magic "SFS", file_count)
//    LBA 801-816: Directory   (16 sectors, 256 max entries)
//    LBA 817-1023:Data area   (207 sectors = 103 KB)
//
//  FAT32 (Windows-compatible) - read-only, mounted from MBR partition
//    Detected via MBR partition table at LBA 0.
//    Read BPB, follow FAT chains, parse 8.3 directory entries.
//
//  File entry (32 bytes, shared by MKFS and SFS):
//    name[20] + size(4) + start_lba(4) + type(1) + parent(2) + reserved(1)
//    type: 0=file, 1=directory
//    parent: entry index of parent directory (0xFFFF = root)

#define MKFS_SUPER_LBA    512
#define MKFS_TABLE_LBA    513
#define MKFS_TABLE_SECT   16
#define MKFS_DATA_LBA     529
#define MKFS_DATA_SECTORS 271

#define SFS_SUPER_LBA     800
#define SFS_DIR_LBA       801
#define SFS_DIR_SECT      16
#define SFS_DATA_LBA      817
// The BIOS os.img build places the same SFS image further out on the disk so
// it does not collide with the 64-bit kernel payload.  Probe both locations
// (kept in sync with kernel.cpp).  kernel64.bin grew with the vector font, so
// the BIOS build places SFS at LBA 3488 (KERNEL64_SECTORS=1440).
#define SFS_ALT_LBA       3488

// RAM-backed SFS handoff, written by the bootloader at 0x0900:
//   [0] magic == SFS_RAM_MAGIC, [1] physical base, [2] size in bytes.
// Used by the UEFI loader (bootuefi.c) and the BIOS CD path (boot_cd.asm);
// value MUST stay identical to kernel.cpp's SFS_RAM_MAGIC.
#define SFS_RAM_MAGIC     0xC0DE5A5Fu
#define SFS_HANDOFF_ADDR  0x0900u

#define FS_NAME_LEN       20
#define FS_ENTRY_SIZE     32
#define FS_ENTRY_PER_SEC  16
#define FS_IOBUF_SIZE     8192
#define FS_WRITEBUF_SIZE  8192

#define FS_TYPE_FILE      0
#define FS_TYPE_DIR       1
#define FS_ROOT_PARENT    0xFFFF

struct FileEntry {
    char     name[FS_NAME_LEN];
    uint32_t size;
    uint32_t start_lba;
    uint8_t  type;
    uint16_t parent;
    uint8_t  reserved;
};

struct Superblock {
    char     magic[4];
    uint16_t version;
    uint16_t file_count;
    uint32_t data_start;
    uint32_t free_lba;
    uint32_t total_sectors;
};

// Shared buffers
static uint8_t g_fsbuf[512];
static uint8_t g_iobuf[FS_IOBUF_SIZE];
static char    g_writebuf[FS_WRITEBUF_SIZE];
static int     g_write_len = 0;
static char    g_write_name[FS_NAME_LEN];

// Current working directory (entry index in MKFS file table, 0xFFFF=root)
static uint16_t g_cwd = FS_ROOT_PARENT;

// Shell modes
enum ShellMode { MODE_NORMAL, MODE_WRITE };
static ShellMode g_mode = MODE_NORMAL;

// =====================================================================
//  Path utilities  -  normalize \ to /, parse path components
// =====================================================================

// Convert all backslashes to forward slashes in-place
static void normalize_path(char* s){
    for(int i=0; s[i]; i++)
        if(s[i]=='\\') s[i]='/';
}

// Check if character is a path separator (/ or \)
static bool is_path_sep(char c){ return c=='/' || c=='\\'; }

// =====================================================================
//  Mkfs - Mini Kernel File System (custom, writable, with directories)
// =====================================================================
class Mkfs {
public:
    bool       mounted;
    Superblock sb;

    void init(){
        ata_read_sector(MKFS_SUPER_LBA, (uint16_t*)g_fsbuf);
        memcpy_(&sb, g_fsbuf, sizeof(sb));
        mounted = (sb.magic[0]=='M' && sb.magic[1]=='K' &&
                   sb.magic[2]=='F' && sb.magic[3]=='S');
    }

    void format(){
        sb.magic[0]='M'; sb.magic[1]='K'; sb.magic[2]='F'; sb.magic[3]='S';
        sb.version = 1;
        sb.file_count = 0;
        sb.data_start = MKFS_DATA_LBA;
        sb.free_lba = MKFS_DATA_LBA;
        sb.total_sectors = MKFS_DATA_SECTORS;
        memset_(g_fsbuf, 0, 512);
        memcpy_(g_fsbuf, &sb, sizeof(sb));
        ata_write_sector(MKFS_SUPER_LBA, (const uint16_t*)g_fsbuf);

        memset_(g_fsbuf, 0, 512);
        for (int s = 0; s < MKFS_TABLE_SECT; s++)
            ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

        mounted = true;
    }

    // List files/dirs in current directory (g_cwd)
    void ls(){
        if (!mounted) { term.write("MKFS not formatted. Use 'mkfs' first.\n"); return; }
        int count = 0;
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0 && fe->parent == g_cwd) {
                    if (fe->type == FS_TYPE_DIR) {
                        term.set_color(make_color(CYAN, BLACK));
                        term.write("  [DIR]  "); term.write(fe->name);
                        term.write("\n");
                        term.set_color(make_color(LIGHT_GREY, BLACK));
                    } else {
                        term.write("  "); term.write(fe->name);
                        term.write("  ("); term.write_dec((int)fe->size);
                        term.write(" bytes)\n");
                    }
                    count++;
                }
            }
        }
        if (count == 0) term.write("  (empty)\n");
    }

    // Find entry by name in current directory
    int find(const char* name){
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0 && fe->parent == g_cwd &&
                    strcmp_(fe->name, name) == 0)
                    return s * FS_ENTRY_PER_SEC + e;
            }
        }
        return -1;
    }

    int read(const char* name, void* buf, int bufsize){
        int idx = find(name);
        if (idx < 0) return -1;
        read_entry(idx);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        int size = (int)fe->size;
        if (size > bufsize) size = bufsize;
        int sectors = ((int)fe->size + 511) / 512;
        uint8_t* dst = (uint8_t*)buf;
        int remaining = size;
        for (int i = 0; i < sectors && remaining > 0; i++) {
            ata_read_sector(fe->start_lba + i, (uint16_t*)g_fsbuf);
            int to_copy = (remaining > 512) ? 512 : remaining;
            memcpy_(dst, g_fsbuf, to_copy);
            dst += 512;
            remaining -= to_copy;
        }
        return size;
    }

    int create(const char* name, const void* data, int size){
        if (!mounted) return -1;
        int existing = find(name);
        if (existing >= 0) remove_idx(existing);

        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] == 0) {
                    int sectors = (size + 511) / 512;
                    if (sectors == 0) sectors = 1;
                    uint32_t lba = sb.free_lba;
                    if (lba + sectors > MKFS_DATA_LBA + MKFS_DATA_SECTORS)
                        return -5;  // disk full

                    const uint8_t* src = (const uint8_t*)data;
                    for (int i = 0; i < sectors; i++) {
                        memset_(g_fsbuf, 0, 512);
                        int off = i * 512;
                        int to_copy = (size - off > 512) ? 512 : (size - off);
                        if (to_copy > 0) memcpy_(g_fsbuf, src + off, to_copy);
                        ata_write_sector(lba + i, (const uint16_t*)g_fsbuf);
                    }

                    // Re-read table sector and write entry
                    ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
                    fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                    memset_((void*)fe, 0, FS_ENTRY_SIZE);
                    int i=0; while(name[i] && i<FS_NAME_LEN-1){ fe->name[i]=name[i]; i++; }
                    fe->name[i] = 0;
                    fe->size = (uint32_t)size;
                    fe->start_lba = lba;
                    fe->type = FS_TYPE_FILE;
                    fe->parent = g_cwd;
                    fe->reserved = 0;
                    ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

                    sb.file_count++;
                    sb.free_lba += sectors;
                    flush_sb();
                    return size;
                }
            }
        }
        return -4;  // table full
    }

    // Create a directory entry
    int mkdir(const char* name){
        if (!mounted) return -1;
        int existing = find(name);
        if (existing >= 0) return -2;  // already exists

        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] == 0) {
                    memset_((void*)fe, 0, FS_ENTRY_SIZE);
                    int i=0; while(name[i] && i<FS_NAME_LEN-1){ fe->name[i]=name[i]; i++; }
                    fe->name[i] = 0;
                    fe->size = 0;
                    fe->start_lba = 0;
                    fe->type = FS_TYPE_DIR;
                    fe->parent = g_cwd;
                    fe->reserved = 0;
                    ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);

                    sb.file_count++;
                    flush_sb();
                    return 0;
                }
            }
        }
        return -4;  // table full
    }

    // Change current directory
    int cd(const char* name){
        if (!mounted) return -1;

        // Handle root
        if (strcmp_(name, "/")==0 || strcmp_(name, "\\")==0 || name[0]==0) {
            g_cwd = FS_ROOT_PARENT;
            return 0;
        }
        // Handle parent
        if (strcmp_(name, "..")==0) {
            if (g_cwd == FS_ROOT_PARENT) return 0;  // already at root
            read_entry(g_cwd);
            FileEntry* fe = (FileEntry*)g_fsbuf;
            g_cwd = fe->parent;
            return 0;
        }
        // Handle current
        if (strcmp_(name, ".")==0) return 0;

        int idx = find(name);
        if (idx < 0) return -2;  // not found
        read_entry(idx);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        if (fe->type != FS_TYPE_DIR) return -3;  // not a directory
        g_cwd = (uint16_t)idx;
        return 0;
    }

    // Print working directory path
    void pwd(){
        if (g_cwd == FS_ROOT_PARENT) {
            term.write("/\n");
            return;
        }
        // Walk up parent chain to build path
        uint16_t path[16];
        int depth = 0;
        uint16_t cur = g_cwd;
        while (cur != FS_ROOT_PARENT && depth < 16) {
            path[depth++] = cur;
            read_entry(cur);
            FileEntry* fe = (FileEntry*)g_fsbuf;
            cur = fe->parent;
        }
        term.write("/");
        for (int i = depth - 1; i >= 0; i--) {
            read_entry(path[i]);
            FileEntry* fe = (FileEntry*)g_fsbuf;
            term.write(fe->name);
            if (i > 0) term.write("/");
        }
        term.write("\n");
    }

    // Get current directory name for prompt
    void cwd_name(char* buf, int maxlen){
        if (g_cwd == FS_ROOT_PARENT) {
            buf[0] = '/'; buf[1] = 0;
            return;
        }
        read_entry(g_cwd);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        int i=0; while(fe->name[i] && i<maxlen-1){ buf[i]=fe->name[i]; i++; }
        buf[i] = 0;
    }

    int remove(const char* name){
        if (!mounted) return -1;
        int idx = find(name);
        if (idx < 0) return -2;
        // Don't allow removing non-empty directories
        read_entry(idx);
        FileEntry* fe = (FileEntry*)g_fsbuf;
        if (fe->type == FS_TYPE_DIR) {
            // Check if directory has children
            for (int s = 0; s < MKFS_TABLE_SECT; s++) {
                ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
                for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                    FileEntry* child = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                    if (child->name[0] != 0 && child->parent == (uint16_t)idx)
                        return -3;  // directory not empty
                }
            }
        }
        remove_idx(idx);
        return 0;
    }

    // Copy a file: read source, create destination with same content
    int copy(const char* src, const char* dst){
        if (!mounted) return -1;
        int ret = read(src, g_iobuf, FS_IOBUF_SIZE);
        if (ret < 0) return -2;  // source not found
        return create(dst, g_iobuf, ret);
    }

private:
    void read_entry(int idx){
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        memcpy_(g_fsbuf, g_fsbuf + e * FS_ENTRY_SIZE, FS_ENTRY_SIZE);
    }

    void remove_idx(int idx){
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        memset_((void*)fe, 0, FS_ENTRY_SIZE);
        ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);
        if (sb.file_count > 0) sb.file_count--;
        flush_sb();
    }

    void flush_sb(){
        memset_(g_fsbuf, 0, 512);
        memcpy_(g_fsbuf, &sb, sizeof(sb));
        ata_write_sector(MKFS_SUPER_LBA, (const uint16_t*)g_fsbuf);
    }
};

// =====================================================================
//  Sfs - Simple File System (compatible, read-only)
// =====================================================================

// (sfs_capture removed — debug-only, no longer needed)

class Sfs {
public:
    bool       mounted;
    uint32_t   base;      // LBA where the superblock was actually found
    int32_t    delta;     // base - SFS_SUPER_LBA (image may be relocated on disk)
    Superblock sb;

    // UEFI/AHCI RAM-SFS support: bootuefi.c streams the SFS image into RAM
    // before ExitBootServices (legacy IDE PIO ata_read_sector returns garbage
    // under q35/AHCI, so the disk probe can never find the superblock).  When
    // ram_mode is set the image is read straight from RAM, exactly like the
    // 32-bit CD-boot path.
    bool       ram_mode;       // true => image is in RAM at ram_base
    uint32_t   ram_base;       // physical address of the SFS image in RAM
    uint32_t   ram_size;       // size of the SFS image in bytes

    void set_ram(uint32_t base, uint32_t size){
        ram_mode = true; ram_base = base; ram_size = size;
    }

    // Unified sector read: from RAM when ram_mode, else from the ATA disk.
    // lba is the canonical SFS LBA (superblock=800, dir=801+, data=817+); in
    // RAM the image is laid out from byte 0, so offset = (lba-800)*512.
    void rd(uint32_t lba, uint16_t* buf){
        if (ram_mode) {
            const uint8_t* src = (const uint8_t*)ram_base
                                + (int32_t)((int32_t)lba - (int32_t)SFS_SUPER_LBA) * 512;
            memcpy_(buf, src, 512);
        } else {
            ata_read_sector(lba, buf);
        }
    }

    // Mirrors kernel.cpp's Sfs::init.  The SFS image always uses the canonical
    // 800/801/817 layout internally, but the build drops it at different disk
    // offsets (UEFI image: LBA 800, BIOS os.img: LBA 3328) so it never collides
    // with the kernel64 payload.  Probe both and carry a delta, otherwise
    // `switch64` lands in a kernel that reports "SFS: not found" on the very
    // same disk the 32-bit kernel mounted fine.
    void init(){
        // RAM-backed SFS (UEFI path): bootuefi.c staged the whole image in
        // RAM before ExitBootServices, so read the superblock straight from
        // there.  Legacy IDE PIO ata_read_sector returns garbage under q35/AHCI,
        // which is exactly why the 64-bit kernel could never find "SFS\0".
        if (ram_mode) {
            rd(SFS_SUPER_LBA, (uint16_t*)g_fsbuf);
            if (g_fsbuf[0]=='S' && g_fsbuf[1]=='F' && g_fsbuf[2]=='S' && g_fsbuf[3]==0) {
                memcpy_(&sb, g_fsbuf, sizeof(sb));
                base    = SFS_SUPER_LBA;
                delta   = 0;
                mounted = true;
                return;
            }
            // RAM handoff claimed (0x0900 == SFS_RAM_MAGIC) but the staged
            // image has no valid superblock -- this happens on a BIOS disk
            // boot where low memory at 0x0900 coincidentally matches the
            // magic, or a stale handoff from an earlier boot.  Don't trust it:
            // clear ram_mode and fall through to the real ATA disk probe
            // below (which works now that the 64-bit ata_read_sector is fixed).
            ram_mode = false;
        }
        mounted = false; base = SFS_SUPER_LBA; delta = 0;
        // Probe the two candidate superblock locations using the #define values
        // *directly as immediates* (NOT a function-local static const array).
        // In the relocated 64-bit flat binary a rodata array can resolve to the
        // link address rather than the load address, yielding 0 and making every
        // probe read LBA0.  g_fsbuf (BSS) and instruction immediates are fine.
        //   SFS_SUPER_LBA (800)  : canonical in-image SFS layout
        //   SFS_ALT_LBA   (3488) : relocated SFS on the BIOS disk image
        // ---- Probe 1: SFS_SUPER_LBA (800) ----
        ata_read_sector(SFS_SUPER_LBA, (uint16_t*)g_fsbuf);
        if (g_fsbuf[0]=='S' && g_fsbuf[1]=='F' && g_fsbuf[2]=='S' && g_fsbuf[3]==0) {
            memcpy_(&sb, g_fsbuf, sizeof(sb));
            base    = SFS_SUPER_LBA;
            delta   = 0;
            mounted = true;
            return;
        }
        // ---- Probe 2: SFS_ALT_LBA (3488) ----
        ata_read_sector(SFS_ALT_LBA, (uint16_t*)g_fsbuf);
        if (g_fsbuf[0]=='S' && g_fsbuf[1]=='F' && g_fsbuf[2]=='S' && g_fsbuf[3]==0) {
            memcpy_(&sb, g_fsbuf, sizeof(sb));
            base    = SFS_ALT_LBA;
            delta   = (int32_t)SFS_ALT_LBA - (int32_t)SFS_SUPER_LBA;
            mounted = true;
            return;
        }
    }

    inline uint32_t dir_lba(int s) const {
        return (uint32_t)((int32_t)SFS_DIR_LBA + delta) + (uint32_t)s;
    }
    inline uint32_t data_lba(uint32_t l) const { return (uint32_t)((int32_t)l + delta); }

    void ls(){
        if (!mounted) { term.write("SFS not found on disk.\n"); return; }
        int count = 0;
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            rd(dir_lba(s), (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0) {
                    term.write("  "); term.write(fe->name);
                    term.write("  ("); term.write_dec((int)fe->size);
                    term.write(" bytes)\n");
                    count++;
                }
            }
        }
        if (count == 0) term.write("  (empty)\n");
    }

    int find(const char* name){
        if (!mounted) return -1;
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            rd(dir_lba(s), (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0) {
                    if (strcmp_(fe->name, name) == 0)
                        return s * FS_ENTRY_PER_SEC + e;
                }
            }
        }
        return -1;
    }

    int read(const char* name, void* buf, int bufsize){
        int idx = find(name);
        if (idx < 0) return -1;
        int s = idx / FS_ENTRY_PER_SEC;
        int e = idx % FS_ENTRY_PER_SEC;
        // MUST go through rd(): in RAM mode (UEFI) a raw ata_read_sector here
        // returns zeros, so fent.start_lba/size come out garbage and every
        // file read silently produces an empty buffer.
        rd(dir_lba(s), (uint16_t*)g_fsbuf);
        // IMPORTANT: private copy - the loop below reloads g_fsbuf with file
        // data, so a pointer into g_fsbuf would decay into garbage after the
        // first sector and every following LBA would be random.
        FileEntry fent;
        memcpy_(&fent, g_fsbuf + e * FS_ENTRY_SIZE, sizeof(FileEntry));
        int size = (int)fent.size;
        if (size > bufsize) size = bufsize;
        int sectors = (size + 511) / 512;
        uint8_t* dst = (uint8_t*)buf;
        int remaining = size;
        for (int i = 0; i < sectors && remaining > 0; i++) {
            rd(data_lba(fent.start_lba + (uint32_t)i), (uint16_t*)g_fsbuf);
            int to_copy = (remaining > 512) ? 512 : remaining;
            memcpy_(dst, g_fsbuf, to_copy);
            dst += to_copy;   // NOT 512: the tail sector is short
            remaining -= to_copy;
        }
        return size;
    }
};

// =====================================================================
//  MBR partition table reader
// =====================================================================
//  MBR is at LBA 0. Partition table starts at offset 446,
//  4 entries of 16 bytes each. Signature 0x55AA at offset 510.

struct MbrPartition {
    uint8_t  boot_flag;       // 0x80 = bootable
    uint8_t  start_chs[3];
    uint8_t  type;            // partition type
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t total_sectors;
};

static const char* part_type_name(uint8_t type){
    switch(type){
        case 0x00: return "Empty";
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Ext LBA";
        case 0x82: return "Linux Swap";
        case 0x83: return "Linux";
        case 0x8E: return "Linux LVM";
        case 0xA5: return "FreeBSD";
        case 0xEE: return "GPT Protective";
        case 0xEF: return "EFI System";
        default:   return "Unknown";
    }
}

static bool is_fat_type(uint8_t type){
    return type==0x01 || type==0x04 || type==0x06 ||
           type==0x0B || type==0x0C || type==0x0E;
}

// Read MBR partition table and display it
// Returns partition start LBA for a given partition index (1-4), or 0 if not found
static uint32_t read_mbr_partition(int part_num, MbrPartition* out){
    ata_read_sector(0, (uint16_t*)g_fsbuf);
    uint8_t* mbr = g_fsbuf;

    // Check MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return 0;

    // Partition table at offset 446, 4 entries x 16 bytes
    MbrPartition* parts = (MbrPartition*)(mbr + 446);
    for (int i = 0; i < 4; i++) {
        if (i + 1 == part_num) {
            *out = parts[i];
            return parts[i].start_lba;
        }
    }
    return 0;
}

// =====================================================================
//  Fat32 - Windows-compatible FAT32 file system reader (read-only)
// =====================================================================
class Fat32 {
public:
    bool      mounted;
    uint32_t  part_start;       // partition start LBA
    uint16_t  bytes_per_sector;
    uint8_t   sectors_per_cluster;
    uint16_t  reserved_sectors;
    uint8_t   num_fats;
    uint32_t  fat_size;         // sectors per FAT
    uint32_t  root_cluster;
    uint32_t  data_start;       // first data sector LBA
    uint32_t  total_sectors;

    void init(){ mounted = false; }

    bool mount(uint32_t start_lba){
        part_start = start_lba;
        ata_read_sector(start_lba, (uint16_t*)g_fsbuf);
        uint8_t* bpb = g_fsbuf;

        // Check FAT signature
        if (bpb[510] != 0x55 || bpb[511] != 0xAA)
            return false;

        bytes_per_sector   = *(uint16_t*)(bpb + 11);
        sectors_per_cluster = bpb[13];
        reserved_sectors    = *(uint16_t*)(bpb + 14);
        num_fats            = bpb[16];
        uint16_t root_entries = *(uint16_t*)(bpb + 17);
        uint16_t fat_size_16  = *(uint16_t*)(bpb + 22);
        uint32_t total_sec_32 = *(uint32_t*)(bpb + 32);

        // FAT32 has root_entries == 0 and fat_size_16 == 0
        if (root_entries != 0) return false;

        fat_size = fat_size_16 ? fat_size_16 : *(uint32_t*)(bpb + 36);
        root_cluster = *(uint32_t*)(bpb + 44);
        total_sectors = total_sec_32 ? total_sec_32 : *(uint16_t*)(bpb + 19);

        data_start = part_start + reserved_sectors + num_fats * fat_size;

        mounted = (bytes_per_sector == 512 && sectors_per_cluster > 0);
        return mounted;
    }

    void info(){
        if (!mounted) { term.write("No FAT32 partition mounted.\n"); return; }
        term.write("FAT32 partition info:\n");
        term.write("  Start LBA:       "); term.write_dec((int)part_start); term.write("\n");
        term.write("  Bytes/sector:    "); term.write_dec((int)bytes_per_sector); term.write("\n");
        term.write("  Sectors/cluster: "); term.write_dec((int)sectors_per_cluster); term.write("\n");
        term.write("  Reserved:        "); term.write_dec((int)reserved_sectors); term.write(" sectors\n");
        term.write("  FATs:            "); term.write_dec((int)num_fats); term.write("\n");
        term.write("  FAT size:        "); term.write_dec((int)fat_size); term.write(" sectors\n");
        term.write("  Root cluster:    "); term.write_dec((int)root_cluster); term.write("\n");
        term.write("  Data start:      LBA "); term.write_dec((int)data_start); term.write("\n");
        term.write("  Total sectors:   "); term.write_dec((int)total_sectors); term.write("\n");

        // Volume label (offset 71 in boot sector, 11 chars)
        ata_read_sector(part_start, (uint16_t*)g_fsbuf);
        term.write("  Volume label:    ");
        for (int i = 0; i < 11; i++) {
            char c = (char)g_fsbuf[71 + i];
            if (c >= 0x20) term.put_char(c);
        }
        term.put_char('\n');
    }

    // List files in root directory
    void ls(){
        if (!mounted) { term.write("No FAT32 partition mounted.\n"); return; }
        int count = 0;
        uint32_t cluster = root_cluster;

        for (int cl = 0; cl < 32; cl++) {  // limit clusters
            if (cluster < 2 || cluster >= 0x0FFFFFF8) break;

            uint32_t lba = data_start + (cluster - 2) * sectors_per_cluster;
            // Read cluster into g_iobuf
            for (int s = 0; s < sectors_per_cluster && s * 512 < FS_IOBUF_SIZE; s++)
                ata_read_sector(lba + s, (uint16_t*)(g_iobuf + s * 512));

            // Parse directory entries (32 bytes each)
            int entries = (sectors_per_cluster * 512) / 32;
            if (entries > FS_IOBUF_SIZE / 32) entries = FS_IOBUF_SIZE / 32;

            for (int e = 0; e < entries; e++) {
                uint8_t* de = g_iobuf + e * 32;
                if (de[0] == 0x00) goto done;       // end of directory
                if (de[0] == 0xE5) continue;          // deleted entry
                uint8_t attr = de[11];
                if (attr & 0x08) continue;            // volume label
                if (attr & 0x0F) continue;            // long filename entry

                bool is_dir = (attr & 0x10) != 0;

                if (is_dir) {
                    term.set_color(make_color(CYAN, BLACK));
                    term.write("  [DIR]  ");
                } else {
                    term.write("  ");
                }

                // Print 8.3 name
                for (int i = 0; i < 8; i++) {
                    if (de[i] != ' ') term.put_char(de[i]);
                }
                if (de[8] != ' ') {
                    term.put_char('.');
                    for (int i = 8; i < 11; i++) {
                        if (de[i] != ' ') term.put_char(de[i]);
                    }
                }

                if (!is_dir) {
                    uint32_t fsize = *(uint32_t*)(de + 28);
                    term.write("  ("); term.write_dec((int)fsize); term.write(" bytes)");
                }
                term.write("\n");
                if (is_dir) term.set_color(make_color(LIGHT_GREY, BLACK));
                count++;
            }

            // Follow FAT chain
            cluster = next_cluster(cluster);
        }
    done:
        if (count == 0) term.write("  (empty)\n");
    }

private:
    uint32_t next_cluster(uint32_t cluster){
        uint32_t fat_offset = cluster * 4;
        uint32_t fat_sector = part_start + reserved_sectors + fat_offset / 512;
        ata_read_sector(fat_sector, (uint16_t*)g_fsbuf);
        uint32_t entry = *(uint32_t*)(g_fsbuf + (fat_offset % 512));
        return entry & 0x0FFFFFFF;
    }
};

static Mkfs   mkfs;
static Sfs    sfs;
static Fat32  fat32;

// =====================================================================
//  Memory Management: Physical (PMM) + Virtual (VMM) + Heap
// =====================================================================
//  PMM:  Bitmap page-frame allocator for 4 KiB pages (1 MB+).
//  VMM:  x86 32-bit 2-level paging (4 MB identity-map + 4 KiB map).
//  Heap: Linked-list first-fit allocator on a 1 MiB region at 2 MB.
// ---------------------------------------------------------------------

// --- Constants ---
constexpr uint32_t PAGE_SIZE       = 4096;
constexpr uint32_t PAGE_SHIFT      = 12;
constexpr uint32_t PMM_BASE_ADDR   = 0x100000;     // 1 MiB – managed start
// Physical memory is managed up to 1 GiB: the VMM identity map ends there,
// so higher physical pages could never be touched anyway.  Keeping the
// bitmap at 1 MiB of .bss also keeps it clear of the heap that starts right
// after .bss -- a full 4 GiB bitmap (4 MiB) overlaps the heap region and the
// heap initializer corrupts the allocation table (470 MiB model big_alloc
// then fails).  The 4 GiB comment below is stale.
constexpr uint32_t PMM_MAX_PAGES   = 1u * 1024 * 1024 * 1024 / PAGE_SIZE; // 262,144 (1 GiB)
// .bss ends at 0x168000 + 0xD2CF8 = 0x23ACF8; a heap starting at 0x200000
// would sit ON the .bss (guilib globals, clr heap, backbuffer kmalloc all
// corrupt each other -> blank desktop).  0x400000 leaves room for the 1 MiB
// PMM bitmap that now follows .bss (ends ~0x33ACF8) and matches the 32-bit
// kernel's heap convention.
constexpr uint32_t HEAP_START      = 0x500000;     // 5 MiB (past .bss end 0x4354A0 + PMM bitmap)
constexpr uint32_t HEAP_SIZE       = 0x1000000;    // 16 MiB (expanded for AI engine)
constexpr uint32_t HEAP_END        = HEAP_START + HEAP_SIZE;

// Boot scratch region owned by the 32-bit kernel's linker.ld (.lmboot block).
// It holds the 32-bit boot stack, the long-mode page tables that CR3 STILL
// POINTS AT while this kernel runs, and the 64-bit boot stack we are executing
// on right now.  pmm_alloc_page() handing any of it out silently corrupts the
// live page tables (random #PF / triple fault) or our own stack.
//
// COUPLING: keep in sync with the `. = 0x1800000;` .lmboot block in linker.ld.
// That region is 0x28000 bytes; 0x30000 is reserved here for slack.
constexpr uint32_t LMBOOT_BASE     = 0x1800000;    // 24 MiB
constexpr uint32_t LMBOOT_SIZE     = 0x0030000;    // 192 KiB

// End of this kernel's .bss (linker64.ld).  .bss is NOBITS so it is NOT part
// of kernel64.bin, which means nothing else knows how far the kernel really
// reaches -- the PMM has to ask the linker.
extern "C" char __bss_end[];

// Page-table / PDE flags
constexpr uint32_t PG_PRESENT  = 0x001;
constexpr uint32_t PG_RW       = 0x002;
constexpr uint32_t PG_USER     = 0x004;
constexpr uint32_t PG_PSE      = 0x080;   // 4 MiB page (PS bit in PDE)

// --- CRx / MSR helpers (64-bit) ---
static inline uint64_t read_cr0(){ uint64_t v; __asm__ __volatile__("mov %%cr0,%0":"=r"(v)); return v; }
static inline uint64_t read_cr3(){ uint64_t v; __asm__ __volatile__("mov %%cr3,%0":"=r"(v)); return v; }
static inline uint64_t read_cr4(){ uint64_t v; __asm__ __volatile__("mov %%cr4,%0":"=r"(v)); return v; }
static inline void write_cr0(uint64_t v){ __asm__ __volatile__("mov %0,%%cr0"::"r"(v)); }
static inline void write_cr3(uint64_t v){ __asm__ __volatile__("mov %0,%%cr3"::"r"(v)); }
static inline void write_cr4(uint64_t v){ __asm__ __volatile__("mov %0,%%cr4"::"r"(v)); }
static inline void invlpg(uint64_t addr){ __asm__ __volatile__("invlpg (%0)"::"r"(addr)); }

// =====================================================================
//  Physical Memory Manager (PMM)
// =====================================================================
static uint32_t pmm_bitmap[PMM_MAX_PAGES];   // 128 KiB in .bss, identity-mapped
static uint32_t  pmm_total_pages = 0;
static uint32_t  pmm_free_pages  = 0;
static uint32_t  pmm_used_pages  = 0;
static uint32_t  pmm_mem_kb      = 0;

// Detect total memory via CMOS RTC (ports 0x70/0x71)
static uint32_t detect_memory_kb(){
    // Offset 0x17/0x18 = extended memory in KiB (15-65 MiB range)
    outb(0x70, 0x17); uint8_t lo = inb(0x71);
    outb(0x70, 0x18); uint8_t hi = inb(0x71);
    uint32_t ext = ((uint32_t)hi << 8) | lo;
    if (ext > 0 && ext < 65535) return 1024 + ext;
    // Fallback: offset 0x30/0x31
    outb(0x70, 0x30); lo = inb(0x71);
    outb(0x70, 0x31); hi = inb(0x71);
    ext = ((uint32_t)hi << 8) | lo;
    if (ext > 0) return 1024 + ext;
    return 64 * 1024;  // assume 64 MiB
}

// Mark [start, end) as used.  Idempotent: pages that are already marked are
// skipped, so overlapping reservations cannot corrupt the free/used counters.
static uint32_t pmm_reserve_range(uint32_t start, uint32_t end){
    if (start < PMM_BASE_ADDR) start = PMM_BASE_ADDR;
    if (end <= start) return 0;
    uint32_t first = (start - PMM_BASE_ADDR) / PAGE_SIZE;
    uint32_t last  = (end - PMM_BASE_ADDR + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t n = 0;
    for (uint32_t i = first; i < last && i < pmm_total_pages; i++){
        if (!(pmm_bitmap[i / 32] & (1u << (i % 32)))){
            pmm_bitmap[i / 32] |= (1u << (i % 32));
            pmm_free_pages--;
            pmm_used_pages++;
            n++;
        }
    }
    return n;
}

static void pmm_init(){
    // Manage the full low 4 GiB the bootloader/QEMU provides. (CMOS detection
    // caps far below this, so size the bitmap directly.)
    pmm_mem_kb = PMM_MAX_PAGES * PAGE_SIZE / 1024;
    pmm_total_pages = PMM_MAX_PAGES;

    // Clear bitmap (all free)
    uint32_t bm_words = (pmm_total_pages + 31) / 32;
    for (uint32_t i = 0; i < bm_words; i++) pmm_bitmap[i] = 0;

    pmm_free_pages = pmm_total_pages;
    pmm_used_pages = 0;

    // Mark the whole kernel image as used: 1 MiB .. end of .bss.
    //
    // This used to be a flat "64 pages = 256 KiB", which was wrong by an
    // order of magnitude: kernel64.bin alone is ~620 KiB (0x100000..0x19B2F0)
    // and .bss runs on to ~0x3A2C60.  Pages 64.. were therefore advertised as
    // free while holding live kernel code and globals, so the very first
    // pmm_alloc_page() returned 0x140000 -- a page inside our own .text.
    // vmm's page-table allocations (pmm_alloc_page at map time) were writing
    // PTEs straight over the running kernel.
    pmm_reserve_range(PMM_BASE_ADDR, (uint32_t)(uintptr_t)__bss_end);

    // Mark the kernel heap as used
    pmm_reserve_range(HEAP_START, HEAP_END);

    // Reserve the boot scratch region: the long-mode page tables CR3 is still
    // walking, plus the 64-bit boot stack this code is running on.  See the
    // LMBOOT_BASE comment above.
    pmm_reserve_range(LMBOOT_BASE, LMBOOT_BASE + LMBOOT_SIZE);

    // Reserve the RAM-staged SFS image (UEFI path).  Normally it sits at
    // 0x50000000, i.e. above this bitmap's 1 GiB ceiling, so nothing happens.
    // But when the loader had to fall back to an arbitrary <4 GiB buffer the
    // range CAN land inside the managed window, and handing those pages out
    // would shred the filesystem under our feet.
    {
        const volatile uint32_t* hs =
            (const volatile uint32_t*)(uintptr_t)SFS_HANDOFF_ADDR;
        if (hs[0] == SFS_RAM_MAGIC && hs[1] >= PMM_BASE_ADDR && hs[2] != 0) {
            uint32_t first = (hs[1] - PMM_BASE_ADDR) / PAGE_SIZE;
            uint32_t npg   = (hs[2] + PAGE_SIZE - 1) / PAGE_SIZE;
            uint32_t marked = 0;
            for (uint32_t i = 0; i < npg; i++) {
                uint32_t idx = first + i;
                if (idx >= pmm_total_pages) break;
                if (!(pmm_bitmap[idx / 32] & (1u << (idx % 32)))) {
                    pmm_bitmap[idx / 32] |= (1u << (idx % 32));
                    pmm_free_pages--;
                    pmm_used_pages++;
                    marked++;
                }
            }
            if (marked) {
                serial_puts("[PMM] RAM-SFS reserved pages=");
                serial_hex(marked);
                serial_puts("\n");
            }
        }
    }

    serial_puts("[PMM] Initialised: image 00100000..");
    serial_hex((uint32_t)(uintptr_t)__bss_end);
    serial_puts(" heap ");
    serial_hex(HEAP_START);
    serial_puts("..");
    serial_hex(HEAP_END);
    serial_puts(" lmboot ");
    serial_hex(LMBOOT_BASE);
    serial_puts("..");
    serial_hex(LMBOOT_BASE + LMBOOT_SIZE);
    serial_puts("\n");

    // Loud guard: if the heap ever overlaps the boot scratch region the
    // symptom is a random late #PF that looks nothing like a heap bug.
    if (HEAP_END > LMBOOT_BASE) {
        serial_puts("[PMM] FATAL: heap end overlaps LMBOOT_BASE -- move .lmboot in linker.ld\n");
    }
    if ((uint32_t)(uintptr_t)__bss_end > HEAP_START) {
        serial_puts("[PMM] FATAL: .bss end overlaps HEAP_START -- raise HEAP_START\n");
    }
}

static uint32_t pmm_alloc_page(){
    for (uint32_t i = 0; i < pmm_total_pages; i++){
        uint32_t w = i / 32, b = i % 32;
        if (!(pmm_bitmap[w] & (1u << b))){
            pmm_bitmap[w] |= (1u << b);
            pmm_free_pages--;
            pmm_used_pages++;
            return PMM_BASE_ADDR + i * PAGE_SIZE;
        }
    }
    return 0;  // OOM
}

static void pmm_free_page(uint32_t phys){
    if (phys < PMM_BASE_ADDR) return;
    uint32_t i = (phys - PMM_BASE_ADDR) / PAGE_SIZE;
    if (i >= pmm_total_pages) return;
    uint32_t w = i / 32, b = i % 32;
    if (pmm_bitmap[w] & (1u << b)){
        pmm_bitmap[w] &= ~(1u << b);
        pmm_free_pages++;
        pmm_used_pages--;
    }
}

static uint32_t pmm_alloc_range(uint32_t count){
    // Contiguous allocation via bitmap run scan (O(N)).
    if (count == 0 || count > pmm_total_pages) return 0;
    uint32_t i = 0;
    const uint32_t total = pmm_total_pages;
    while (i + count <= total){
        bool ok = true;
        for (uint32_t j = 0; j < count; j++){
            if (pmm_bitmap[(i+j)/32] & (1u << ((i+j)%32))){
                // Hit a used bit; jump to the start of the next bitmap word.
                i = ((i + j) / 32 + 1) * 32;
                ok = false;
                break;
            }
        }
        if (ok){
            for (uint32_t j = 0; j < count; j++){
                pmm_bitmap[(i+j)/32] |= (1u << ((i+j)%32));
                pmm_free_pages--;
                pmm_used_pages++;
            }
            return PMM_BASE_ADDR + i * PAGE_SIZE;
        }
    }
    return 0;
}

// GB-scale contiguous allocation for model weights. In long mode the page
// tables identity-map physical==virtual, so the returned pointer is directly
// usable. Returns nullptr on OOM.
extern "C" void* big_alloc(uint32_t bytes){
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t phys = pmm_alloc_range(pages);
    if (!phys) return nullptr;
    return (void*)(uintptr_t)phys;
}
extern "C" void big_free(void* p, uint32_t bytes){
    if (!p) return;
    uint32_t phys = (uint32_t)(uintptr_t)p;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t j = 0; j < pages; j++) pmm_free_page(phys + j * PAGE_SIZE);
}

// =====================================================================
//  Virtual Memory Manager (VMM)
// =====================================================================
//  Page directory: linker-reserved 4 KiB-aligned array (never a fixed
//  absolute address -- .bss now overlaps the old hard-coded 0x70000).
alignas(4096) static uint32_t g_page_directory_store[1024];
static uint32_t* const page_directory = g_page_directory_store;

static bool vmm_paging_on  = false;
static bool vmm_long_mode  = false;
static bool vmm_our_paging = false;  // true only when WE set up paging (BIOS path)

static bool vmm_check_long_mode(){
    // EFER MSR (0xC0000080), bit 8 = LME (in EAX, not EDX)
    uint32_t eax, edx;
    __asm__ __volatile__("rdmsr":"=a"(eax),"=d"(edx):"c"(0xC0000080u));
    return (eax & (1u << 8)) != 0;
}

static void vmm_init(){
    vmm_long_mode = vmm_check_long_mode();
    vmm_paging_on = (read_cr0() & 0x80000000ull) != 0;

    if (vmm_long_mode){
        // 64-bit kernel: we are in long mode with 4-level identity-mapped paging
        // set up by switch32to64.asm (BIOS) or UEFI firmware.
        serial_puts("[VMM] Long mode – 4-level paging in use\n");
        return;
    }
    if (vmm_paging_on){
        // Firmware's 32-bit paging is active. Use the existing identity mapping.
        serial_puts("[VMM] Firmware paging active (not our own)\n");
        return;
    }

    // ---- 32-bit BIOS path: set up 32-bit paging with 4 MiB PSE ----
    // (only reached if somehow not in long mode and no firmware paging)
    serial_puts("[VMM] Setting up 32-bit paging (PSE)...\n");

    // Clear page directory
    for (int i = 0; i < 1024; i++) page_directory[i] = 0;

    // Identity-map first 32 MiB with 4 MiB pages (covers kernel + 16 MiB heap)
    for (int i = 0; i < 8; i++)
        page_directory[i] = (i * 0x400000u) | PG_PRESENT | PG_RW | PG_PSE;

    // Enable PSE (CR4.PSE = bit 4)
    uint64_t cr4 = read_cr4();
    write_cr4(cr4 | 0x10);

    // Load page directory into CR3
    write_cr3((uint64_t)(uintptr_t)page_directory);

    // Enable paging (CR0.PG = bit 31)
    uint64_t cr0 = read_cr0();
    write_cr0(cr0 | 0x80000000ull);

    vmm_paging_on  = true;
    vmm_our_paging = true;
    serial_puts("[VMM] Paging enabled (32 MiB identity-mapped, 4 MiB PSE)\n");
}

// Split a 4 MiB PDE into 1024 × 4 KiB pages (transparent: all identity-mapped)
static bool vmm_split_4mb(uint32_t pd_idx){
    uint32_t pde = page_directory[pd_idx];
    if (!(pde & PG_PSE)) return true;          // already a PT
    if (!(pde & PG_PRESENT)) return false;

    // Allocate a 4 KiB page table from PMM (identity-mapped, so accessible)
    uint32_t pt_phys = pmm_alloc_page();
    if (pt_phys == 0) return false;

    uint32_t* pt = (uint32_t*)pt_phys;
    uint32_t base = pd_idx << 22;               // 4 MiB base physical addr
    for (int i = 0; i < 1024; i++)
        pt[i] = (base + i * PAGE_SIZE) | PG_PRESENT | PG_RW;

    page_directory[pd_idx] = pt_phys | PG_PRESENT | PG_RW;
    return true;
}

// Map a 4 KiB virtual page to a physical page
static bool vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags){
    if (!vmm_our_paging) return false;  // can't modify firmware page tables

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    // Ensure we have a 4 KiB page table (split if 4 MiB PSE entry)
    uint32_t pde = page_directory[pd_idx];
    if (pde & PG_PSE){
        if (!vmm_split_4mb(pd_idx)) return false;
        pde = page_directory[pd_idx];
    }
    if (!(pde & PG_PRESENT)){
        uint32_t pt_phys = pmm_alloc_page();
        if (pt_phys == 0) return false;
        memset_((void*)pt_phys, 0, PAGE_SIZE);
        page_directory[pd_idx] = pt_phys | PG_PRESENT | PG_RW;
        pde = page_directory[pd_idx];
    }

    uint32_t* pt = (uint32_t*)(pde & 0xFFFFF000u);
    pt[pt_idx] = (phys & 0xFFFFF000u) | (flags & 0xFFF) | PG_PRESENT;
    invlpg(virt);
    return true;
}

// Translate virtual → physical (returns 0 if not mapped)
static uint32_t vmm_get_phys(uint32_t virt){
    if (!vmm_paging_on) return virt;  // identity (no paging)
    if (vmm_long_mode)  return virt;  // long mode: identity-mapped

    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;
    uint32_t pde = page_directory[pd_idx];

    if (!(pde & PG_PRESENT)) return 0;
    if (pde & PG_PSE){
        // 4 MiB page – offset within the 4 MiB block
        return (pde & 0xFFC00000u) + (virt & 0x3FFFFF);
    }
    uint32_t* pt = (uint32_t*)(pde & 0xFFFFF000u);
    uint32_t pte = pt[pt_idx];
    if (!(pte & PG_PRESENT)) return 0;
    return (pte & 0xFFFFF000u) + (virt & 0xFFF);
}

// =====================================================================
//  Kernel Heap (kmalloc / kfree)
// =====================================================================
//  Simple linked-list first-fit allocator on a fixed 1 MiB region.
struct HeapBlock {
    uint32_t magic;       // 0xDEADBEEF = allocated, 0xFEEDFACE = free
    uint32_t size;        // user data size (excludes header, includes padding)
    HeapBlock* next;
    HeapBlock* prev;
};

constexpr uint32_t HEAP_MAGIC_ALLOC = 0xDEADBEEFu;
constexpr uint32_t HEAP_MAGIC_FREE  = 0xFEEDFACEu;
constexpr uint32_t HEAP_ALIGN       = 8;

static HeapBlock* heap_head = nullptr;
static uint32_t   heap_alloc_count   = 0;
static uint32_t   heap_free_count    = 0;
static uint32_t   heap_bytes_alloc   = 0;
static uint32_t   heap_bytes_freed   = 0;

static void heap_init(){
    // The 16 MiB region at HEAP_START is identity-mapped (first 32 MiB)
    heap_head = (HeapBlock*)HEAP_START;
    heap_head->magic = HEAP_MAGIC_FREE;
    heap_head->size  = HEAP_SIZE - sizeof(HeapBlock);
    heap_head->next  = nullptr;
    heap_head->prev  = nullptr;
    heap_alloc_count = 0;
    heap_free_count  = 0;
    heap_bytes_alloc = 0;
    heap_bytes_freed = 0;
    serial_puts("[HEAP] Initialised: 16 MiB at 0x500000\n");
}

extern "C" void* kmalloc(uint32_t size){
    if (size == 0) return nullptr;

    // Align to 8 bytes
    size = (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);

    HeapBlock* blk = heap_head;
    while (blk){
        if (blk->magic == HEAP_MAGIC_FREE && blk->size >= size + sizeof(HeapBlock)){
            // Split if there's room for another block
            if (blk->size >= size + sizeof(HeapBlock) + HEAP_ALIGN){
                HeapBlock* new_blk = (HeapBlock*)((uint8_t*)blk + sizeof(HeapBlock) + size);
                new_blk->magic = HEAP_MAGIC_FREE;
                new_blk->size  = blk->size - size - sizeof(HeapBlock);
                new_blk->next  = blk->next;
                new_blk->prev  = blk;
                if (blk->next) blk->next->prev = new_blk;
                blk->next = new_blk;
                blk->size = size;
            }
            blk->magic = HEAP_MAGIC_ALLOC;
            heap_alloc_count++;
            heap_bytes_alloc += size;
            return (uint8_t*)blk + sizeof(HeapBlock);
        }
        blk = blk->next;
    }
    return nullptr;  // OOM
}

extern "C" void kfree(void* ptr){
    if (!ptr) return;
    HeapBlock* blk = (HeapBlock*)((uint8_t*)ptr - sizeof(HeapBlock));

    if (blk->magic != HEAP_MAGIC_ALLOC) return;  // invalid or double-free

    blk->magic = HEAP_MAGIC_FREE;
    heap_free_count++;
    heap_bytes_freed += blk->size;

    // Coalesce with next block if free
    if (blk->next && blk->next->magic == HEAP_MAGIC_FREE){
        HeapBlock* nb = blk->next;
        blk->size += sizeof(HeapBlock) + nb->size;
        blk->next = nb->next;
        if (nb->next) nb->next->prev = blk;
    }
    // Coalesce with previous block if free
    if (blk->prev && blk->prev->magic == HEAP_MAGIC_FREE){
        HeapBlock* pb = blk->prev;
        pb->size += sizeof(HeapBlock) + blk->size;
        pb->next = blk->next;
        if (blk->next) blk->next->prev = pb;
    }
}

// =====================================================================
//  Command history (persistent via save/load)
// =====================================================================
constexpr int CMD_FILE_LBA     = 300;  // moved from 256 to avoid kernel overlap
constexpr int CMD_FILE_SECTORS = 4;
constexpr int HIST_MAX         = 64;
constexpr int HIST_LEN         = 80;

static char g_hist[HIST_MAX][HIST_LEN];
static int  g_hist_count=0;
static uint8_t g_diskbuf[CMD_FILE_SECTORS*512];

static void hist_add(const char* s){
    if(g_hist_count<HIST_MAX){
        int i=0; while(s[i] && i<HIST_LEN-1){ g_hist[g_hist_count][i]=s[i]; i++; }
        g_hist[g_hist_count][i]=0;
        g_hist_count++;
    }
}

// =====================================================================
//  Shell command implementations
// =====================================================================

// --- Memory management commands ---
static void cmd_meminfo(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("=== Memory Information ===\n\n");
    term.set_color(make_color(WHITE, BLACK));

    // PMM stats
    term.write("Physical Memory Manager:\n");
    term.write("  Total RAM:     "); term.write_dec((int)pmm_mem_kb); term.write(" KiB (");
    term.write_dec((int)(pmm_mem_kb / 1024)); term.write(" MiB)\n");
    term.write("  Managed pages: "); term.write_dec((int)pmm_total_pages);
    term.write(" ("); term.write_dec((int)(pmm_total_pages * PAGE_SIZE / 1024)); term.write(" KiB)\n");
    term.write("  Free pages:    "); term.write_dec((int)pmm_free_pages);
    term.write(" ("); term.write_dec((int)(pmm_free_pages * PAGE_SIZE / 1024)); term.write(" KiB)\n");
    term.write("  Used pages:    "); term.write_dec((int)pmm_used_pages);
    term.write(" ("); term.write_dec((int)(pmm_used_pages * PAGE_SIZE / 1024)); term.write(" KiB)\n");
    term.write("  PMM base:      "); term.write_hex(PMM_BASE_ADDR); term.write("\n\n");

    // VMM stats
    term.write("Virtual Memory Manager:\n");
    term.write("  Paging:        ");
    if (vmm_paging_on) { term.set_color(make_color(GREEN, BLACK)); term.write("ENABLED\n"); }
    else               { term.set_color(make_color(BROWN, BLACK)); term.write("DISABLED\n"); }
    term.set_color(make_color(WHITE, BLACK));
    term.write("  Mode:          ");
    if (vmm_long_mode)        term.write("Long mode (64-bit 4-level paging)\n");
    else if (vmm_our_paging)  term.write("32-bit (our PSE 4 MiB paging)\n");
    else                      term.write("32-bit (firmware paging)\n");
    term.write("  Page directory:"); term.write_hex((uintptr_t)page_directory); term.write("\n");
    term.write("  CR0:           "); term.write_hex(read_cr0()); term.write("\n");
    term.write("  CR3:           "); term.write_hex(read_cr3()); term.write("\n");
    term.write("  CR4:           "); term.write_hex(read_cr4()); term.write("\n\n");

    // Heap stats
    term.write("Kernel Heap:\n");
    term.write("  Region:        "); term.write_hex(HEAP_START);
    term.write(" - "); term.write_hex(HEAP_END);
    term.write(" ("); term.write_dec(HEAP_SIZE / 1024); term.write(" KiB)\n");
    term.write("  Allocations:   "); term.write_dec((int)heap_alloc_count); term.write("\n");
    term.write("  Frees:         "); term.write_dec((int)heap_free_count); term.write("\n");
    term.write("  Bytes alloc:   "); term.write_dec((int)heap_bytes_alloc); term.write("\n");
    term.write("  Bytes freed:   "); term.write_dec((int)heap_bytes_freed); term.write("\n");
    int in_use = (int)heap_bytes_alloc - (int)heap_bytes_freed;
    term.write("  In use:        "); term.write_dec(in_use); term.write(" bytes\n\n");

    // Memory map summary
    term.write("Memory Map:\n");
    term.write("  0x000000 - 0x000FFF  Real-mode IVT + BDA\n");
    term.write("  0x007C00 - 0x007DFF  Boot sector\n");
    term.write("  0x010000 - 0x027FFF  Kernel image (~92 KiB)\n");
    term.write("  0x070000 - 0x07FFFF  Page directory + page tables\n");
    term.write("  0x080000 - 0x081FFF  PMM bitmap (8 KiB)\n");
    term.write("  0x090000 - 0x09FFFF  Kernel stack\n");
    term.write("  0x0B8000 - 0x0BFFFF  VGA text buffer\n");
    term.write("  0x100000 - 0x1FFFFF  Free (PMM-managed)\n");
    term.write("  0x200000 - 0x2FFFFF  Kernel heap (1 MiB)\n");
    term.write("  0x300000+            Free (PMM-managed)\n");
}

static void cmd_memtest(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("=== Memory Allocation Test ===\n\n");
    term.set_color(make_color(WHITE, BLACK));

    // Test 1: kmalloc/kfree
    term.write("[1] kmalloc/kfree test:\n");
    void* p1 = kmalloc(64);
    void* p2 = kmalloc(256);
    void* p3 = kmalloc(1024);
    term.write("  kmalloc(64)   = "); term.write_hex((uintptr_t)p1); term.write("\n");
    term.write("  kmalloc(256)  = "); term.write_hex((uintptr_t)p2); term.write("\n");
    term.write("  kmalloc(1024) = "); term.write_hex((uintptr_t)p3); term.write("\n");

    // Write test patterns
    if (p1) { memset_(p1, 0xAA, 64); }
    if (p2) { memset_(p2, 0xBB, 256); }
    if (p3) { memset_(p3, 0xCC, 1024); }

    // Verify
    bool ok = true;
    if (p1){ for(int i=0;i<64;i++)  if(((uint8_t*)p1)[i]!=0xAA) ok=false; }
    if (p2){ for(int i=0;i<256;i++) if(((uint8_t*)p2)[i]!=0xBB) ok=false; }
    if (p3){ for(int i=0;i<1024;i++)if(((uint8_t*)p3)[i]!=0xCC) ok=false; }
    term.write("  Pattern verify: ");
    term.set_color(ok ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
    term.write(ok ? "PASS\n" : "FAIL\n");
    term.set_color(make_color(WHITE, BLACK));

    kfree(p1);
    kfree(p2);
    term.write("  kfree(p1), kfree(p2) done\n");

    // Allocate again to test reuse
    void* p4 = kmalloc(64);
    term.write("  kmalloc(64)   = "); term.write_hex((uintptr_t)p4);
    term.write(" (should reuse freed block)\n");
    kfree(p3);
    kfree(p4);
    term.write("  kfree(p3), kfree(p4) done\n");
    term.write("  Allocs: "); term.write_dec((int)heap_alloc_count);
    term.write("  Frees: "); term.write_dec((int)heap_free_count); term.write("\n\n");

    // Test 2: PMM
    term.write("[2] PMM page allocation test:\n");
    uint32_t pages[5];
    for (int i = 0; i < 5; i++){
        pages[i] = pmm_alloc_page();
        term.write("  pmm_alloc_page() = "); term.write_hex(pages[i]); term.write("\n");
    }
    for (int i = 0; i < 5; i++){
        if (pages[i]) pmm_free_page(pages[i]);
    }
    term.write("  All 5 pages freed\n");
    term.write("  Free pages: "); term.write_dec((int)pmm_free_pages); term.write("\n\n");

    // Test 3: Large allocation
    term.write("[3] Large allocation test (100 KiB):\n");
    void* big = kmalloc(100 * 1024);
    term.write("  kmalloc(102400) = "); term.write_hex((uintptr_t)big); term.write("\n");
    if (big){
        memset_(big, 0x42, 100 * 1024);
        bool big_ok = true;
        for (int i = 0; i < 100 * 1024; i++)
            if (((uint8_t*)big)[i] != 0x42) { big_ok = false; break; }
        term.write("  Verify 100 KiB: ");
        term.set_color(big_ok ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
        term.write(big_ok ? "PASS\n" : "FAIL\n");
        term.set_color(make_color(WHITE, BLACK));
        kfree(big);
        term.write("  Freed\n");
    }
    term.write("\n");
}

static void cmd_pagetest(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("=== Virtual Memory Page Test ===\n\n");
    term.set_color(make_color(WHITE, BLACK));

    if (!vmm_our_paging){
        term.write("Page mapping test requires 32-bit PSE paging.\n");
        term.write("In 64-bit long mode, firmware/switcher paging is used.\n\n");
        return;
    }

    // Show current identity mapping
    term.write("[1] Identity mapping check:\n");
    uint32_t test_addr = 0x100000;  // 1 MiB
    uint32_t phys = vmm_get_phys(test_addr);
    term.write("  vmm_get_phys("); term.write_hex(test_addr);
    term.write(") = "); term.write_hex(phys);
    term.write(phys == test_addr ? " (identity OK)\n" : " (MISMATCH!)\n");

    test_addr = 0x200000;  // 2 MiB (heap)
    phys = vmm_get_phys(test_addr);
    term.write("  vmm_get_phys("); term.write_hex(test_addr);
    term.write(") = "); term.write_hex(phys);
    term.write(phys == test_addr ? " (identity OK)\n" : " (MISMATCH!)\n\n");

    // Map a physical page to a higher virtual address
    term.write("[2] Page mapping test (4 KiB):\n");
    uint32_t vaddr = 0x40000000;  // 1 GiB virtual
    uint32_t paddr = pmm_alloc_page();
    if (paddr == 0){
        term.write("  PMM out of memory!\n\n");
        return;
    }
    term.write("  Physical page: "); term.write_hex(paddr); term.write("\n");
    term.write("  Virtual addr:  "); term.write_hex(vaddr); term.write("\n");

    // Write pattern to physical page (via identity mapping)
    uint8_t* phys_ptr = (uint8_t*)paddr;
    for (int i = 0; i < 256; i++) phys_ptr[i] = (uint8_t)(i & 0xFF);

    // Map virtual to physical
    bool mapped = vmm_map_page(vaddr, paddr, PG_PRESENT | PG_RW);
    term.write("  vmm_map_page:  ");
    term.set_color(mapped ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
    term.write(mapped ? "OK\n" : "FAILED\n");
    term.set_color(make_color(WHITE, BLACK));

    if (mapped){
        // Verify translation
        phys = vmm_get_phys(vaddr);
        term.write("  vmm_get_phys("); term.write_hex(vaddr);
        term.write(") = "); term.write_hex(phys); term.write("\n");

        // Read through virtual address
        uint8_t* virt_ptr = (uint8_t*)vaddr;
        bool vok = true;
        for (int i = 0; i < 256; i++)
            if (virt_ptr[i] != (uint8_t)(i & 0xFF)) { vok = false; break; }
        term.write("  Read via virtual: ");
        term.set_color(vok ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
        term.write(vok ? "PASS\n" : "FAIL\n");
        term.set_color(make_color(WHITE, BLACK));

        // Write via virtual, read via physical
        virt_ptr[0] = 0x99;
        term.write("  Write 0x99 via virtual, read via physical: ");
        term.set_color(phys_ptr[0] == 0x99 ? make_color(GREEN,BLACK) : make_color(RED,BLACK));
        term.write(phys_ptr[0] == 0x99 ? "PASS\n" : "FAIL\n");
        term.set_color(make_color(WHITE, BLACK));
    }

    // Page directory dump (first 4 entries) - only for 32-bit paging
    if (!vmm_long_mode) {
        term.write("\n[3] Page directory (first 4 entries):\n");
        for (int i = 0; i < 4; i++){
            uint32_t pde = page_directory[i];
            term.write("  PDE["); term.write_dec(i); term.write("] = ");
            term.write_hex(pde);
            if (pde & PG_PRESENT){
                if (pde & PG_PSE) term.write("  4MiB page @ ");
                else              term.write("  PT @ ");
                term.write_hex(pde & 0xFFFFF000u);
            } else {
                term.write("  (not present)");
            }
            term.write("\n");
        }
    } else {
        term.write("\n[3] Page tables: 4-level paging (long mode)\n");
        term.write("  CR3 (PML4): "); term.write_hex(read_cr3()); term.write("\n");
    }
    term.write("\n");

    pmm_free_page(paddr);
}

static void cmd_help(){
    term.write("Commands:\n");
    term.write("  help        Show this help\n");
    term.write("  echo <text> Print text\n");
    term.write("  clear, cls  Clear screen & history\n");
    term.write("  about       System info\n");
    term.write("  history, h  Show command history\n");
    term.write("  save        Save history to disk (LBA ");
    term.write_dec(CMD_FILE_LBA); term.write(")\n");
    term.write("  load        Load history from disk\n");
    term.write("\nMKFS (custom file system with dirs):\n");
    term.write("  mkfs        Format MKFS file system\n");
    term.write("  ls, dir     List files in current dir\n");
    term.write("  cat, type   Print file from MKFS\n");
    term.write("  touch       Create empty file\n");
    term.write("  write <f>   Write text to file (empty line to save)\n");
    term.write("  rm, del     Delete file or empty dir\n");
    term.write("  copy <s> <d> Copy file (src -> dst)\n");
    term.write("  mkdir, md   Create directory\n");
    term.write("  cd <d>      Change directory (supports / and \\)\n");
    term.write("  pwd         Show current path\n");
    term.write("\nSFS (compatible read-only file system):\n");
    term.write("  lsfs        List files on SFS\n");
    term.write("  catfs <f>   Print file from SFS\n");
    term.write("\nDisk partitions (Windows-compatible):\n");
    term.write("  part        Show MBR partition table\n");
    term.write("  mount <n>   Mount FAT32 partition (1-4)\n");
    term.write("  lsfat       List files on mounted FAT32\n");
    term.write("  fatinfo     Show mounted FAT32 info\n");
    term.write("\nScript execution:\n");
    term.write("  run <f>     Run .sh script from MKFS\n");
    term.write("  runfs <f>   Run .sh script from SFS\n");
    term.write("\nAI engine:\n");
    term.write("  ai init     Initialize AI engine (Markov + Transformer)\n");
    term.write("  ai info     Show AI engine status\n");
    term.write("  ai mode     Switch mode (markov/transformer)\n");
    term.write("  ai test     Test transformer forward pass\n");
    term.write("  ai cleanup  Free AI engine resources\n");
    term.write("  generate <p> Generate text from prompt\n");
    term.write("  agent init  Initialize multi-agent framework\n");
    term.write("  agent run <g> Run agent pipeline (Planner->Actor->Critic)\n");
        term.write("  agent remote-url <url>  Set OpenAI-compatible remote endpoint\n");
        term.write("  agent remote-key <key>  Set API key (sent as Authorization: Bearer)\n");
        term.write("  agent remote-model <m>  Set model name (e.g. deepseek-chat)\n");
        term.write("  agent remote <p>  Ask the remote API directly\n");
    term.write("  agent status Show agent framework status\n");
    term.write("\nNetwork (HTTP server on port 8080):\n");
    term.write("  netstart    Initialize NE2000 NIC and HTTP server\n");
    term.write("  netinfo     Show network status (IP, MAC, connections)\n");
    term.write("  netstat     Alias for netinfo\n");
    term.write("  Web UI:     http://10.0.2.15:8080 (from host: localhost:8080)\n");
    term.write("\nKernel switching:\n");
    term.write("  switch      Switch to 32-bit kernel (loads from LBA 33)\n");
    term.write("\nSecurity (login + rwx permissions + consent engine):\n");
    term.write("  whoami      Show current user\n");
    term.write("  id          Show uid/gid of current user\n");
    term.write("  users       List all users\n");
    term.write("  login <u>   Log in as a user\n");
    term.write("  logout      Log out (returns to login prompt)\n");
    term.write("  su [u]      Switch user (default root)\n");
    term.write("  useradd <u> [pw]  Add a user (root only)\n");
    term.write("  deluser <u> Delete a user (root only)\n");
    term.write("  passwd [u]  Change password\n");
    term.write("  chmod <m> <f> Change file mode (e.g. 644)\n");
    term.write("  stat <f>    Show file owner/mode\n");
    term.write("  sudo <cmd>  Run a command as root\n");
    term.write("  perm [reset] Inspect / clear remembered grants\n");

    term.write("\nGUI:\n");
    term.write("  gui         Enter graphical desktop (VBE framebuffer)\n");
    term.write("  ESC         Exit GUI mode and return to text\n");
    term.write("  Mouse       Move cursor, drag title bar, click [X] to close\n");
    term.write("\nMemory management:\n");
    term.write("  meminfo     Show memory/PMM/VMM/heap stats\n");
    term.write("  memtest     Run kmalloc/kfree + PMM tests\n");
    term.write("  pagetest    Test virtual memory page mapping\n");
    term.write("\nPower management:\n");
    term.write("  shutdown    Power off (ACPI)\n");
    term.write("  reboot      Restart system\n");
    term.write("\nKeyboard shortcuts:\n");
    term.write("  Tab         Auto-complete command/filename\n");
    term.write("  Left/Right  Move cursor in input line\n");
    term.write("  Home/End    Jump to start/end of input line\n");
    term.write("  Up/Dn       Recall command history (at prompt)\n");
    term.write("  PgUp/PgDn   Scroll terminal view up/down\n");
    term.write("  Ctrl+C      Abort input / copy selected text\n");
    term.write("  Ctrl+V      Paste clipboard   Ctrl+L: refocus\n");
    term.write("  Ctrl+Up/Dn  Cycle clipboard history\n");
    term.write("\nMouse: wheel=scroll, drag=select, click=refocus\n");
}

static void cmd_about(){
    term.write("NexOS v4.0  -  C++ freestanding kernel (64-bit)\n");
    term.write("64-bit long mode kernel with 4-level paging.\n");
    term.write("PS/2 keyboard + mouse, ATA disk, VGA 80x25.\n");
    term.write("MKFS+SFS+FAT32, dirs, .sh, Tab completion.\n");
    term.write("PMM (bitmap) + VMM (x86 paging) + kmalloc/kfree + AI engine.\n");
}

// Build full path string for prompt (PowerShell-style "PS /path>")
static void build_prompt_path(char* buf, int maxlen){
    if (g_cwd == FS_ROOT_PARENT || !mkfs.mounted) {
        buf[0] = '/'; buf[1] = 0;
        return;
    }
    // Walk up parent chain
    uint16_t path[16];
    int depth = 0;
    uint16_t cur = g_cwd;
    while (cur != FS_ROOT_PARENT && depth < 16) {
        path[depth++] = cur;
        // Read entry to get parent
        int s = cur / FS_ENTRY_PER_SEC;
        int e = cur % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        cur = fe->parent;
    }
    int pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0 && pos < maxlen - 1; i--) {
        int s = path[i] / FS_ENTRY_PER_SEC;
        int e = path[i] % FS_ENTRY_PER_SEC;
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
        int j = 0;
        while (fe->name[j] && pos < maxlen - 2) {
            buf[pos++] = fe->name[j++];
        }
        if (i > 0 && pos < maxlen - 1)
            buf[pos++] = '/';
    }
    buf[pos] = 0;
}

static void cmd_history(){
    term.write("Command history ("); term.write_dec(g_hist_count); term.write("):\n");
    for(int i=0;i<g_hist_count;i++){
        term.write("  "); term.write_dec(i+1); term.write(". ");
        term.write(g_hist[i]); term.put_char('\n');
    }
}

static void cmd_save(){
    memset_(g_diskbuf,0,sizeof(g_diskbuf));
    g_diskbuf[0]='K'; g_diskbuf[1]='C'; g_diskbuf[2]='M'; g_diskbuf[3]='D';
    g_diskbuf[4]=(uint8_t)(g_hist_count&0xFF);
    g_diskbuf[5]=(uint8_t)((g_hist_count>>8)&0xFF);
    int off=8;
    for(int i=0;i<g_hist_count && off<(int)sizeof(g_diskbuf)-2;i++){
        int L=strlen_(g_hist[i]);
        if(off+1+L >= (int)sizeof(g_diskbuf)) break;
        g_diskbuf[off++]=(uint8_t)L;
        memcpy_(g_diskbuf+off, g_hist[i], L);
        off+=L;
    }
    for(int s=0;s<CMD_FILE_SECTORS;s++)
        ata_write_sector(CMD_FILE_LBA+s, (const uint16_t*)(g_diskbuf+s*512));
    term.write("Saved "); term.write_dec(g_hist_count);
    term.write(" commands to disk (LBA "); term.write_dec(CMD_FILE_LBA); term.write(").\n");
}

static void cmd_load(){
    for(int s=0;s<CMD_FILE_SECTORS;s++)
        ata_read_sector(CMD_FILE_LBA+s, (uint16_t*)(g_diskbuf+s*512));
    if(!(g_diskbuf[0]=='K'&&g_diskbuf[1]=='C'&&g_diskbuf[2]=='M'&&g_diskbuf[3]=='D')){
        term.write("No command file found on disk.\n");
        return;
    }
    int count=g_diskbuf[4] | (g_diskbuf[5]<<8);
    g_hist_count=0;
    int off=8;
    for(int i=0;i<count && off<(int)sizeof(g_diskbuf)-1;i++){
        int L=g_diskbuf[off++];
        if(L>=HIST_LEN) L=HIST_LEN-1;
        if(off+L>(int)sizeof(g_diskbuf)) break;
        memcpy_(g_hist[g_hist_count], g_diskbuf+off, L);
        g_hist[g_hist_count][L]=0;
        g_hist_count++;
        off+=L;
    }
    term.write("Loaded "); term.write_dec(g_hist_count);
    term.write(" commands from disk.\n");
}

// ----- MKFS commands -----
static void cmd_mkfs(){
    mkfs.format();
    g_cwd = FS_ROOT_PARENT;  // reset to root
    term.write("MKFS formatted. Data area: LBA ");
    term.write_dec(MKFS_DATA_LBA); term.write("-");
    term.write_dec(MKFS_DATA_LBA + MKFS_DATA_SECTORS - 1);
    term.write(" ("); term.write_dec(MKFS_DATA_SECTORS * 512 / 1024);
    term.write(" KB)\n");
}

static void cmd_ls(){
    mkfs.ls();
}

static bool perm_check(const char* name, char need, bool quiet);  // fwd decl (defined in security block)

static void cmd_cat(const char* name){
    if (!name[0]) { term.write("Usage: cat <filename>\n"); return; }
    if(!perm_check(name, 'r', false)) return;
    int ret = mkfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    g_iobuf[ret] = 0;
    term.write((const char*)g_iobuf);
    if (ret > 0 && g_iobuf[ret-1] != '\n') term.put_char('\n');
}

static void cmd_touch(const char* name){
    if (!name[0]) { term.write("Usage: touch <filename>\n"); return; }
    char buf[FS_NAME_LEN];
    int i=0; while(name[i] && i<FS_NAME_LEN-1){ buf[i]=name[i]; i++; }
    buf[i]=0;
    int ret = mkfs.create(buf, (const uint8_t*)"", 0);
    if (ret >= 0) {
        term.write("Created: "); term.write(buf); term.put_char('\n');
    } else {
        term.write("Failed (code "); term.write_dec(ret); term.write(")\n");
    }
}

static void cmd_rm(const char* name){
    if (!name[0]) { term.write("Usage: rm <filename>\n"); return; }
    if(!perm_check(name, 'w', false)) return;
    int ret = mkfs.remove(name);
    if (ret >= 0) {
        term.write("Removed: "); term.write(name); term.put_char('\n');
    } else if (ret == -3) {
        term.write("Directory not empty: "); term.write(name); term.put_char('\n');
    } else {
        term.write("File not found: "); term.write(name); term.put_char('\n');
    }
}

static void cmd_copy(const char* args){
    // Parse "src dst" from args
    char src[FS_NAME_LEN], dst[FS_NAME_LEN];
    int i=0, j=0;
    while(args[i] && args[i]!=' ' && j<FS_NAME_LEN-1) src[j++]=args[i++];
    src[j]=0;
    while(args[i]==' ') i++;
    j=0;
    while(args[i] && args[i]!=' ' && j<FS_NAME_LEN-1) dst[j++]=args[i++];
    dst[j]=0;
    if(!src[0] || !dst[0]){
        term.write("Usage: copy <src> <dst>\n");
        return;
    }
    int ret = mkfs.copy(src, dst);
    if(ret >= 0){
        term.write("Copied: "); term.write(src);
        term.write(" -> "); term.write(dst);
        term.write(" ("); term.write_dec(ret); term.write(" bytes)\n");
    } else if(ret == -2){
        term.write("Source not found: "); term.write(src); term.put_char('\n');
    } else {
        term.write("Copy failed (code "); term.write_dec(ret); term.write(")\n");
    }
}

static void cmd_write(const char* name){
    if (!name[0]) { term.write("Usage: write <filename>\n"); return; }
    int i=0; while(name[i] && i<FS_NAME_LEN-1){ g_write_name[i]=name[i]; i++; }
    g_write_name[i]=0;
    g_write_len=0;
    g_mode=MODE_WRITE;
    term.write("Writing to: "); term.write(g_write_name);
    term.write("\nEnter text (empty line to save, max 8KB):\n");
}

static void cmd_mkdir(const char* name){
    if (!name[0]) { term.write("Usage: mkdir <dirname>\n"); return; }
    int ret = mkfs.mkdir(name);
    if (ret >= 0) {
        term.write("Created dir: "); term.write(name); term.put_char('\n');
    } else if (ret == -2) {
        term.write("Already exists: "); term.write(name); term.put_char('\n');
    } else {
        term.write("Failed (code "); term.write_dec(ret); term.write(")\n");
    }
}

static void cmd_cd(const char* name){
    char path[FS_NAME_LEN * 2];
    int i=0;
    while(name[i] && i < (int)sizeof(path)-1){ path[i]=name[i]; i++; }
    path[i]=0;
    normalize_path(path);  // convert \ to /

    int ret = mkfs.cd(path);
    if (ret < 0) {
        if (ret == -2) term.write("Directory not found: ");
        else if (ret == -3) term.write("Not a directory: ");
        else term.write("cd failed: ");
        term.write(path); term.put_char('\n');
    }
}

static void cmd_pwd(){
    mkfs.pwd();
}

// ----- SFS commands -----
static void cmd_lsfs(){
    sfs.ls();
}

static void cmd_catfs(const char* name){
    if (!name[0]) { term.write("Usage: catfs <filename>\n"); return; }
    int ret = sfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    g_iobuf[ret] = 0;
    term.write((const char*)g_iobuf);
    if (ret > 0 && g_iobuf[ret-1] != '\n') term.put_char('\n');
}

// ----- Partition commands -----
static void cmd_part(){
    ata_read_sector(0, (uint16_t*)g_fsbuf);
    uint8_t* mbr = g_fsbuf;

    term.write("MBR partition table (LBA 0):\n");

    // Check MBR signature
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        term.write("  No valid MBR signature (0x55AA) found.\n");
        term.write("  This disk uses a raw layout (no partition table).\n");
    } else {
        term.write("  Signature: 0x55AA (valid)\n");
        MbrPartition* parts = (MbrPartition*)(mbr + 446);
        int found = 0;
        for (int i = 0; i < 4; i++) {
            MbrPartition* p = &parts[i];
            if (p->type == 0) continue;
            found++;
            term.write("\n  Partition "); term.write_dec(i + 1);
            term.write(p->boot_flag & 0x80 ? " [BOOT]" : "       ");
            term.write("\n    Type:        "); term.write_hex(p->type);
            term.write(" ("); term.write(part_type_name(p->type)); term.write(")\n");
            term.write("    Start LBA:   "); term.write_dec((int)p->start_lba); term.write("\n");
            term.write("    Sectors:     "); term.write_dec((int)p->total_sectors); term.write("\n");
            term.write("    Size:        ");
            term.write_dec((int)(p->total_sectors / 2)); term.write(" KB (");
            term.write_dec((int)(p->total_sectors / 2048)); term.write(" MB)\n");

            // If FAT type, try to read BPB
            if (is_fat_type(p->type)) {
                ata_read_sector(p->start_lba, (uint16_t*)g_iobuf);
                if (g_iobuf[510] == 0x55 && g_iobuf[511] == 0xAA) {
                    uint16_t bps = *(uint16_t*)(g_iobuf + 11);
                    uint8_t spc = g_iobuf[13];
                    uint16_t root_entries = *(uint16_t*)(g_iobuf + 17);
                    term.write("    FAT BPB:     ");
                    term.write_dec((int)bps); term.write(" bytes/sec, ");
                    term.write_dec((int)spc); term.write(" sec/cluster");
                    if (root_entries == 0) term.write(" (FAT32)");
                    else { term.write(" (FAT16, "); term.write_dec((int)root_entries); term.write(" root entries)"); }
                    term.write("\n");
                    // Volume label
                    term.write("    Volume:      ");
                    for (int j = 0; j < 11; j++) {
                        char c = (char)g_iobuf[71 + j];
                        if (c >= 0x20) term.put_char(c);
                    }
                    term.put_char('\n');
                }
            }
        }
        if (found == 0)
            term.write("\n  No partitions defined (all entries empty).\n");
    }

    // Show NexOS disk layout
    term.write("\nNexOS disk layout:\n");
    term.write("  LBA 0:        MBR / Boot sector\n");
    term.write("  LBA 1-32:     Stage2 bootloader\n");
    term.write("  LBA 33+:      C++ kernel (up to 128 KiB)\n");
    term.write("  LBA 300-303:  Command history (save/load)\n");
    term.write("  LBA 512-799:  MKFS file system\n");
    term.write("  LBA 800-1023: SFS file system\n");
}

static void cmd_mount(const char* arg){
    if (!arg[0]) { term.write("Usage: mount <partition_number 1-4>\n"); return; }

    int part_num = 0;
    const char* p = arg;
    while(*p >= '0' && *p <= '9'){ part_num = part_num * 10 + (*p - '0'); p++; }
    if (part_num < 1 || part_num > 4) {
        term.write("Partition number must be 1-4\n");
        return;
    }

    MbrPartition mp;
    memset_(&mp, 0, sizeof(mp));
    uint32_t start = read_mbr_partition(part_num, &mp);
    if (start == 0 || mp.type == 0) {
        term.write("Partition "); term.write_dec(part_num);
        term.write(" does not exist\n");
        return;
    }

    term.write("Mounting partition "); term.write_dec(part_num);
    term.write(" (type "); term.write_hex(mp.type);
    term.write(": "); term.write(part_type_name(mp.type)); term.write(")\n");
    term.write("  Start LBA: "); term.write_dec((int)start); term.write("\n");

    if (!is_fat_type(mp.type)) {
        term.write("  Not a FAT partition (type ");
        term.write_hex(mp.type); term.write(").\n");
        term.write("  NTFS partitions cannot be read (read-only kernel).\n");
        if (fat32.mounted) {
            term.write("  Previous FAT32 mount unmounted.\n");
            fat32.mounted = false;
        }
        return;
    }

    if (fat32.mount(start)) {
        term.write("  FAT32 mounted successfully!\n");
        term.write("  Use 'lsfat' to list files, 'fatinfo' for details.\n");
    } else {
        term.write("  FAT32 mount failed (not FAT32 or invalid BPB).\n");
        term.write("  Note: FAT16 partitions are not supported yet.\n");
    }
}

static void cmd_lsfat(){
    fat32.ls();
}

static void cmd_fatinfo(){
    fat32.info();
}

// ----- Script runner -----
static bool g_in_script = false;
static void run_command(const char* line);  // forward declaration

static void run_script(const char* content, int size){
    char line[HIST_LEN];
    int linepos = 0;
    g_in_script = true;
    for (int i = 0; i < size; i++) {
        char c = content[i];
        if (c == '\n') {
            line[linepos] = 0;
            const char* p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != 0 && *p != '#') {
                term.set_color(make_color(YELLOW, BLACK));
                term.write("> "); term.write(p); term.put_char('\n');
                term.set_color(make_color(LIGHT_GREY, BLACK));
                run_command(p);
            }
            linepos = 0;
        } else if (c != '\r') {
            if (linepos < HIST_LEN - 1) line[linepos++] = c;
        }
    }
    if (linepos > 0) {
        line[linepos] = 0;
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != 0 && *p != '#') {
            term.set_color(make_color(YELLOW, BLACK));
            term.write("> "); term.write(p); term.put_char('\n');
            term.set_color(make_color(LIGHT_GREY, BLACK));
            run_command(p);
        }
    }
    g_in_script = false;
}

static void cmd_run(const char* name){
    if (!name[0]) { term.write("Usage: run <script.sh>\n"); return; }
    int ret = mkfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- running: "); term.write(name); term.write(" ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    run_script((const char*)g_iobuf, ret);
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- end of script ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
}

static void cmd_runfs(const char* name){
    if (!name[0]) { term.write("Usage: runfs <script.sh>\n"); return; }
    int ret = sfs.read(name, g_iobuf, FS_IOBUF_SIZE);
    if (ret < 0) {
        term.write("File not found: "); term.write(name); term.put_char('\n');
        return;
    }
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- running SFS: "); term.write(name); term.write(" ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    run_script((const char*)g_iobuf, ret);
    term.set_color(make_color(CYAN, BLACK));
    term.write("--- end of script ---\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
}

// =====================================================================
//  Shutdown / Reboot  -  ACPI power-off + keyboard controller reset
// =====================================================================
static void cmd_shutdown(){
    term.set_color(make_color(YELLOW, BLACK));
    term.write("\nShutting down...\n");
    term.render();
    serial_puts("[SHUTDOWN] ACPI power-off requested\n");

    // QEMU / ACPI shutdown methods (try multiple ports)
    outw(0x604, 0x2000);    // QEMU ACPI shutdown (modern)
    outw(0xB004, 0x2000);   // QEMU/Bochs ACPI (legacy)
    outw(0x4004, 0x3400);   // Older ACPI

    // Fallback: halt forever
    for(;;){
        __asm__ __volatile__("hlt");
    }
}

static void cmd_reboot(){
    term.set_color(make_color(YELLOW, BLACK));
    term.write("\nRebooting...\n");
    term.render();
    serial_puts("[REBOOT] Keyboard controller reset\n");

    // Keyboard controller reset (pulse reset line)
    // Small delay to let serial flush
    for(volatile int i=0; i<100000; i++);
    uint8_t val = inb(0x64);
    outb(0x64, val | 0x04);    // set System Reset bit
    outb(0x64, val & ~0x04);   // clear it
    // Alternative: write 0xFE to port 0x64
    outb(0x64, 0xFE);

    // Triple-fault fallback (64-bit IDT descriptor: 2-byte limit + 8-byte base)
    static struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ __volatile__("lidt %0" :: "m"(null_idt));
    __asm__ __volatile__("int $0x03");

    for(;;){ __asm__ __volatile__("hlt"); }
}

// =====================================================================
//  AI Engine commands
// =====================================================================

static void cmd_ai(const char* args){
    if(!*args){
        if(!g_ai_initialized){
            term.write("AI engine not initialized. Use 'ai init' first.\n");
            return;
        }
        char info[512];
        ai_get_info(info, sizeof(info));
        term.write(info);
        return;
    }
    // Parse subcommand
    char sub[16]; int si=0;
    while(*args && *args!=' ' && si<15) sub[si++]=*args++;
    sub[si]=0;
    while(*args==' ') args++;

    if(!strcmp_(sub,"init")){
        term.write("Initializing AI engine...\n");
        serial_puts("[AI] ai_init starting...\n");
        int ret = ai_init("/boot/model.gguf");
        serial_puts("[AI] ai_init returned.\n");
        if(ret==0){
            g_ai_initialized = true;
            term.write("AI engine initialized (Markov mode).\n");
            term.write("  Corpus trained. Model weights generated.\n");
        } else {
            term.write("AI init failed!\n");
        }
    } else if(!strcmp_(sub,"info")){
        if(!g_ai_initialized){ term.write("AI not initialized. Use 'ai init'.\n"); return; }
        char info[512];
        ai_get_info(info, sizeof(info));
        term.write(info);
    } else if(!strcmp_(sub,"mode")){
        if(!g_ai_initialized){ term.write("AI not initialized.\n"); return; }
        if(!strcmp_(args,"transformer") || !strcmp_(args,"1")){
            ai_set_mode(1);
            term.write("Switched to Transformer mode.\n");
        } else if(!strcmp_(args,"markov") || !strcmp_(args,"0")){
            ai_set_mode(0);
            term.write("Switched to Markov mode.\n");
        } else {
            term.write("Usage: ai mode <markov|transformer>\n");
        }
    } else if(!strcmp_(sub,"test")){
        if(!g_ai_initialized){ term.write("AI not initialized.\n"); return; }
        term.write("Running transformer forward pass test...\n");
        int ret = ai_transformer_test();
        if(ret==0) term.write("Transformer forward pass: OK (no NaN/Inf)\n");
        else term.write("Transformer test failed!\n");
    } else if(!strcmp_(sub,"cleanup")){
        if(g_ai_initialized){
            ai_cleanup();
            g_ai_initialized = false;
            term.write("AI engine cleaned up.\n");
        } else {
            term.write("AI not initialized.\n");
        }
    } else {
        term.write("Usage: ai [init|info|mode|test|cleanup]\n");
    }
}

static void cmd_generate(const char* args){
    if(!g_ai_initialized){
        term.write("AI not initialized. Use 'ai init' first.\n");
        return;
    }
    if(!*args){
        term.write("Usage: generate <prompt>\n");
        return;
    }
    term.write("Generating...");
    term.put_char('\n');
    char* result = ai_generate(args, 200);
    if(result){
        term.write(result);
        term.put_char('\n');
        kfree(result);
    } else {
        term.write("Generation failed.\n");
    }
}

// Streaming sink for qwen_generate(): terminal + serial so headless tests
// can assert on the generated text.
// Forward declaration (defined later, next to the auto-answer backdoor).
static int model_try_load(void);

static void ask_emit(const char* piece){
    term.write(piece);
    serial_puts(piece);
}

// Collecting sink for distnet: stream qwen output into a bounded buffer so
// kern_ai_ask() can copy a real-GGUF answer back to the (32- or 64-bit)
// distnet compute node without touching the Markov engine.
#define DN_AI_BUF_SZ 512
static char g_dn_ai_buf[DN_AI_BUF_SZ];
static int  g_dn_ai_len;
static void ask_collect(const char* piece){
    while (*piece && g_dn_ai_len < DN_AI_BUF_SZ - 1)
        g_dn_ai_buf[g_dn_ai_len++] = *piece++;
}
static void ask_collect_reset(void){ g_dn_ai_len = 0; g_dn_ai_buf[0] = 0; }

static void cmd_ask(const char* args){
    // A real GGUF beats the built-in toy engine whenever one is loaded,
    // regardless of whether 'ai init' was ever run.
    if(qwen_ready()){
        if(!*args){ term.write("Usage: ask <question>\n"); return; }
        const GGUFModelInfo* mi = qwen_info();
        term.write("AI ("); term.write(mi ? mi->arch : "gguf");
        term.write("/real weights): ");
        serial_puts("[qwen] ask: "); serial_puts(args); serial_puts("\n");
        serial_puts("[qwen] out: ");
        char ask_sys[768];
        kb_build_prompt(args, ask_sys, (int)sizeof(ask_sys));
        int n = qwen_chat(ask_sys, args, 64, 0.8f, ask_emit);
        serial_puts("\n[qwen] tokens=");
        { char b[16]; int i=0, v=(n<0)?0:n;
          if(!v){ b[i++]='0'; } else { char t[16]; int j=0;
            while(v){ t[j++]=(char)('0'+v%10); v/=10; }
            while(j) b[i++]=t[--j]; }
          b[i]=0; serial_puts(b); }
        serial_puts("\n");
        term.put_char('\n');
        if(n <= 0) term.write("(model produced no tokens)\n");
        return;
    }
    if(!g_ai_initialized){
        term.write("AI not initialized. Use 'ai init' (or 'agent init') first.\n");
        return;
    }
    if(!*args){
        term.write("Usage: ask <question>\n");
        return;
    }
    term.write(ai_env_is_vm()
               ? "AI (VM/built-in): "
               : "AI (bare-metal/real): ");
    char* result = ai_generate(args, 160);
    if(result){
        term.write(result);
        term.put_char('\n');
        kfree(result);
    } else {
        term.write("(no response)\n");
    }
}

static void cmd_agent(const char* args){
    if(!*args){
        char status[256];
        agent_get_status(status, sizeof(status));
        term.write(status);
        return;
    }
    char sub[16]; int si=0;
    while(*args && *args!=' ' && si<15) sub[si++]=*args++;
    sub[si]=0;
    while(*args==' ') args++;

    if(!strcmp_(sub,"init")){
        if(!g_ai_initialized){
            ai_init("/boot/model.gguf");
            g_ai_initialized = true;
        }
        agent_init();
        term.write("Agent framework initialized.\n");
        term.write("  Agents: Planner, Actor, Critic\n");
        term.write("  Use 'agent run <goal>' to execute.\n");
    } else if(!strcmp_(sub,"run")){
        if(!*args){
            term.write("Usage: agent run <goal>\n");
            return;
        }
        // The AI desktop runs on this kernel now, so answering with the
        // real GGUF model is the default when one is embedded.  The built-in
        // Markov engine only kicks in as an offline fallback.
        if(model_try_load()){
            serial_puts("[qwen] ask: "); serial_puts(args); serial_puts("\n");
            serial_puts("[qwen] out: ");
            int n = qwen_generate(args, 128, 0.8f, ask_emit);
            serial_puts("\n[qwen] tokens=");
            { char b[16]; int i=0, v=(n<0)?0:n;
              if(!v){ b[i++]='0'; } else { char t[16]; int j=0;
                while(v){ t[j++]=(char)('0'+v%10); v/=10; }
                while(j) b[i++]=t[--j]; }
              b[i]=0; serial_puts(b); }
            serial_puts("\n");
            term.put_char('\n');
            if(n <= 0) term.write("(model produced no tokens)\n");
            return;
        }
        char output[4096];
        int n = agent_run(args, output, sizeof(output));
        if(n > 0){
            term.write(output);
        } else {
            term.write("Agent run failed. Initialize first with 'agent init'.\n");
        }
    } else if(!strcmp_(sub,"remote-url")){
        if(!*args){ term.write("Usage: agent remote-url <openai-compatible-url>\n"); return; }
        net_set_agent_remote_url(args);
        term.write("Remote agent URL set to: "); term.write(args); term.write("\n");
    } else if(!strcmp_(sub,"remote-key")){
        if(!*args){ term.write("Usage: agent remote-key <api-key>\n"); return; }
        net_set_agent_api_key(args);
        term.write("Remote API key set (sent as Authorization: Bearer).\n");
    } else if(!strcmp_(sub,"remote-model")){
        if(!*args){ term.write("Usage: agent remote-model <model-name>\n"); return; }
        net_set_agent_model(args);
        term.write("Remote model set to: "); term.write(args); term.write("\n");
    } else if(!strcmp_(sub,"remote")){
        if(!*args){ term.write("Usage: agent remote <prompt>\n"); return; }
        char out[4096];
        int n = net_agent_remote(args, NULL, out, (int)sizeof(out));
        if(n > 0){ term.write(out); term.write("\n"); }
        else { term.write("(remote agent failed - check agent remote-url and connectivity)\n"); }
    } else if(!strcmp_(sub,"status")){
        char status[256];
        agent_get_status(status, sizeof(status));
        term.write(status);
    } else if(!strcmp_(sub,"abort")){
        agent_abort();
        term.write("Agent abort requested.\n");
    } else if(!strcmp_(sub,"confirm")){
        if(!strcmp_(args,"off")){ agent_set_confirm(0); term.write("Confirm mode OFF.\n"); }
        else { agent_set_confirm(1); term.write("Confirm mode ON (dangerous tasks blocked).\n"); }
    } else {
        term.write("Usage: agent [init|run|status|abort|confirm|remote-url|remote-key|remote-model|remote]\n");
    }
}

// Forward declarations: both are defined much later in this file (near the
// GUI callback table), but cmd_model() needs them here.
static void uint_to_str_k64(uint32_t val, char* buf);
static int  gui_cb_read_file(int fs_type, const char* name, uint8_t* buf, int bufsize);

// Keep parity with the 32-bit kernel: report model sizes via a constant shift
// instead of a 64-bit divide.
static uint32_t approx_mb_k64(unsigned long long bytes){ return (uint32_t)(bytes >> 20); }

// ---------------------------------------------------------------------
//  Raw model blob region
//
//  SFS tops out around 384 KiB, which is far too small for real GGUF
//  weights, so the build drops the model straight onto the disk after the
//  filesystem.  LBA 4095 holds a one-sector descriptor, the payload starts
//  at LBA 4096 and may run for as many sectors as the image has.
// ---------------------------------------------------------------------
// The descriptor location has moved once already (SFS grew past LBA 4096),
// so probe a list instead of trusting a single hard-coded sector.
#define MODEL_DATA_LBA  16384u
static const uint32_t g_model_hdr_lbas[] = { 16383u, 8191u, 4095u };

struct ModelBlobHdr {
    char     magic[8];      // "MINIMDL1"
    uint64_t size;          // payload bytes
    uint64_t start_lba;     // where the payload begins
};

static uint8_t*  g_model_blob   = 0;   // big_alloc'd copy of the weights
static uint64_t  g_model_bytes  = 0;

// Read the descriptor; returns payload size or 0 when no blob is present.
static uint64_t model_blob_probe(uint64_t* out_lba){
    uint8_t sec[512];
    const char* m = "MINIMDL1";
    for (unsigned c = 0; c < sizeof(g_model_hdr_lbas)/sizeof(g_model_hdr_lbas[0]); c++){
        ata_read_sector(g_model_hdr_lbas[c], (uint16_t*)sec);
        bool hit = true;
        for (int i = 0; i < 8; i++) if (sec[i] != (uint8_t)m[i]) { hit = false; break; }
        if (!hit) continue;
        ModelBlobHdr h;
        memcpy_(&h, sec, sizeof(h));
        if (h.size == 0 || h.size > (1ull << 32)) continue;
        if (out_lba) *out_lba = h.start_lba ? h.start_lba : MODEL_DATA_LBA;
        return h.size;
    }
    return 0;
}

// Pull `size` bytes starting at `lba` into a big_alloc'd buffer.
static uint8_t* model_blob_read(uint64_t lba, uint64_t size){
    uint8_t* dst = (uint8_t*)big_alloc((uint32_t)size);
    if (!dst) return 0;
    // Lazily bring up virtio-blk once PMM is available (i.e. now).
    if (!g_vblk_io && !g_vblk_tried){
        g_vblk_tried = true;
        virtio_blk_init();
    }
    // Fast path: pull the entire model from a virtio-blk disk in ONE request.
    if (g_vblk_io){
        uint64_t sectors = (size + 511) / 512;
        serial_puts("[VBLK] reading "); serial_dec((uint32_t)(size>>20)); serial_puts("MB in one DMA...\n");
        if (virtio_blk_read(0, dst, (uint32_t)sectors)){
            serial_puts("[VBLK] model read OK\n");
            return dst;
        }
        serial_puts("[VBLK] read failed, falling back to IDE PIO\n");
    }
    uint64_t sectors = (size + 511) / 512;
    uint64_t done = 0;
    uint8_t sec[512];
    uint64_t i = 0;
    bool use_dma = (g_bm_base != 0) && (g_prdt_phys != 0);
    while (i < sectors){
        uint64_t remaining = sectors - i;
        uint8_t batch = (remaining > 255) ? 255 : (uint8_t)remaining;
        if (use_dma && (done + (uint64_t)batch*512 <= size)){
            // BM-DMA: 128 sectors = 64KiB straight into the (physically
            // contiguous, identity-mapped) destination buffer.
            if (!ata_read_dma_block((uint32_t)(lba + i), dst + done)){
                // Fall back to PIO for the rest if DMA hiccups.
                use_dma = false;
                ata_read_multi((uint32_t)(lba + i), batch, (uint16_t*)(dst + done));
            }
            done += (uint64_t)batch*512;
        } else if (done + (uint64_t)batch*512 <= size){
            ata_read_multi((uint32_t)(lba + i), batch, (uint16_t*)(dst + done));
            done += (uint64_t)batch*512;
        } else {
            ata_read_sector((uint32_t)(lba + i), (uint16_t*)sec);
            uint64_t chunk = size - done;
            memcpy_(dst + done, sec, (int)chunk);
            done += chunk;
        }
        i += batch;
        // Progress dots keep long transfers from looking like a hang.
        if ((i & 1023u) == 0u) term.put_char('.');
        if ((i & 32767u) == 0u) { serial_puts("[load "); serial_dec((uint32_t)(i>>17)); serial_puts("MB]\n"); }
    }
    return dst;
}

static void model_report_loaded(){
    const GGUFModelInfo* mi = qwen_info();
    if (!mi) return;
    char n[24];
    term.write("  arch="); term.write(mi->arch);
    term.write(" layers="); uint_to_str_k64(mi->block_count, n); term.write(n);
    term.write(" embd=");   uint_to_str_k64(mi->embed_length, n); term.write(n);
    term.write(" heads=");  uint_to_str_k64(mi->head_count, n); term.write(n);
    term.write("/");        uint_to_str_k64(mi->head_count_kv, n); term.write(n);
    term.write(" vocab=");  uint_to_str_k64(mi->vocab_size, n); term.write(n);
    term.write("\n  weights="); uint_to_str_k64(approx_mb_k64(g_model_bytes), n);
    term.write(n); term.write(" MB, runtime=");
    uint_to_str_k64((uint32_t)(qwen_runtime_bytes() >> 10), n);
    term.write(n); term.write(" KB\n");
}

static void cmd_model(const char* args){
    if(!*args){
        const struct KnownModel* d = ai_model_default();
        term.write("Model subsystem. Default: ");
        if (d) term.write(d->name); else term.write("(none)");
        term.write("\n  Usage: model [list|info|status|load|unload|set-default|recognize|download|run|selftest]\n");
        return;
    }
    char sub[16]; int si=0;
    while(*args && *args!=' ' && si<15) sub[si++]=*args++;
    sub[si]=0;
    while(*args==' ') args++;

    if(!strcmp_(sub,"env")){
        term.write("Environment: ");
        term.write(ai_env_desc());
        if (ai_env_is_vm())
            term.write("  (virtual machine -> built-in engine, no real weights)\n");
        else
            term.write("  (bare metal -> real transformer inference enabled)\n");
    }
    if(!strcmp_(sub,"list")){
        for (int i=0;i<ai_model_count();i++){
            const struct KnownModel* m = ai_model_get(i);
            term.write((m == ai_model_default()) ? " *" : "  ");
            term.write(m->name); term.write("  ");
            term.write(m->family); term.write("  ");
            term.write(m->params_str); term.write("  ");
            term.write(ai_model_fmt_name(m->fmt)); term.write("  ~");
            char sz[16]; uint_to_str_k64(approx_mb_k64(m->approx_size), sz);
            term.write(sz); term.write(" MB\n");
        }
        term.write("  (* = default model)\n");
    }
    else if(!strcmp_(sub,"info")){
        const struct KnownModel* m = *args ? ai_model_find(args) : ai_model_default();
        if(!m){ term.write("Unknown model. Use 'model list'.\n"); return; }
        term.write("Model: "); term.write(m->name); term.write("\n");
        term.write("  Family: "); term.write(m->family); term.write("\n");
        term.write("  Params: "); term.write(m->params_str); term.write("\n");
        term.write("  Format: "); term.write(ai_model_fmt_name(m->fmt)); term.write("\n");
        term.write("  Quant:  "); term.write(m->quant); term.write("\n");
        term.write("  Size: ~"); char sz[16]; uint_to_str_k64(approx_mb_k64(m->approx_size), sz);
        term.write(sz); term.write(" MB\n");
        term.write("  URL: "); term.write(m->url); term.write("\n");
    }
    else if(!strcmp_(sub,"set-default")){
        if(!*args){ term.write("Usage: model set-default <name>\n"); return; }
        if(ai_model_set_default(args)==0){ term.write("Default model set to: "); term.write(args); term.write("\n"); }
        else term.write("Unknown model name.\n");
    }
    else if(!strcmp_(sub,"recognize")){
        if(!*args){ term.write("Usage: model recognize <file>\n"); return; }
        uint8_t buf[8192];
        int n = gui_cb_read_file(1, args, buf, (int)sizeof(buf));
        if(n < 0) n = gui_cb_read_file(0, args, buf, (int)sizeof(buf));
        if(n < 0){ term.write("Cannot read file: "); term.write(args); term.write("\n"); return; }
        struct ModelInfo info;
        int fmt = ai_model_recognize_mem(buf, n, &info);
        term.write("Format: "); term.write(ai_model_fmt_name(fmt)); term.write("\n");
        term.write("  Family: "); term.write(info.family[0]?info.family:"(unknown)"); term.write("\n");
        term.write("  Name:   "); term.write(info.name[0]?info.name:"(unknown)"); term.write("\n");
        term.write("  Quant:  "); term.write(info.quant[0]?info.quant:"(unknown)"); term.write("\n");
        term.write("  Params: "); char pn[24]; uint_to_str_k64((uint32_t)info.params, pn); term.write(pn); term.write("\n");
        term.write("  Bytes read: "); char bn[16]; uint_to_str_k64((uint32_t)n, bn); term.write(bn); term.write("\n");
    }
    else if(!strcmp_(sub,"download")){
        const struct KnownModel* m = *args ? ai_model_find(args) : ai_model_default();
        if(!m){ term.write("Unknown model. Use 'model list'.\n"); return; }
        term.write("Model: "); term.write(m->name); term.write("\n");
        term.write("  URL: "); term.write(m->url); term.write("\n");
        term.write("  Expected size: ~"); char sz[16]; uint_to_str_k64(approx_mb_k64(m->approx_size), sz);
        term.write(sz); term.write(" MB\n");
        term.write("  Attempting fetch (best-effort)...\n");
        if(!g_net_initialized){ term.write("  Network not initialized. Use 'netstart'.\n"); return; }
        char buf[65536];
        int got = net_http_get(m->url, buf, (int)sizeof(buf));
        if(got > 0){
            term.write("  Fetched "); char gn[16]; uint_to_str_k64((uint32_t)got, gn); term.write(gn);
            term.write(" bytes. Full model is GB-scale and cannot be stored in this OS.\n");
        } else {
            term.write("  Fetch failed / host unreachable from emulated NIC.\n");
        }
    }
    else if(!strcmp_(sub,"run")){
        const struct KnownModel* m = 0;
        if(*args) m = ai_model_find(args);
        if(!m){
            if(!*args){ term.write("Usage: model run <name|file>\n"); return; }
            uint8_t buf[8192];
            int n = gui_cb_read_file(1, args, buf, (int)sizeof(buf));
            if(n < 0) n = gui_cb_read_file(0, args, buf, (int)sizeof(buf));
            if(n < 0){ term.write("Cannot read: "); term.write(args); term.write("\n"); return; }
            struct ModelInfo info;
            int fmt = ai_model_recognize_mem(buf, n, &info);
            term.write("Recognized "); term.write(ai_model_fmt_name(fmt));
            term.write(" ("); term.write(info.family[0]?info.family:info.name[0]?info.name:"?"); term.write(").\n");
            ai_model_set_active_name(args);
        } else {
            ai_model_set_active_name(m->name);
            term.write("Selected model: "); term.write(m->name);
            term.write(" ("); term.write(m->family); term.write(", "); term.write(m->params_str);
            term.write(", "); term.write(ai_model_fmt_name(m->fmt)); term.write(")\n");
        }
        if(!g_ai_initialized){ int r = ai_init("/boot/model.gguf"); if(r==0) g_ai_initialized = true; }
        if (ai_env_is_vm())
            term.write("VM detected: using built-in engine (no real weights loaded).\n");
        else
            term.write("Bare metal detected: real transformer inference enabled.\n");
        term.write("AI engine ready. Inference uses the on-board mini-engine;\n");
        term.write("the 1.7B weights need >1GB RAM, so this build routes them\n");
        term.write("through the built-in model. Use 'ask' / 'agent run'.\n");
    }
    else if(!strcmp_(sub,"adapter")){
        // Phase 0 verification: re-run the ggml adapter self-test on demand.
        gguf_adapter_selftest();
    }
    else if(!strcmp_(sub,"load")){
        if(qwen_ready()){
            term.write("A model is already loaded. Use 'model unload' first.\n");
            return;
        }
        uint64_t lba = MODEL_DATA_LBA, size = 0;
        if(*args){
            // Named file: SFS only holds small models, but it is handy for
            // the self-contained test GGUF.
            int idx = sfs.find(args);
            if(idx < 0){ term.write("No such SFS file: "); term.write(args); term.write("\n"); return; }
            uint8_t probe[512];
            int n = sfs.read(args, probe, (int)sizeof(probe));
            if(n <= 0){ term.write("Read failed.\n"); return; }
            term.write("Loading "); term.write(args); term.write(" from SFS");
            size = (uint64_t)n;
            g_model_blob = (uint8_t*)big_alloc((uint32_t)size);
            if(!g_model_blob){ term.write("\nOut of memory.\n"); return; }
            sfs.read(args, g_model_blob, (int)size);
        } else {
            size = model_blob_probe(&lba);
            if(!size){
                term.write("No model blob on disk (descriptor sector missing).\n");
                term.write("Rebuild the image with a GGUF in build/, or use\n");
                term.write("'model load <sfsfile>' for a small model.\n");
                return;
            }
            char n[24];
            term.write("Reading "); uint_to_str_k64((uint32_t)(size >> 10), n);
            term.write(n); term.write(" KB from LBA ");
            uint_to_str_k64((uint32_t)lba, n); term.write(n); term.write(" ");
            g_model_blob = model_blob_read(lba, size);
            if(!g_model_blob){ term.write("\nOut of memory (need ");
                uint_to_str_k64(approx_mb_k64(size), n); term.write(n);
                term.write(" MB contiguous).\n"); return; }
        }
        g_model_bytes = size;
        term.write("\nParsing GGUF...\n");
        int rc = qwen_load(g_model_blob, size, 512);
        if(rc != 0){
            term.write("Load failed: "); term.write(qwen_error()); term.write("\n");
            big_free(g_model_blob, (uint32_t)size);
            g_model_blob = 0; g_model_bytes = 0;
            return;
        }
        term.write("Model loaded.\n");
        model_report_loaded();
        ai_model_set_active_name(*args ? args : "disk-blob");
        term.write("Real inference is live - use 'ask <question>'.\n");
    }
    else if(!strcmp_(sub,"unload")){
        if(!qwen_ready()){ term.write("No model loaded.\n"); return; }
        qwen_unload();
        if(g_model_blob) big_free(g_model_blob, (uint32_t)g_model_bytes);
        g_model_blob = 0; g_model_bytes = 0;
        term.write("Model unloaded, weights freed.\n");
    }
    else if(!strcmp_(sub,"status")){
        if(qwen_ready()){
            term.write("Real GGUF model: LOADED\n");
            model_report_loaded();
        } else {
            term.write("Real GGUF model: not loaded\n");
            uint64_t lba = 0, size = model_blob_probe(&lba);
            if(size){
                char n[24];
                term.write("  A blob is available on disk: ");
                uint_to_str_k64((uint32_t)(size >> 10), n); term.write(n);
                term.write(" KB at LBA "); uint_to_str_k64((uint32_t)lba, n);
                term.write(n); term.write(" -> run 'model load'\n");
            } else {
                term.write("  No blob on disk. Built-in engine handles 'ask'.\n");
            }
        }
    }
    else if(!strcmp_(sub,"selftest")){
        uint8_t hdr[96]; int o=0;
        hdr[o++]='G'; hdr[o++]='G'; hdr[o++]='U'; hdr[o++]='F';
        hdr[o++]=3; hdr[o++]=0; hdr[o++]=0; hdr[o++]=0;
        for(int i=0;i<8;i++) hdr[o++]=0;
        hdr[o++]=1; for(int i=1;i<8;i++) hdr[o++]=0;
        hdr[o++]=19; for(int i=1;i<8;i++) hdr[o++]=0;
        { const char* k="general.architecture"; for(int i=0;i<19;i++) hdr[o++]=k[i]; }
        hdr[o++]=8; hdr[o++]=0; hdr[o++]=0; hdr[o++]=0;
        hdr[o++]=5; for(int i=1;i<8;i++) hdr[o++]=0;
        hdr[o++]='q'; hdr[o++]='w'; hdr[o++]='e'; hdr[o++]='n'; hdr[o++]='2';
        int len=o; while(o<(int)sizeof(hdr)) hdr[o++]=0;
        struct ModelInfo info;
        int fmt = ai_model_recognize_mem(hdr, len, &info);
        term.write("selftest -> fmt="); term.write(ai_model_fmt_name(fmt));
        term.write(" family="); term.write(info.family[0]?info.family:"?");
        term.write(" (expected GGUF/qwen2)\n");
    }
    else {
        term.write("Usage: model [list|info|status|load|unload|set-default|recognize|download|run|selftest]\n");
    }
}

// =====================================================================
//  Network commands
// =====================================================================
static void cmd_netinfo(){
    if(!g_net_initialized){
        term.write("Network not initialized. Use 'netstart' to initialize.\n");
        return;
    }
    char info[512];
    int n = net_status(info, sizeof(info));
    if(n > 0) term.write(info);
    else term.write("Failed to get network status.\n");
}

static void cmd_netstart(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("Initializing network...\n");
    serial_puts("[K8] Initializing network...\n");
    int ret = net_init();
    if(ret == 0){
        g_net_initialized = true;
        term.set_color(make_color(GREEN, BLACK));
        term.write("Network UP! HTTP server on http://");
        term.write(net_ip_str());
        term.write(":8080\n");
        term.set_color(make_color(CYAN, BLACK));
        term.write("  (QEMU: use -net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080)\n");
    } else {
        term.set_color(make_color(RED, BLACK));
        term.write("Network init failed! (NE2000 NIC not detected)\n");
        term.set_color(make_color(CYAN, BLACK));
        term.write("  Make sure QEMU has: -net nic,model=ne2k_isa\n");
    }
    term.set_color(make_color(WHITE, BLACK));
}

// =====================================================================
//  Switch to 32-bit kernel
//  Reads kernel.bin from disk (LBA 33) into memory at 0x10000,
//  then calls switch_to_32bit() which transitions the CPU back to
//  32-bit protected mode.
// =====================================================================
static void cmd_switch32(){
    term.set_color(make_color(CYAN, BLACK));
    term.write("\nSwitching to 32-bit kernel...\n");
    serial_puts("[K64] Switching to 32-bit kernel...\n");

    // ---- Load kernel.bin from disk to 0x10000 (64KB) ----
    term.write("Loading kernel.bin from LBA ");
    term.write_dec(KERNEL32_LBA);
    term.write("...\n");

    uint16_t* dst = (uint16_t*)KERNEL32_ADDR;
    serial_puts("[K64] loading kernel.bin to 0x10000...\n");
    for(int i=0; i<KERNEL32_SECTORS; i++){
        ata_read_sector(KERNEL32_LBA + i, dst);
        dst += 256;  // 512 bytes = 256 words
    }
    serial_puts("[K64] kernel.bin loaded at 0x10000\n");

    term.write("\n  Kernel32 loaded at 0x10000\n");
    term.write("  Transitioning to 32-bit protected mode...\n\n");
    serial_puts("[K64] Kernel32 loaded, calling switch_to_32bit()...\n");

    // ---- Call the mode switcher ----
    // This function never returns — it far-jumps to the 32-bit kernel
    switch_to_32bit();

    // Should never reach here
    term.set_color(make_color(RED, BLACK));
    term.write("ERROR: switch_to_32bit() returned!\n");
    serial_puts("[K64] ERROR: switch_to_32bit returned!\n");
    term.set_color(make_color(WHITE, BLACK));
}

// =====================================================================
//  GUI mode - enter graphical desktop environment
// =====================================================================
// Defined below with the rest of the Win32 subsystem commands; cmd_gui now
// needs it because the desktop can launch PE images.
static void win32_ensure_init();

// MiniCLR:  run a managed C# app (.mex) from the SFS volume (mirrors the
// 32-bit kernel's clr_ensure_init()).  mforms_boot() needs the CLR alive to
// host the managed Win11 shell; without it the 64-bit desktop is blank.
static bool g_clr_ready = false;
static void clr_ensure_init(){
    if (g_clr_ready) return;
    clr_init(
        [](const char* fn, unsigned char* buf, int bufsize) -> int {
            int r = -1;
            while (*fn == ' ') fn++;
            if (sfs.mounted)           r = sfs.read(fn, (uint8_t*)buf, bufsize);
            if (r < 0 && mkfs.mounted) r = mkfs.read(fn, (uint8_t*)buf, bufsize);
            return r;
        });
    g_clr_ready = true;
}

static void cmd_gui(const char* args){
    if(!g_vbe_active){
        term.set_color(make_color(RED, BLACK));
        term.write("GUI not available (VBE graphics mode not set).\n");
        term.set_color(make_color(CYAN, BLACK));
        term.write("  VBE requires BIOS boot with graphics mode support.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }
    if(!gui_available()){
        term.set_color(make_color(RED, BLACK));
        term.write("GUI initialization failed.\n");
        term.set_color(make_color(WHITE, BLACK));
        return;
    }

    // The desktop can EXECUTE real Windows PE images now (the Browser icon
    // runs chrome.exe through the PE32+ loader), so the Win32 subsystem --
    // registry, API tables and above all the SFS/MKFS file reader -- must be
    // live before we hand over.  Idempotent.
    win32_ensure_init();

    // The managed (C#) shell needs MiniCLR up before gui_enter() -> mforms_boot().
    clr_ensure_init();

    // Optional: `gui <app>` opens straight into an app after the desktop
    // starts, e.g. `gui browser`, `gui calc`, `gui files`.  Unknown names
    // just land on the plain desktop.
    if (args) {
        while (*args == ' ') args++;
        if (*args) {
            int aid = gui_app_id_by_name(args);
            if (aid >= 0) gui_set_startup_app(aid);
        }
    }

    term.set_color(make_color(CYAN, BLACK));
    term.write("Entering Win11 Desktop GUI mode...\n");
    serial_puts("[K64] Entering Win11 GUI mode\n");

    // Enter GUI - renders Win11-style desktop with top bar, icons, and cursor
    gui_enter();
    serial_puts("[M-GE]\n");   // after gui_enter returned
    gui_render();
    serial_puts("[M-GR]\n");   // after gui_render returned

    term.set_color(make_color(WHITE, BLACK));
}

// Disable auto-launch of the GUI on subsequent boots (debug / headless).
static void cmd_nogui(const char* args){
    (void)args;
    g_auto_gui = 0;
    term.set_color(make_color(CYAN, BLACK));
    term.write("Auto-GUI disabled. The desktop will not start automatically\n");
    term.write("on the next boot (this session keeps its current mode).\n");
    term.set_color(make_color(WHITE, BLACK));
}

// ---------------------------------------------------------------------
//  Voice engine bridge + command
// ---------------------------------------------------------------------
// Feed a recognised phrase to the managed Voice engine.  Safe no-op when
// the CLR / managed shell is not up (clr_call returns -1 and does nothing).
void gui_voice_say(const char* phrase){
    if (phrase == 0 || phrase[0] == 0) return;
    int32_t s = clr_new_str(phrase);
    int32_t a = s, r = 0;
    serial_puts("[VSAY]\n");
    clr_call("NexOS.Forms.Shell::VoiceSay", &a, 1, &r);
}
void gui_voice_set(int on){
    int32_t a = on ? 1 : 0, r = 0;
    clr_call("NexOS.Forms.Shell::VoiceSet", &a, 1, &r);
}

// Poll COM2 (0x2F8) for a framed voice phrase.  Voice input lives on its
// OWN serial port, separate from the COM1 console, so a flood of console
// output on COM1 can never deadlock the host->guest voice pipe (the two
// directions would otherwise share one bidirectional pipe and wedge).
// Protocol: a 0x02 (STX) framing byte, then a length byte (1..127), then
// that many UTF-8 bytes forming the phrase.  Any other leading byte is
// ignored.  A real recogniser pushes text the same way.
static void serial_voice_poll(void){
    const uint16_t VB = 0x2F8;                       // COM2 base
    uint8_t lsr = inb(VB + 5);
    if ((lsr & 1) == 0) return;                      // LSR: no data
    uint8_t b = inb(VB);
    if (b != 0x02) return;                          // not a voice frame
    serial_puts("[VSTX]\n");
    if ((inb(VB + 5) & 1) == 0) return;
    int n = inb(VB);
    if (n <= 0 || n > 127) return;
    char buf[128]; int i = 0;
    while (i < n) {
        if ((inb(VB + 5) & 1) == 0) continue;
        buf[i++] = (char)inb(VB);
    }
    buf[n] = 0;
    serial_puts("[VRX]\n");
    gui_voice_say(buf);
}

static void cmd_voice(const char* args){
    if (args == 0 || args[0] == 0) {
        term.write("usage: voice <on|off|phrase>\n");
        return;
    }
    while (*args == ' ') args++;
    if (strcmp_(args, "on") == 0) {
        gui_voice_set(1);
        term.write("Voice engine ON\n");
    } else if (strcmp_(args, "off") == 0) {
        gui_voice_set(0);
        term.write("Voice engine OFF\n");
    } else {
        gui_voice_say(args);
        term.write("voice phrase sent: ");
        term.write(args);
        term.write("\n");
    }
}

// =====================================================================
//  Tab completion  -  PowerShell-style command & filename completion
// =====================================================================

// All known command names (including PowerShell-style aliases)
static const char* g_cmd_table[] = {
    "help", "echo", "clear", "cls", "about", "history", "save", "load",
    "mkfs", "ls", "dir", "cat", "type", "touch", "rm", "del",
    "copy", "write", "mkdir", "md", "cd", "pwd",
    "lsfs", "catfs", "part", "mount", "lsfat", "fatinfo",
    "run", "runfs", "ai", "generate", "agent",
    "netinfo", "netstat", "netstart",
    "switch", "shutdown", "reboot", "exit",
    "gui", "nogui", "voice"
};
static const int g_cmd_count = sizeof(g_cmd_table)/sizeof(g_cmd_table[0]);

// Find all commands matching the given prefix. Returns count, fills matches[].
static int match_commands(const char* prefix, const char* matches[], int max){
    int count = 0;
    int prefixlen = strlen_(prefix);
    for(int i=0; i<g_cmd_count && count<max; i++){
        if(strncmp_(g_cmd_table[i], prefix, prefixlen) == 0)
            matches[count++] = g_cmd_table[i];
    }
    return count;
}

// Find all MKFS files in current directory matching the given prefix
static int match_files(const char* prefix, char matches[][FS_NAME_LEN], int max){
    if(!mkfs.mounted) return 0;
    int count = 0;
    int prefixlen = strlen_(prefix);
    for(int s=0; s<MKFS_TABLE_SECT && count<max; s++){
        ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
        for(int e=0; e<FS_ENTRY_PER_SEC; e++){
            FileEntry* fe = (FileEntry*)(g_fsbuf + e*FS_ENTRY_SIZE);
            if(fe->name[0] != 0 && fe->parent == g_cwd){
                if(strncmp_(fe->name, prefix, prefixlen) == 0){
                    int j=0;
                    while(fe->name[j] && j<FS_NAME_LEN-1){ matches[count][j]=fe->name[j]; j++; }
                    matches[count][j] = 0;
                    count++;
                }
            }
        }
    }
    return count;
}

// Perform tab completion on the current input buffer.
// Modifies inbuf and inlen in place. Returns true if completed.
static bool do_tab_complete(char* inbuf, int* inlen){
    // Make a null-terminated copy of current input
    inbuf[*inlen] = 0;

    // Find the last word boundary (space or start)
    int word_start = *inlen;
    while(word_start > 0 && inbuf[word_start-1] != ' ')
        word_start--;
    const char* partial = inbuf + word_start;
    int partial_len = *inlen - word_start;

    if(word_start == 0){
        // Completing a command name
        const char* cmd_matches[16];
        int n = match_commands(partial, cmd_matches, 16);
        if(n == 1){
            // Single match: complete it
            int mlen = strlen_(cmd_matches[0]);
            for(int i=partial_len; i<mlen; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = cmd_matches[0][i];
                    term.put_char(cmd_matches[0][i]);
                    (*inlen)++;
                }
            }
            // Add a space after command
            if(*inlen < HIST_LEN-1){
                inbuf[*inlen] = ' ';
                term.put_char(' ');
                (*inlen)++;
            }
            return true;
        } else if(n > 1){
            // Multiple matches: find common prefix and show options
            int common = partial_len;
            bool done = false;
            while(!done){
                char c = cmd_matches[0][common];
                if(c == 0) break;
                bool all_match = true;
                for(int i=1; i<n; i++){
                    if(cmd_matches[i][common] != c){ all_match = false; break; }
                }
                if(!all_match) break;
                common++;
            }
            // Complete up to common prefix
            for(int i=partial_len; i<common; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = cmd_matches[0][i];
                    term.put_char(cmd_matches[0][i]);
                    (*inlen)++;
                }
            }
            // Show all matches
            term.put_char('\n');
            term.set_color(make_color(CYAN, BLACK));
            for(int i=0; i<n; i++){
                term.write(cmd_matches[i]);
                term.write("  ");
            }
            term.set_color(make_color(LIGHT_GREY, BLACK));
            term.put_char('\n');
            return true;
        }
    } else {
        // Completing a filename
        char file_matches[16][FS_NAME_LEN];
        int n = match_files(partial, file_matches, 16);
        if(n == 1){
            int mlen = strlen_(file_matches[0]);
            for(int i=partial_len; i<mlen; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = file_matches[0][i];
                    term.put_char(file_matches[0][i]);
                    (*inlen)++;
                }
            }
            return true;
        } else if(n > 1){
            // Find common prefix
            int common = partial_len;
            bool done = false;
            while(!done){
                char c = file_matches[0][common];
                if(c == 0) break;
                bool all_match = true;
                for(int i=1; i<n; i++){
                    if(file_matches[i][common] != c){ all_match = false; break; }
                }
                if(!all_match) break;
                common++;
            }
            for(int i=partial_len; i<common; i++){
                if(*inlen < HIST_LEN-1){
                    inbuf[*inlen] = file_matches[0][i];
                    term.put_char(file_matches[0][i]);
                    (*inlen)++;
                }
            }
            // Show all matches
            term.put_char('\n');
            term.set_color(make_color(CYAN, BLACK));
            for(int i=0; i<n; i++){
                term.write(file_matches[i]);
                term.write("  ");
            }
            term.set_color(make_color(LIGHT_GREY, BLACK));
            term.put_char('\n');
            return true;
        }
    }
    // No match: beep
    term.put_char('\a');
    return false;
}

// =====================================================================
//  Win64 subsystem (PE32+ loader) -  only meaningful in long mode
// =====================================================================
static bool g_win32_ready = false;
static void win32_ensure_init(){
    if (g_win32_ready) return;
    win32_init(
        [](const char* fn, uint8_t* buf, int bufsize) -> int {
            int r = -1;
            while (*fn == ' ') fn++;
            if (sfs.mounted)  { r = sfs.read(fn, buf, bufsize); }
            // Fall back to the writable mkfs volume so a .exe the user
            // dropped there (File Explorer "This PC") also runs, matching
            // the 32-bit kernel's reader.
            if (r < 0 && mkfs.mounted) { r = mkfs.read(fn, buf, bufsize); }
            return r;
        },
        [](const char* text) {
            term.write(text);
            serial_puts(text);
        }
    );
    g_win32_ready = true;
}

static void cmd_winapp(const char* args){
    win32_ensure_init();
    while (*args == ' ') args++;
    int info_only = 0;
    if (args[0] == '/' && (args[1] == 'i' || args[1] == 'I') &&
        (args[2] == 0 || args[2] == ' ')) {
        info_only = 1;
        args += 2;
        while (*args == ' ') args++;
    }
    if (!args[0]) {
        term.write("Usage: winapp [/i] <file.exe> [args]\n");
        term.write("       Runs a 32-bit PE32 or 64-bit PE32+ image.\n");
        return;
    }
    char file[64]; int fi = 0;
    while (*args && *args != ' ' && fi < 63) file[fi++] = *args++;
    file[fi] = 0;
    while (*args == ' ') args++;

    term.set_color(make_color(CYAN, BLACK));
    term.write("=== NexOS Win64 Subsystem ===\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    term.write("Image  : "); term.write(file); term.put_char('\n');
    if (args[0]) { term.write("Args   : "); term.write(args); term.put_char('\n'); }

    int rc = win32_run(file, args, info_only);

    const char* rep = win32_last_report();
    if (rep && rep[0]) { term.write(rep); serial_puts(rep); }

    if (rc != 0) {
        term.set_color(make_color(RED, BLACK));
        const char* msg = "[X] Load failed.\n";
        switch (rc) {
            case -1: msg = "[X] File not found in SFS.\n"; break;
            case -2: msg = "[X] Not a PE32 / PE32+ executable (missing MZ/PE signature).\n"; break;
            case -3: msg = "[X] Unsupported PE (needs 32-bit i386 PE32, or 64-bit PE32+ under the 64-bit kernel).\n"; break;
            case -4: msg = "[X] Out of memory while mapping the image.\n"; break;
            case -5: msg = "[X] Unresolved imports - see the list above.\n"; break;
            case -6: msg = "[X] Image too large for the loader (192 KiB limit).\n"; break;
            default: break;
        }
        term.write(msg);
        serial_puts(msg);
        term.set_color(make_color(LIGHT_GREY, BLACK));
        return;
    }
}

// =====================================================================
//  Command dispatcher
// =====================================================================

// =====================================================================
//  Security subsystem  (ported from kernel.cpp for 32/64-bit parity)
//  - Login system (struct User, FNV-1a password hash, userdb)
//  - Unix-style rwx owner permissions (PermEntry table, perm_check)
//  - Y/N consent engine UI hook (perm_ui_console) for perm.cpp
// =====================================================================

// ---- Permission bits (standard 9-bit rwxrwxrwx) ----
#define P_OWNER_R  0x100
#define P_OWNER_W  0x080
#define P_OWNER_X  0x040
#define P_GRP_R    0x020
#define P_GRP_W    0x010
#define P_GRP_X    0x008
#define P_OTH_R    0x004
#define P_OTH_W    0x002
#define P_OTH_X    0x001
#define DEFAULT_FILE_MODE  (P_OWNER_R|P_OWNER_W|P_GRP_R|P_OTH_R)
#define DEFAULT_DIR_MODE   (P_OWNER_R|P_OWNER_W|P_OWNER_X|P_GRP_R|P_GRP_X|P_OTH_R|P_OTH_X)

#define USER_NAME_LEN   32
#define USER_GROUP_LEN  32
#define MAX_USERS       16
#define MAX_PERMS       64

struct User {
    bool     exists;
    char     name[USER_NAME_LEN];
    uint32_t uid;
    uint32_t gid;
    char     group[USER_GROUP_LEN];
    char     hash[17];            // FNV-1a 32-bit, 8 hex chars + null
};
static User     g_users[MAX_USERS];
static int      g_user_count = 0;
static int      g_login_idx = -1;    // current logged-in user (-1 = none)
static uint32_t g_euid = 0;          // effective uid (root when sudo active)
static bool     g_sudo_active = false;

struct PermEntry {
    char     name[FS_NAME_LEN];
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;               // 9-bit rwxrwxrwx
};
static PermEntry g_perms[MAX_PERMS];
static int       g_perm_count = 0;

static void run_command(const char* line);   // forward

// ---- FNV-1a hash ----
static uint32_t fnv1a(const char* s){
    uint32_t h = 2166136261u;
    while(*s){ h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
static void hash_password(const char* user, const char* pw, char out[17]){
    char tmp[64]; int i = 0;
    while(user[i] && i < 31){ tmp[i] = user[i]; i++; }
    const char* p = pw;
    while(*p && i < 63){ tmp[i++] = *p++; }
    tmp[i] = 0;
    uint32_t h = fnv1a(tmp);
    for(int j = 0; j < 8; j++){
        uint8_t d = (h >> (28 - j*4)) & 0xF;
        out[j] = (d < 10) ? ('0' + d) : ('a' + d - 10);
    }
    out[8] = 0;
}

// ---- DB text helpers ----
static void db_append(char* buf, int* len, int cap, const char* s){
    while(*s && *len < cap - 1) buf[(*len)++] = *s++;
}
static void db_append_u32(char* buf, int* len, int cap, uint32_t v){
    char t[12]; int ti = 0;
    if(v == 0) t[ti++] = '0';
    while(v && ti < 11){ t[ti++] = '0' + (int)(v % 10); v /= 10; }
    while(ti > 0 && *len < cap - 1) buf[(*len)++] = t[--ti];
}
static int db_split(char* line, char* fields[], int max){
    int n = 0; char* p = line;
    while(*p && n < max){
        fields[n++] = p;
        while(*p && *p != ':') p++;
        if(*p == ':') *p++ = 0;
    }
    return n;
}

// ---- User database (persisted as "shadow" in MKFS root) ----
static void userdb_save(){
    if(!mkfs.mounted) return;
    char buf[512]; int len = 0;
    for(int i = 0; i < g_user_count; i++){
        if(!g_users[i].exists) continue;
        db_append(buf, &len, sizeof(buf), g_users[i].name);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_users[i].uid);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_users[i].gid);
        db_append(buf, &len, sizeof(buf), ":");
        db_append(buf, &len, sizeof(buf), g_users[i].group);
        db_append(buf, &len, sizeof(buf), ":");
        db_append(buf, &len, sizeof(buf), g_users[i].hash);
        db_append(buf, &len, sizeof(buf), "\n");
    }
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    mkfs.create("shadow", (const uint8_t*)buf, len);
    g_cwd = saved;
}
static void userdb_load(){
    g_user_count = 0;
    for(int i = 0; i < MAX_USERS; i++){ g_users[i].exists = false; g_users[i].name[0] = 0; }
    if(!mkfs.mounted) return;
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    int rd = mkfs.read("shadow", g_iobuf, FS_IOBUF_SIZE - 1);
    g_cwd = saved;
    if(rd <= 0) return;
    ((char*)g_iobuf)[rd] = 0;
    char* p = (char*)g_iobuf;
    while(*p && g_user_count < MAX_USERS){
        char* nl = p;
        while(*nl && *nl != '\n') nl++;
        if(*nl == '\n') *nl = 0;
        char* f[5];
        int n = db_split(p, f, 5);
        if(n >= 5 && f[0][0]){
            int idx = g_user_count++;
            g_users[idx].exists = true;
            int i = 0; while(f[0][i] && i < USER_NAME_LEN - 1){ g_users[idx].name[i] = f[0][i]; i++; }
            g_users[idx].name[i] = 0;
            g_users[idx].uid = 0; { const char* q = f[1]; while(*q){ g_users[idx].uid = g_users[idx].uid*10 + (*q - '0'); q++; } }
            g_users[idx].gid = 0; { const char* q = f[2]; while(*q){ g_users[idx].gid = g_users[idx].gid*10 + (*q - '0'); q++; } }
            i = 0; while(f[3][i] && i < USER_GROUP_LEN - 1){ g_users[idx].group[i] = f[3][i]; i++; }
            g_users[idx].group[i] = 0;
            i = 0; while(f[4][i] && i < 16){ g_users[idx].hash[i] = f[4][i]; i++; }
            g_users[idx].hash[i] = 0;
        }
        if(*nl == 0) break;
        p = nl + 1;
    }
}
static void seed_default_users(){
    if(g_user_count > 0) return;
    g_users[0].exists = true;
    int i = 0; const char* rn = "root";
    while(rn[i] && i < USER_NAME_LEN - 1){ g_users[0].name[i] = rn[i]; i++; }
    g_users[0].name[i] = 0;
    g_users[0].uid = 0; g_users[0].gid = 0;
    i = 0; const char* rg = "root";
    while(rg[i] && i < USER_GROUP_LEN - 1){ g_users[0].group[i] = rg[i]; i++; }
    g_users[0].group[i] = 0;
    hash_password("root", "admin", g_users[0].hash);
    g_users[1].exists = true;
    i = 0; const char* gn = "guest";
    while(gn[i] && i < USER_NAME_LEN - 1){ g_users[1].name[i] = gn[i]; i++; }
    g_users[1].name[i] = 0;
    g_users[1].uid = 1000; g_users[1].gid = 1000;
    i = 0; const char* gg = "users";
    while(gg[i] && i < USER_GROUP_LEN - 1){ g_users[1].group[i] = gg[i]; i++; }
    g_users[1].group[i] = 0;
    hash_password("guest", "guest", g_users[1].hash);
    g_user_count = 2;
    userdb_save();
}

// ---- Permission table (persisted as "permdb" in MKFS root) ----
static void permdb_save(){
    if(!mkfs.mounted) return;
    char buf[2048]; int len = 0;
    for(int i = 0; i < g_perm_count; i++){
        db_append(buf, &len, sizeof(buf), g_perms[i].name);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_perms[i].uid);
        db_append(buf, &len, sizeof(buf), ":");
        db_append_u32(buf, &len, sizeof(buf), g_perms[i].gid);
        db_append(buf, &len, sizeof(buf), ":");
        char m3[4];
        m3[0] = '0' + ((g_perms[i].mode >> 6) & 7);
        m3[1] = '0' + ((g_perms[i].mode >> 3) & 7);
        m3[2] = '0' + (g_perms[i].mode & 7);
        m3[3] = 0;
        db_append(buf, &len, sizeof(buf), m3);
        db_append(buf, &len, sizeof(buf), "\n");
    }
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    mkfs.create("permdb", (const uint8_t*)buf, len);
    g_cwd = saved;
}
static void permdb_load(){
    g_perm_count = 0;
    if(!mkfs.mounted) return;
    uint16_t saved = g_cwd;
    g_cwd = FS_ROOT_PARENT;
    int rd = mkfs.read("permdb", g_iobuf, FS_IOBUF_SIZE - 1);
    g_cwd = saved;
    if(rd <= 0) return;
    ((char*)g_iobuf)[rd] = 0;
    char* p = (char*)g_iobuf;
    while(*p && g_perm_count < MAX_PERMS){
        char* nl = p;
        while(*nl && *nl != '\n') nl++;
        if(*nl == '\n') *nl = 0;
        char* f[4];
        int n = db_split(p, f, 4);
        if(n >= 4 && f[0][0]){
            int idx = g_perm_count++;
            int i = 0; while(f[0][i] && i < FS_NAME_LEN - 1){ g_perms[idx].name[i] = f[0][i]; i++; }
            g_perms[idx].name[i] = 0;
            g_perms[idx].uid = 0; { const char* q = f[1]; while(*q){ g_perms[idx].uid = g_perms[idx].uid*10 + (*q - '0'); q++; } }
            g_perms[idx].gid = 0; { const char* q = f[2]; while(*q){ g_perms[idx].gid = g_perms[idx].gid*10 + (*q - '0'); q++; } }
            uint32_t m = 0; { const char* q = f[3]; while(*q){ m = m*8 + (*q - '0'); q++; } }
            g_perms[idx].mode = (uint16_t)m;
        }
        if(*nl == 0) break;
        p = nl + 1;
    }
}
static void perm_set(const char* name, uint32_t uid, uint32_t gid, uint16_t mode){
    for(int i = 0; i < g_perm_count; i++){
        if(strcmp_(g_perms[i].name, name) == 0){
            g_perms[i].uid = uid; g_perms[i].gid = gid; g_perms[i].mode = mode;
            permdb_save();
            return;
        }
    }
    if(g_perm_count < MAX_PERMS){
        int idx = g_perm_count++;
        int i = 0; while(name[i] && i < FS_NAME_LEN - 1){ g_perms[idx].name[i] = name[i]; i++; }
        g_perms[idx].name[i] = 0;
        g_perms[idx].uid = uid; g_perms[idx].gid = gid; g_perms[idx].mode = mode;
        permdb_save();
    }
}
static uint16_t perm_mode_of(const char* name){
    for(int i = 0; i < g_perm_count; i++)
        if(strcmp_(g_perms[i].name, name) == 0) return g_perms[i].mode;
    return (uint16_t)DEFAULT_FILE_MODE;
}

// ---- Current identity ----
static int cur_uid(){ return g_sudo_active ? 0 : (g_login_idx >= 0 ? (int)g_users[g_login_idx].uid : 0); }
static int cur_gid(){ return g_sudo_active ? 0 : (g_login_idx >= 0 ? (int)g_users[g_login_idx].gid : 0); }
static bool is_root(){ return cur_uid() == 0; }

// ---- Permission check: need = 'r' | 'w' | 'x' ----
static bool perm_check(const char* name, char need, bool quiet){
    if(is_root()) return true;
    uint16_t mode = perm_mode_of(name);
    uint32_t cu = (uint32_t)cur_uid();
    uint32_t cg = (uint32_t)cur_gid();
    uint8_t bits = 0;
    // owner = the file's recorded uid (root owns everything by default)
    uint32_t owner_uid = 0;
    for(int i = 0; i < g_perm_count; i++)
        if(strcmp_(g_perms[i].name, name) == 0){ owner_uid = g_perms[i].uid; break; }
    if(cu == owner_uid){
        bits = (need=='r') ? (mode & P_OWNER_R) : (need=='w') ? (mode & P_OWNER_W) : (mode & P_OWNER_X);
    } else if(cg == 0){
        bits = (need=='r') ? (mode & P_GRP_R) : (need=='w') ? (mode & P_GRP_W) : (mode & P_GRP_X);
    } else {
        bits = (need=='r') ? (mode & P_OTH_R) : (need=='w') ? (mode & P_OTH_W) : 0;
    }
    if(!bits){
        if(!quiet){
            term.write("Permission denied: "); term.write(name);
            term.write(" (need "); term.put_char(need); term.write(")\n");
        }
        return false;
    }
    return true;
}

// ---- Read a line from keyboard (password: hidden with '*') ----
static void read_line_quiet(char* buf, int* len, bool hidden){
    *len = 0;
    for(;;){
        uint8_t st = inb(0x64);
        if(st == 0xFF) st = 0;
        if(!(st & 0x01)) continue;
        uint8_t data = inb(0x60);
        if(st & 0x20) continue;
        KbdEvent e = kbd.process(data);
        if(e.type != K_CHAR) continue;
        if(e.ch == '\n'){ term.put_char('\n'); term.render(); buf[*len] = 0; return; }
        if(e.ch == '\b'){
            if(*len > 0){ (*len)--; term.put_char('\b'); term.render(); }
            continue;
        }
        if(*len < 63){
            buf[(*len)++] = e.ch;
            term.put_char(hidden ? '*' : e.ch);
            term.render();
        }
    }
}

static void login_prompt(){
    term.write("\n=== NexOS Security Login ===\n");
    for(;;){
        term.set_color(make_color(LIGHT_GREY, BLACK));
        term.write("login: ");
        term.render();
        char user[32]; int ulen = 0;
        read_line_quiet(user, &ulen, false);
        int idx = -1;
        for(int i = 0; i < g_user_count; i++){
            if(g_users[i].exists && strcmp_(g_users[i].name, user) == 0){ idx = i; break; }
        }
        if(idx < 0){ term.write("Login incorrect\n"); continue; }
        term.write("Password: ");
        term.render();
        char pw[64]; int plen = 0;
        read_line_quiet(pw, &plen, true);
        char hash[17];
        hash_password(g_users[idx].name, pw, hash);
        if(strcmp_(hash, g_users[idx].hash) != 0){ term.write("Login incorrect\n"); continue; }
        g_login_idx = idx;
        g_euid = g_users[idx].uid;
        g_sudo_active = false;
        serial_puts("[K64-LOGIN] OK user="); serial_puts(g_users[idx].name); serial_puts("\n");
        term.write("Welcome, "); term.write(g_users[idx].name);
        term.write("! uid="); term.write_dec((int)g_users[idx].uid);
        term.write(" gid="); term.write_dec((int)g_users[idx].gid);
        term.write(" group="); term.write(g_users[idx].group);
        term.write("\n");
        term.render();
        return;
    }
}

// ---- User commands ----
static void cmd_whoami(){
    if(g_sudo_active){ term.write("root\n"); serial_puts("[K64-WHOAMI] root\n"); return; }
    if(g_login_idx < 0){ term.write("nobody\n"); serial_puts("[K64-WHOAMI] nobody\n"); return; }
    term.write(g_users[g_login_idx].name); term.put_char('\n');
    serial_puts("[K64-WHOAMI] "); serial_puts(g_users[g_login_idx].name); serial_puts("\n");
}
static void cmd_id(){
    term.write("uid="); term.write_dec(cur_uid());
    term.write("(");
    if(is_root() && g_sudo_active) term.write("root");
    else if(g_login_idx >= 0) term.write(g_users[g_login_idx].name);
    else term.write("nobody");
    term.write(") gid="); term.write_dec(cur_gid());
    term.write(" groups=");
    if(g_login_idx >= 0) term.write(g_users[g_login_idx].group);
    else term.write("none");
    term.put_char('\n');
}
static void cmd_users(){
    term.write("Users on NexOS:\n");
    for(int i = 0; i < g_user_count; i++){
        if(!g_users[i].exists) continue;
        term.write("  "); term.write(g_users[i].name);
        term.write("  uid="); term.write_dec((int)g_users[i].uid);
        term.write("  gid="); term.write_dec((int)g_users[i].gid);
        term.write("  group="); term.write(g_users[i].group);
        if(g_users[i].uid == 0) term.write("  [root]");
        if(i == g_login_idx) term.write("  <-- you");
        term.put_char('\n');
    }
}
static void cmd_login(const char* name){
    if(!name[0]){ term.write("Usage: login <username>\n"); return; }
    int idx = -1;
    for(int i = 0; i < g_user_count; i++){
        if(g_users[i].exists && strcmp_(g_users[i].name, name) == 0){ idx = i; break; }
    }
    if(idx < 0){ term.write("User not found: "); term.write(name); term.put_char('\n'); return; }
    term.write("Password: "); term.render();
    char pw[64]; int plen = 0;
    read_line_quiet(pw, &plen, true);
    char hash[17];
    hash_password(g_users[idx].name, pw, hash);
    if(strcmp_(hash, g_users[idx].hash) != 0){ term.write("Incorrect password.\n"); return; }
    g_login_idx = idx; g_euid = g_users[idx].uid; g_sudo_active = false;
    term.write("Logged in as "); term.write(g_users[idx].name); term.write(".\n");
}
static void cmd_logout(){
    if(g_login_idx < 0){ term.write("Not logged in.\n"); return; }
    term.write("Logged out. Returning to login prompt.\n");
    g_login_idx = -1; g_euid = 0; g_sudo_active = false;
    login_prompt();
}
static void cmd_su(const char* name){
    const char* target = name[0] ? name : "root";
    int idx = -1;
    for(int i = 0; i < g_user_count; i++){
        if(g_users[i].exists && strcmp_(g_users[i].name, target) == 0){ idx = i; break; }
    }
    if(idx < 0){ term.write("User not found: "); term.write(target); term.put_char('\n'); return; }
    term.write("Password: "); term.render();
    char pw[64]; int plen = 0;
    read_line_quiet(pw, &plen, true);
    char hash[17];
    hash_password(g_users[idx].name, pw, hash);
    if(strcmp_(hash, g_users[idx].hash) != 0){ term.write("Incorrect password.\n"); return; }
    g_login_idx = idx; g_euid = g_users[idx].uid; g_sudo_active = false;
    term.write("Switched to user "); term.write(g_users[idx].name); term.write(".\n");
    serial_puts("[K64-SU] "); serial_puts(g_users[idx].name); serial_puts("\n");
}
static void cmd_useradd(const char* args){
    if(!is_root()){ term.write("Permission denied: only root can add users (use 'sudo useradd ...').\n"); return; }
    char name[USER_NAME_LEN], pw[64];
    int i = 0;
    while(args[i] && args[i] != ' ' && i < USER_NAME_LEN - 1){ name[i] = args[i]; i++; }
    name[i] = 0;
    while(args[i] == ' ') i++;
    int j = 0;
    while(args[i] && args[i] != ' ' && j < 63){ pw[j++] = args[i++]; }
    pw[j] = 0;
    if(!name[0]){ term.write("Usage: useradd <username> [password]\n"); return; }
    for(const char* q = name; *q; q++){
        if(!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')){
            term.write("Invalid username (a-z A-Z 0-9 _ only)\n"); return;
        }
    }
    for(int k = 0; k < g_user_count; k++){
        if(g_users[k].exists && strcmp_(g_users[k].name, name) == 0){
            term.write("User already exists: "); term.write(name); term.put_char('\n'); return;
        }
    }
    if(g_user_count >= MAX_USERS){ term.write("User table full (max "); term.write_dec(MAX_USERS); term.write(")\n"); return; }
    if(!pw[0]){ int k = 0; const char* d = "123456"; while(d[k] && k < 63){ pw[k] = d[k]; k++; } pw[k] = 0; }
    int idx = g_user_count++;
    g_users[idx].exists = true;
    i = 0; while(name[i] && i < USER_NAME_LEN - 1){ g_users[idx].name[i] = name[i]; i++; }
    g_users[idx].name[i] = 0;
    g_users[idx].uid = 1000 + (idx - 1);
    g_users[idx].gid = g_users[idx].uid;
    i = 0; const char* grp = "users";
    while(grp[i] && i < USER_GROUP_LEN - 1){ g_users[idx].group[i] = grp[i]; i++; }
    g_users[idx].group[i] = 0;
    hash_password(name, pw, g_users[idx].hash);
    userdb_save();
    term.write("User added: "); term.write(name);
    term.write(" (uid="); term.write_dec((int)g_users[idx].uid);
    term.write(", password="); term.write(pw[0] ? pw : "(none)");
    term.write(")\n");
}
static void cmd_deluser(const char* name){
    if(!is_root()){ term.write("Permission denied: only root can delete users.\n"); return; }
    if(!name[0]){ term.write("Usage: deluser <username>\n"); return; }
    if(strcmp_(name, "root") == 0){ term.write("Cannot delete root.\n"); return; }
    int idx = -1;
    for(int i = 0; i < g_user_count; i++){
        if(g_users[i].exists && strcmp_(g_users[i].name, name) == 0){ idx = i; break; }
    }
    if(idx < 0){ term.write("User not found: "); term.write(name); term.put_char('\n'); return; }
    if(idx == g_login_idx){ term.write("Cannot delete the user you are logged in as.\n"); return; }
    g_users[idx].exists = false; g_users[idx].name[0] = 0;
    userdb_save();
    term.write("User deleted: "); term.write(name); term.put_char('\n');
}
static void cmd_passwd(const char* args){
    char target[USER_NAME_LEN];
    int i = 0;
    while(args[i] && args[i] != ' ' && i < USER_NAME_LEN - 1){ target[i] = args[i]; i++; }
    target[i] = 0;
    int idx = g_login_idx;
    if(target[0]){
        int t = -1;
        for(int k = 0; k < g_user_count; k++){
            if(g_users[k].exists && strcmp_(g_users[k].name, target) == 0){ t = k; break; }
        }
        if(t < 0){ term.write("User not found: "); term.write(target); term.put_char('\n'); return; }
        if(t != g_login_idx && !is_root()){ term.write("Permission denied: only root can change other users' passwords.\n"); return; }
        idx = t;
    }
    if(idx < 0){ term.write("Not logged in.\n"); return; }
    if(!is_root()){
        term.write("Current password: "); term.render();
        char old[64]; int ol = 0;
        read_line_quiet(old, &ol, true);
        char h[17];
        hash_password(g_users[idx].name, old, h);
        if(strcmp_(h, g_users[idx].hash) != 0){ term.write("Incorrect password.\n"); return; }
    }
    term.write("New password: "); term.render();
    char pw1[64]; int l1 = 0;
    read_line_quiet(pw1, &l1, true);
    term.write("Confirm: "); term.render();
    char pw2[64]; int l2 = 0;
    read_line_quiet(pw2, &l2, true);
    if(strcmp_(pw1, pw2) != 0){ term.write("Passwords do not match.\n"); return; }
    if(!pw1[0]){ term.write("Password cannot be empty.\n"); return; }
    hash_password(g_users[idx].name, pw1, g_users[idx].hash);
    userdb_save();
    term.write("Password changed for "); term.write(g_users[idx].name); term.write(".\n");
}
static void cmd_chmod(const char* args){
    char m3[4]; char fname[FS_NAME_LEN];
    int i = 0;
    while(args[i] && args[i] != ' ' && i < 3){ m3[i] = args[i]; i++; }
    m3[i] = 0;
    while(args[i] == ' ') i++;
    int j = 0;
    while(args[i] && args[i] != ' ' && j < FS_NAME_LEN - 1){ fname[j++] = args[i++]; }
    fname[j] = 0;
    if(!fname[0]){ term.write("Usage: chmod <mode3> <file>   (e.g. chmod 644 file.txt)\n"); return; }
    if(strlen_(m3) != 3){ term.write("Mode must be 3 octal digits (e.g. 644, 755)\n"); return; }
    uint32_t m = 0;
    for(int k = 0; k < 3; k++){
        if(m3[k] < '0' || m3[k] > '7'){ term.write("Mode must be octal (0-7)\n"); return; }
        m = m * 8 + (m3[k] - '0');
    }
    uint32_t uid = 0, gid = 0; uint16_t omode = perm_mode_of(fname);
    (void)omode;
    if(!is_root() && (uint32_t)cur_uid() != uid){
        term.write("Permission denied: only the owner or root can chmod "); term.write(fname); term.put_char('\n');
        return;
    }
    uint16_t mode = (uint16_t)(m & 0x1FF);
    perm_set(fname, uid, gid, mode);
    if(mkfs.mounted){
        for(int s = 0; s < MKFS_TABLE_SECT; s++){
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for(int e = 0; e < FS_ENTRY_PER_SEC; e++){
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if(fe->name[0] != 0 && fe->parent == g_cwd && strcmp_(fe->name, fname) == 0){
                    fe->reserved = (uint8_t)(mode & 0xFF);
                    ata_write_sector(MKFS_TABLE_LBA + s, (const uint16_t*)g_fsbuf);
                    break;
                }
            }
        }
    }
    term.write("chmod: "); term.write(fname);
    term.write(" -> "); term.write(m3); term.write(" (octal)\n");
}
static void cmd_stat(const char* name){
    if(!name[0]){ term.write("Usage: stat <file>\n"); return; }
    uint16_t mode = perm_mode_of(name);
    term.write("File: "); term.write(name); term.put_char('\n');
    term.write("  mode: ");
    char bits[10];
    bits[0] = (mode & P_OWNER_R) ? 'r' : '-';
    bits[1] = (mode & P_OWNER_W) ? 'w' : '-';
    bits[2] = (mode & P_OWNER_X) ? 'x' : '-';
    bits[3] = (mode & P_GRP_R)   ? 'r' : '-';
    bits[4] = (mode & P_GRP_W)   ? 'w' : '-';
    bits[5] = (mode & P_GRP_X)   ? 'x' : '-';
    bits[6] = (mode & P_OTH_R)   ? 'r' : '-';
    bits[7] = (mode & P_OTH_W)   ? 'w' : '-';
    bits[8] = '-'; bits[9] = 0;
    term.write(bits);
    term.write("  ");
    term.write_dec((int)((mode >> 6) & 7));
    term.write_dec((int)((mode >> 3) & 7));
    term.write_dec((int)(mode & 7));
    term.write(" (octal)\n");
    if(mkfs.mounted){
        for(int s = 0; s < MKFS_TABLE_SECT; s++){
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for(int e = 0; e < FS_ENTRY_PER_SEC; e++){
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if(fe->name[0] != 0 && fe->parent == g_cwd && strcmp_(fe->name, name) == 0){
                    term.write("  FS entry: size="); term.write_dec((int)fe->size);
                    term.write(" type="); term.write_dec((int)fe->type);
                    term.write(" mode=0x"); term.write_hex(fe->reserved);
                    term.put_char('\n');
                    return;
                }
            }
        }
    }
}
static void cmd_sudo(const char* args){
    if(!args[0]){ term.write("Usage: sudo <command> [args...]\n"); return; }
    if(g_login_idx < 0){ term.write("Not logged in.\n"); return; }
    term.write("[sudo] password for "); term.write(g_users[g_login_idx].name); term.write(": ");
    term.render();
    char pw[64]; int plen = 0;
    read_line_quiet(pw, &plen, true);
    char hash[17];
    hash_password(g_users[g_login_idx].name, pw, hash);
    if(strcmp_(hash, g_users[g_login_idx].hash) != 0){ term.write("Sorry, try again.\n"); return; }
    term.write("OK, running as root: "); term.write(args); term.put_char('\n');
    bool saved = g_sudo_active;
    g_sudo_active = true;
    run_command(args);
    g_sudo_active = saved;
    if(!saved){ term.write("[sudo] returned to "); term.write(g_users[g_login_idx].name); term.put_char('\n'); }
}
static void cmd_perm(const char* args){
    if(args[0] == 'r'){ perm_reset(); term.write("All remembered permission grants cleared.\n"); return; }
    perm_dump();
    term.write("Remembered grants dumped to serial. Usage: perm [reset]\n");
}

// ---- Y/N consent engine UI hook (passed to perm_init) ----
#define PERM_TIMEOUT_SEC 15
static int rtc_seconds(){
    for (int guard = 0; guard < 100000; guard++){
        outb(0x70, 0x0A);
        if (!(inb(0x71) & 0x80)) break;
    }
    outb(0x70, 0x00);
    uint8_t s = inb(0x71);
    outb(0x70, 0x0B);
    if (inb(0x71) & 0x04) return s;
    return (s & 0x0F) + ((s >> 4) & 0x0F) * 10;
}
static const char* perm_risk_word(int risk){
    switch (risk){
        case 0:  return "LOW";
        case 1:  return "MEDIUM";
        case 2:  return "HIGH";
        default: return "CRITICAL";
    }
}
static void perm_draw_gui_panel(const PermRequest* req){
    int W = gui_get_width(), H = gui_get_height();
    if (W <= 0 || H <= 0) return;
    int pw = 560, ph = 200;
    int px = (W - pw) / 2, py = (H - ph) / 3;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    gui_fill_rect(px - 3, py - 3, pw + 6, ph + 6, 0x00C01010);
    gui_fill_rect(px, py, pw, ph, 0x00FFE9A8);
    gui_draw_text(px + 16, py + 14,  "NexOS SECURITY REQUEST");
    gui_draw_text(px + 16, py + 40,  req->app);
    gui_draw_text(px + 16, py + 60,  req->action);
    gui_draw_text(px + 16, py + 80,  req->category);
    gui_draw_text(px + 16, py + 100, req->resource);
    gui_draw_text(px + 16, py + 132, "[Y] allow once   [N] deny");
    gui_draw_text(px + 16, py + 152, "[A] always allow [D] always deny");
    gui_draw_text(px + 16, py + 172, "no answer = DENY");
}
static int perm_ui_console(const PermRequest* req, int* remember){
    *remember = 0;
    bool in_gui = gui_is_active() != 0;
    if (in_gui) perm_draw_gui_panel(req);
    term.write("\n");
    term.set_color(make_color(WHITE, RED));
    term.write(" NexOS SECURITY REQUEST                        RISK: ");
    term.write(perm_risk_word(req->risk));
    term.write(" \n");
    term.set_color(make_color(YELLOW, BLACK));
    term.write("  Application : "); term.write(req->app);
    term.write("  (pid ");          term.write_dec((int)req->pid);
    term.write(", uid ");           term.write_dec((int)req->uid);
    term.write(")\n");
    term.write("  Wants to    : "); term.write(req->action); term.write("\n");
    term.write("  Category    : "); term.write(req->category); term.write("\n");
    term.write("  Resource    : "); term.write(req->resource); term.write("\n");
    term.set_color(make_color(WHITE, BLACK));
    term.write("  [Y] allow once   [N] deny   [A] always allow   [D] always deny\n");
    term.write("  No answer in "); term.write_dec(PERM_TIMEOUT_SEC);
    term.write("s = DENY\n");
    term.set_color(make_color(LIGHT_GREY, BLACK));
    term.render();
    // drain one RTC tick (tapjacking guard)
    int s0 = rtc_seconds();
    for (;;){
        uint8_t st = inb(0x64);
        if (st != 0xFF && (st & 0x01)) inb(0x60);
        if (rtc_seconds() != s0) break;
    }
    int last = rtc_seconds();
    int elapsed = 0;
    for (;;){
        int now = rtc_seconds();
        if (now != last){
            last = now;
            if (++elapsed >= PERM_TIMEOUT_SEC){
                term.write("  (timed out - denied)\n");
                term.render();
                if (in_gui) gui_render();
                return PERM_DENY;
            }
        }
        uint8_t st = inb(0x64);
        if (st == 0xFF || !(st & 0x01)) continue;
        uint8_t data = inb(0x60);
        if (st & 0x20) continue;
        KbdEvent e = kbd.process(data);
        if (e.type != K_CHAR) continue;
        char c = e.ch;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        int answer;
        if      (c == 'y'){ answer = PERM_ALLOW; }
        else if (c == 'n'){ answer = PERM_DENY;  }
        else if (c == 'a'){ answer = PERM_ALLOW; *remember = 1; }
        else if (c == 'd'){ answer = PERM_DENY;  *remember = 1; }
        else continue;
        term.set_color(answer == PERM_ALLOW ? make_color(GREEN, BLACK)
                                            : make_color(RED, BLACK));
        term.write(answer == PERM_ALLOW ? "  -> ALLOWED" : "  -> DENIED");
        term.write(*remember ? " (remembered)\n" : "\n");
        term.set_color(make_color(LIGHT_GREY, BLACK));
        term.render();
        if (in_gui) gui_render();
        return answer;
    }
}

// --- Auto-inference backdoor -----------------------------------------
// The 32-bit kernel's `ask64 <question>` writes 'NEXQ' + the UTF-8 question
// to shared memory at 0x5100/0x5104 and switches to long mode.  We answer
// synchronously here (streaming to serial) so headless tests can exercise
// the real GGUF engine without the PS/2 keyboard.  Checked every main-loop
// iteration, not only on a command line, so no keypress is required.
//
// Try to load the embedded GGUF model blob; returns 1 when qwen is ready.
static int model_try_load(void){
    if (qwen_ready()) return 1;
    serial_puts("[K64] loading embedded GGUF model...\n");
    uint64_t lba = 0;
    uint64_t size = model_blob_probe(&lba);
    if (!size){ serial_puts("[K64] no model blob on disk\n"); return 0; }
    uint8_t* blob = model_blob_read(lba, size);
    if (!blob){ serial_puts("[K64] blob read failed\n"); return 0; }
    int rc = qwen_load(blob, size, 512);
    if (rc != 0){
        serial_puts("[K64] qwen_load failed: ");
        serial_puts(qwen_error());
        serial_puts("\n");
        return 0;
    }
    serial_puts("[K64] model loaded\n");
    return 1;
}

static void auto_ask_backdoor(void){
    volatile uint32_t* qmagic = (volatile uint32_t*)0x5100;
    if (*qmagic != 0x5145584Eu) return;                 // 'NEXQ'
    *qmagic = 0;
    char q[128];
    volatile char* src = (volatile char*)0x5104;
    int qi = 0;
    while (qi < 120 && src[qi]){ q[qi] = (char)src[qi]; qi++; }
    q[qi] = 0;
    serial_puts("[K64-AUTO] question: "); serial_puts(q); serial_puts("\n");
    if (!model_try_load()){
        serial_puts("[K64-AUTO] no model available\n");
        return;
    }
    serial_puts("[K64-AUTO] model loaded; answer: ");
    qwen_generate(q, 96, 0.8f, ask_emit);
    serial_puts("\n[K64-AUTO] done\n");
}

// Forward declarations: these are defined later in this file (after run_command),
// but cmd_plugin needs them, so declare them here.
int  kern_fs_create(const char* name, const unsigned char* data, int len);
static void int_to_str_k64(int val, char* buf);

static void cmd_plugin(const char* args){
    if (!g_plugin_inited) { ai_plugin_init(); g_plugin_inited = true; }

    // No args -> list
    if (!*args) { char buf[3072]; ai_plugin_list(buf, sizeof(buf)); term.write(buf); return; }

    char sub[16]; int si = 0;
    while (*args && *args != ' ' && si < 15) sub[si++] = *args++;
    sub[si] = 0;
    while (*args == ' ') args++;

    if (!strcmp_(sub, "list") || !strcmp_(sub, "ls")) {
        char buf[3072]; ai_plugin_list(buf, sizeof(buf)); term.write(buf);
    } else if (!strcmp_(sub, "persist")) {
        char buf[3072]; int n = ai_plugin_serialize(buf, sizeof(buf));
        int r = kern_fs_create("plugins.lst", (const unsigned char*)buf, n);
        if (r >= 0) { term.write("Plugin catalogue written to plugins.lst ("); char nn[16]; int_to_str_k64(n, nn); term.write(nn); term.write(" bytes)\n"); }
        else term.write("Failed to write plugins.lst\n");
    } else if (!strcmp_(sub, "toggle") || !strcmp_(sub, "load") || !strcmp_(sub, "unload")) {
        if (!*args) { term.write("Usage: plugin toggle <id>\n"); return; }
        int want = !strcmp_(sub, "unload") ? 0 : 1;
        int s = (!strcmp_(sub, "toggle")) ? ai_plugin_toggle(args) : ai_plugin_set(args, want);
        if (s < 0) { term.write("plugin not found: "); term.write(args); term.put_char('\n'); return; }
        char buf[3072]; int n = ai_plugin_serialize(buf, sizeof(buf));
        kern_fs_create("plugins.lst", (const unsigned char*)buf, n);
        term.write(args); term.write(s ? " -> loaded\n" : " -> unloaded\n");
    } else if (!strcmp_(sub, "info")) {
        if (!*args) { term.write("Usage: plugin info <id>\n"); return; }
        int i = ai_plugin_find(args);
        if (i < 0) { term.write("plugin not found: "); term.write(args); term.put_char('\n'); return; }
        AiPlugin* pl = &g_plugins[i];
        term.write("Plugin: "); term.write(pl->id); term.put_char('\n');
        term.write("  Name : "); term.write(pl->name); term.put_char('\n');
        term.write("  Deps : "); term.write(pl->deps); term.put_char('\n');
        term.write("  Mem  : "); char nn[16]; int_to_str_k64(pl->mem_kb, nn); term.write(nn); term.write(" KB\n");
        term.write("  State: "); int_to_str_k64(pl->state, nn); term.write(nn); term.write(" (0 planned,1 basic,2 available)\n");
        term.write("  Load : "); int_to_str_k64(pl->loaded, nn); term.write(nn); term.put_char('\n');
    } else if (!strcmp_(sub, "run")) {
        if (!*args) { term.write("Usage: plugin run <id>\n"); return; }
        if (!strcmp_(args, "nexos.ai.inference")) {
            term.write("Loading core AI inference plugin...\n");
            int ret = ai_init("/boot/model.gguf");
            if (ret == 0) { g_ai_initialized = true; term.write("AI inference engine loaded.\n"); }
            else term.write("AI init failed (no model?).\n");
        } else {
            term.write("Plugin '"); term.write(args); term.write("' is not runnable in this build.\n");
        }
    } else {
        term.write("Usage: plugin [list|persist|toggle|load|unload|info|run] [<id>]\n");
    }
}

// Forward declaration: distributed compute fabric (distnet.cpp, linked as
// distnet64.o).  Lets `distnet` reach the 64-bit kernel shell so the network
// works on real UEFI hardware, not just the 32-bit BIOS path.  C linkage to
// match the declaration in distnet.h (so the symbol mangles identically).
extern "C" void cmd_distnet(const char* args);

static void run_command(const char* line){
    auto_ask_backdoor();
    while(*line==' ') line++;
    if(*line==0) return;
    if (!g_in_script) hist_add(line);

    // Normalize path separators in the entire line
    char normline[HIST_LEN];
    int li=0;
    while(line[li] && li<HIST_LEN-1){ normline[li]=line[li]; li++; }
    normline[li]=0;
    normalize_path(normline);

    char cmd[32]; int ci=0;
    const char* p=normline;
    while(*p && *p!=' ' && ci<31) cmd[ci++]=*p++;
    cmd[ci]=0;
    const char* args=p; while(*args==' ') args++;

    if(!strcmp_(cmd,"help"))       cmd_help();
    else if(!strcmp_(cmd,"echo"))  { term.write(args); term.put_char('\n'); }
    else if(!strcmp_(cmd,"clear")||!strcmp_(cmd,"cls")) term.clear_screen();
    else if(!strcmp_(cmd,"about")) cmd_about();
    else if(!strcmp_(cmd,"history")||!strcmp_(cmd,"h")) cmd_history();
    else if(!strcmp_(cmd,"save"))  cmd_save();
    else if(!strcmp_(cmd,"load"))  cmd_load();
    // MKFS commands (with PowerShell-style aliases)
    else if(!strcmp_(cmd,"mkfs"))  cmd_mkfs();
    else if(!strcmp_(cmd,"ls")||!strcmp_(cmd,"dir")) cmd_ls();
    else if(!strcmp_(cmd,"cat")||!strcmp_(cmd,"type")) cmd_cat(args);
    else if(!strcmp_(cmd,"touch")||!strcmp_(cmd,"ni")) cmd_touch(args);
    else if(!strcmp_(cmd,"rm")||!strcmp_(cmd,"del")||!strcmp_(cmd,"erase")) cmd_rm(args);
    else if(!strcmp_(cmd,"copy")||!strcmp_(cmd,"cp"))  cmd_copy(args);
    else if(!strcmp_(cmd,"write")) cmd_write(args);
    else if(!strcmp_(cmd,"mkdir")||!strcmp_(cmd,"md")) cmd_mkdir(args);
    else if(!strcmp_(cmd,"cd")||!strcmp_(cmd,"sl"))    cmd_cd(args);
    else if(!strcmp_(cmd,"pwd")||!strcmp_(cmd,"gl"))   cmd_pwd();
    // SFS commands
    else if(!strcmp_(cmd,"lsfs"))  cmd_lsfs();
    else if(!strcmp_(cmd,"catfs")) cmd_catfs(args);
    // Partition commands
    else if(!strcmp_(cmd,"part"))  cmd_part();
    else if(!strcmp_(cmd,"mount")) cmd_mount(args);
    else if(!strcmp_(cmd,"lsfat")) cmd_lsfat();
    else if(!strcmp_(cmd,"fatinfo"))cmd_fatinfo();
    // Script execution
    else if(!strcmp_(cmd,"run"))   cmd_run(args);
    else if(!strcmp_(cmd,"runfs")) cmd_runfs(args);
    // AI engine commands
    else if(!strcmp_(cmd,"ai"))         cmd_ai(args);
    else if(!strcmp_(cmd,"generate")||!strcmp_(cmd,"gen")) cmd_generate(args);
    else if(!strcmp_(cmd,"agent"))      cmd_agent(args);
    else if(!strcmp_(cmd,"ask"))        cmd_ask(args);
    else if(!strcmp_(cmd,"model"))      cmd_model(args);
    // Plugin subsystem (catalogue manager + Settings > Plugins UI backend)
    else if(!strcmp_(cmd,"plugin"))     cmd_plugin(args);
    // Network commands
    else if(!strcmp_(cmd,"netinfo")||!strcmp_(cmd,"netstat")) cmd_netinfo();
    else if(!strcmp_(cmd,"netstart")||!strcmp_(cmd,"net"))   cmd_netstart();
    else if(!strcmp_(cmd,"distnet"))                         cmd_distnet(args);
    // Kernel switching
    else if(!strcmp_(cmd,"switch")||!strcmp_(cmd,"switch32")) cmd_switch32();
    // GUI
    else if(!strcmp_(cmd,"gui"))  cmd_gui(args);
    else if(!strcmp_(cmd,"nogui")) cmd_nogui(args);
    // Voice interaction engine
    else if(!strcmp_(cmd,"voice")) cmd_voice(args);
    // Win64 subsystem (PE32+ loader)
    else if(!strcmp_(cmd,"winapp")) cmd_winapp(args);
    // Memory management
    else if(!strcmp_(cmd,"meminfo"))  cmd_meminfo();
    else if(!strcmp_(cmd,"memtest"))  cmd_memtest();
    else if(!strcmp_(cmd,"pagetest")) cmd_pagetest();
    // Power management
    else if(!strcmp_(cmd,"shutdown")||!strcmp_(cmd,"exit")||!strcmp_(cmd,"poweroff")) cmd_shutdown();
    else if(!strcmp_(cmd,"reboot")||!strcmp_(cmd,"restart")) cmd_reboot();
    // Security subsystem (login + rwx permissions + consent engine)
    else if(!strcmp_(cmd,"whoami"))  cmd_whoami();
    else if(!strcmp_(cmd,"id"))      cmd_id();
    else if(!strcmp_(cmd,"users"))   cmd_users();
    else if(!strcmp_(cmd,"login"))   cmd_login(args);
    else if(!strcmp_(cmd,"logout"))  cmd_logout();
    else if(!strcmp_(cmd,"su"))      cmd_su(args);
    else if(!strcmp_(cmd,"useradd")) cmd_useradd(args);
    else if(!strcmp_(cmd,"deluser")) cmd_deluser(args);
    else if(!strcmp_(cmd,"passwd"))  cmd_passwd(args);
    else if(!strcmp_(cmd,"chmod"))   cmd_chmod(args);
    else if(!strcmp_(cmd,"stat"))    cmd_stat(args);
    else if(!strcmp_(cmd,"sudo"))    cmd_sudo(args);
    else if(!strcmp_(cmd,"perm"))    cmd_perm(args);
    else { term.write("Unknown command: "); term.write(cmd);
           term.write("  (Type 'help' for available commands)\n"); }
}

// =====================================================================
//  Terminal::render (defined after the class, uses its members)
// =====================================================================

// ---- SSH server hooks (called from net.cpp) ----
extern "C" int nexos_auth(const char* user, const char* pw){
    for(int i = 0; i < g_user_count; i++){
        if(!g_users[i].exists) continue;
        if(strcmp_(g_users[i].name, user) != 0) continue;
        char hash[17];
        hash_password(g_users[i].name, pw, hash);
        if(strcmp_(hash, g_users[i].hash) == 0){
            g_login_idx = i;
            g_euid = g_users[i].uid;
            g_sudo_active = false;
            return 1;
        }
    }
    return 0;
}

extern "C" void kernel_exec_line(const char* line){
    run_command(line);
}

extern "C" void term_set_ssh_sink(ssh_out_fn_t fn){ g_ssh_out_fn = fn; }
extern "C" void term_clear_ssh_sink(void){ g_ssh_out_fn = 0; }

void Terminal::render(){
    if(m_at_bottom) m_view=bottom_view();
    int cur_row = m_at_bottom ? (m_count - m_view) : -1;
    for(int r=0;r<VGA_HEIGHT;r++){
        if(m_at_bottom && r==cur_row){
            for(int x=0;x<VGA_WIDTH;x++){
                char ch=(x<m_cur_len)?m_cur[x]:' ';
                VGA_MEMORY[r*VGA_WIDTH+x]=make_entry((unsigned char)ch,m_color);
            }
        } else {
            int li=m_view+r;
            if(li<m_count){
                Line& L=line_at(li);
                for(int x=0;x<VGA_WIDTH;x++){
                    char ch=(x<L.len)?L.data[x]:' ';
                    VGA_MEMORY[r*VGA_WIDTH+x]=make_entry((unsigned char)ch,m_color);
                }
            } else {
                for(int x=0;x<VGA_WIDTH;x++)
                    VGA_MEMORY[r*VGA_WIDTH+x]=make_entry(' ',m_color);
            }
        }
    }

    // ----- Selection highlight (invert colors for selected cells) -----
    if(m_selecting || m_has_selection){
        int sx=m_sel_sx, sy=m_sel_sy, ex=m_sel_ex, ey=m_sel_ey;
        if(sy>ey || (sy==ey && sx>ex)){
            int t=sx; sx=ex; ex=t;
            int t2=sy; sy=ey; ey=t2;
        }
        for(int r=sy; r<=ey && r<VGA_HEIGHT; r++){
            int cs = (r==sy) ? sx : 0;
            int ce = (r==ey) ? ex : VGA_WIDTH-1;
            for(int x=cs; x<=ce && x<VGA_WIDTH; x++){
                uint16_t cell = VGA_MEMORY[r*VGA_WIDTH+x];
                uint8_t  attr = (cell>>8)&0xFF;
                uint8_t  inv  = ((attr&0x0F)<<4) | ((attr>>4)&0x0F);
                VGA_MEMORY[r*VGA_WIDTH+x] = (cell&0x00FF) | ((uint16_t)inv<<8);
            }
        }
    }

    // ----- Mouse cursor (invert the cell under the mouse) -----
    if(m_mouse_visible){
        int idx = m_mouse_y * VGA_WIDTH + m_mouse_x;
        if(idx >= 0 && idx < VGA_WIDTH * VGA_HEIGHT){
            uint16_t cell = VGA_MEMORY[idx];
            uint8_t  attr = (cell>>8)&0xFF;
            uint8_t  inv  = ((attr&0x0F)<<4) | ((attr>>4)&0x0F);
            VGA_MEMORY[idx] = (cell&0x00FF) | ((uint16_t)inv<<8);
        }
    }

    if(m_at_bottom){ show_cursor(); set_cursor_pos(cur_row, m_cur_pos); }
    else            hide_cursor();

    // In framebuffer-console mode, blit the (shadow) VGA text buffer to the
    // real VBE linear framebuffer so the shell is visible on `make play` /
    // GTK / SDL.  The hardware cursor is invisible in graphics mode; fb_console
    // draws its own software cursor from the position set above.
    if (g_fb_console_mode) fb_console_render();
}

// =====================================================================
//  GUI Callbacks - registered by kernel for data access (64-bit version)
// =====================================================================
// Simple int-to-string for serial debug
static void int_to_str_k64(int val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char tmp[16]; int i = 0;
    bool neg = val < 0;
    if (neg) val = -val;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

static void uint_to_str_k64(uint32_t val, char* buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char tmp[16]; int i = 0;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

// ---- Memory info callbacks ----
static uint32_t gui_cb_total_mem(void)    { return pmm_mem_kb; }
static uint32_t gui_cb_free_pages(void)   { return pmm_free_pages; }
static uint32_t gui_cb_used_pages(void)   { return pmm_used_pages; }
static uint32_t gui_cb_total_pages(void)  { return pmm_total_pages; }

// ---- Heap info callbacks ----
static uint32_t gui_cb_heap_alloc_bytes(void) { return heap_bytes_alloc - heap_bytes_freed; }
static uint32_t gui_cb_heap_free_bytes(void)  {
    uint32_t used = heap_bytes_alloc - heap_bytes_freed;
    return (HEAP_SIZE - sizeof(HeapBlock)) > used ? (HEAP_SIZE - sizeof(HeapBlock)) - used : 0;
}
static uint32_t gui_cb_heap_alloc_count(void) { return heap_alloc_count - heap_free_count; }
static uint32_t gui_cb_heap_free_count(void)  { return heap_free_count; }

// ---- Memory optimization callback ----
static void gui_cb_optimize_memory(void) {
    serial_puts("[K64-MEM] Memory optimization requested\n");
    // Coalesce free heap blocks
    HeapBlock* blk = heap_head;
    int coalesced = 0;
    while (blk) {
        if (blk->magic == HEAP_MAGIC_FREE && blk->next &&
            blk->next->magic == HEAP_MAGIC_FREE) {
            HeapBlock* nb = blk->next;
            blk->size += sizeof(HeapBlock) + nb->size;
            blk->next = nb->next;
            if (nb->next) nb->next->prev = blk;
            coalesced++;
            continue; // don't advance, try again
        }
        blk = blk->next;
    }
    serial_puts("[K64-MEM] Heap coalesced ");
    char buf[16]; int_to_str_k64(coalesced, buf); serial_puts(buf);
    serial_puts(" blocks\n");
}

// ---- File listing callback ----
// fs_type: 0=MKFS, 1=SFS, 2=FAT32
static int gui_cb_list_files(int fs_type, char* outbuf, int bufsize) {
    int count = 0;
    int pos = 0;
    outbuf[0] = 0;

    if (fs_type == 0) {
        // MKFS
        if (!mkfs.mounted) return 0;
        for (int s = 0; s < MKFS_TABLE_SECT; s++) {
            ata_read_sector(MKFS_TABLE_LBA + s, (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0 && fe->parent == g_cwd) {
                    const char* prefix = (fe->type == FS_TYPE_DIR) ? "[D] " : "    ";
                    int plen = strlen_(prefix);
                    int nlen = strlen_(fe->name);
                    if (pos + plen + nlen + 2 < bufsize) {
                        memcpy_(outbuf + pos, prefix, plen); pos += plen;
                        memcpy_(outbuf + pos, fe->name, nlen); pos += nlen;
                        outbuf[pos++] = '\n';
                        count++;
                    }
                }
            }
        }
    } else if (fs_type == 1) {
        // SFS
        if (!sfs.mounted) return 0;
        for (int s = 0; s < SFS_DIR_SECT; s++) {
            ata_read_sector(sfs.dir_lba(s), (uint16_t*)g_fsbuf);
            for (int e = 0; e < FS_ENTRY_PER_SEC; e++) {
                FileEntry* fe = (FileEntry*)(g_fsbuf + e * FS_ENTRY_SIZE);
                if (fe->name[0] != 0) {
                    int nlen = strlen_(fe->name);
                    if (pos + nlen + 12 < bufsize) {
                        memcpy_(outbuf + pos, fe->name, nlen); pos += nlen;
                        outbuf[pos++] = ' ';
                        outbuf[pos++] = '(';
                        char sz[12]; uint_to_str_k64(fe->size, sz);
                        int sl = strlen_(sz);
                        memcpy_(outbuf + pos, sz, sl); pos += sl;
                        outbuf[pos++] = 'B';
                        outbuf[pos++] = ')';
                        outbuf[pos++] = '\n';
                        count++;
                    }
                }
            }
        }
    } else if (fs_type == 2) {
        // FAT32
        if (!fat32.mounted) return 0;
        uint32_t cluster = fat32.root_cluster;
        for (int cl = 0; cl < 32; cl++) {
            if (cluster < 2 || cluster >= 0x0FFFFFF8) break;
            uint32_t lba = fat32.data_start + (cluster - 2) * fat32.sectors_per_cluster;
            for (int s = 0; s < fat32.sectors_per_cluster && s * 512 < FS_IOBUF_SIZE; s++)
                ata_read_sector(lba + s, (uint16_t*)(g_iobuf + s * 512));
            for (int i = 0; i < fat32.sectors_per_cluster * 512; i += 32) {
                uint8_t* de = g_iobuf + i;
                if (de[0] == 0x00) break;
                if (de[0] == 0xE5) continue;
                if (de[11] & 0x0F) continue; // skip LFN entries
                char name[13];
                int ni = 0;
                for (int j = 0; j < 8 && de[j] != ' '; j++)
                    if (ni < 12) name[ni++] = de[j];
                if (de[8] != ' ') {
                    if (ni < 12) name[ni++] = '.';
                    for (int j = 8; j < 11 && de[j] != ' '; j++)
                        if (ni < 12) name[ni++] = de[j];
                }
                name[ni] = 0;
                int nlen = ni;
                if (pos + nlen + 2 < bufsize) {
                    memcpy_(outbuf + pos, name, nlen); pos += nlen;
                    outbuf[pos++] = '\n';
                    count++;
                }
            }
            // Follow FAT chain
            uint32_t fat_off = cluster * 4;
            uint32_t fat_sec = fat32.part_start + fat32.reserved_sectors + fat_off / 512;
            ata_read_sector(fat_sec, (uint16_t*)g_fsbuf);
            cluster = *(uint32_t*)(g_fsbuf + (fat_off % 512)) & 0x0FFFFFFF;
        }
    }
    outbuf[pos] = 0;
    return count;
}

// ---- File read callback ----
// The desktop's texture loader (gui.cpp tex_load_one) goes through this to
// pull tex_*.tex out of SFS.  Without it every HasImage() query returns 0 and
// the managed shell silently falls back to a flat gradient -- no wallpaper,
// no icons.  Mirrors the 32-bit gui_cb_read_file.
static int gui_cb_read_file(int fs_type, const char* name, uint8_t* buf, int bufsize) {
    while (*name == ' ') name++;   // strip the "    " / "[D] " list prefix

    if (fs_type == 0) {            // MKFS
        if (!mkfs.mounted) return -1;
        return mkfs.read(name, buf, bufsize);
    } else if (fs_type == 1) {     // SFS  <- textures live here
        if (!sfs.mounted) return -1;
        return sfs.read(name, buf, bufsize);
    } else if (fs_type == 3) {     // Desktop folder (MKFS subdir)
        if (!mkfs.mounted) return -1;
        int di = mkfs.find("Desktop");
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        int n = mkfs.read(name, buf, bufsize);
        g_cwd = saved;
        return n;
    }
    return -1;                     // FAT32 read not implemented here
}

// ---- Session persistence (MKFS data disk) ----
static int gui_cb_session_save(const char* name, const void* data, int size) {
    if (!mkfs.mounted) return -1;
    return mkfs.create(name, data, size);
}
static int gui_cb_session_load(const char* name, void* buf, int bufsize) {
    if (!mkfs.mounted) return -1;
    return mkfs.read(name, (uint8_t*)buf, bufsize);
}

// Kernel file-system write back (MKFS data FS) -- used by the Win32 subsystem
// (CreateFileA/WriteFile/CloseHandle) and the skill system to persist files.
// Mirrors the 32-bit kernel.cpp definition so win32_64.o can link it.
int kern_fs_create(const char* name, const unsigned char* data, int len){
    if (!mkfs.mounted) return -2;
    return mkfs.create(name, (const uint8_t*)data, len);
}

// ---- Time callback (from CMOS RTC) ----
static void gui_cb_get_time(int* h, int* m, int* s) {
    outb(0x70, 0x00); *s = inb(0x71);
    outb(0x70, 0x02); *m = inb(0x71);
    outb(0x70, 0x04); *h = inb(0x71);
    // Convert BCD to binary
    *s = (*s & 0x0F) + ((*s >> 4) & 0x0F) * 10;
    *m = (*m & 0x0F) + ((*m >> 4) & 0x0F) * 10;
    *h = (*h & 0x0F) + ((*h >> 4) & 0x0F) * 10;
}

// ---- OS name callback ----
static const char* gui_cb_os_name(void) {
    return "NexOS v2.0 (Win11 Desktop)";
}

// ---- 64-bit detection ----
// Returns whether we are CURRENTLY running in 64-bit long mode.
// This 64-bit kernel runs in long mode, so return true.
static bool gui_cb_is_64bit(void) {
    return true; // 64-bit long mode active
}

// CPU supports 64-bit (always true in 64-bit kernel)
static int gui_cb_cpu_64bit_capable(void) {
    return 1;
}

// ---- Browser / HTTP callbacks ----
// Mirror the 32-bit kernel's wiring (kernel.cpp gui_cb_browser_*) so the
// managed BrowserApp under long mode can reach the kernel's HTTP client.
// The net_* / browser_* symbols live in net.cpp (compiled into kernel64),
// so they are directly callable once declared above.
static int gui_cb_browser_navigate(const char* url) {
    if (!g_net_initialized) return -1;
    return browser_navigate(url);
}
static int gui_cb_browser_status(void) {
    if (!g_net_initialized) return 0;
    return browser_status();
}
static int gui_cb_browser_get_page(char* buf, int bufsize) {
    if (!g_net_initialized) return -1;
    return browser_get_page(buf, bufsize);
}
static void gui_cb_browser_reset(void) {
    if (!g_net_initialized) return;
    browser_reset();
}
// Synchronous HTTP GET for the managed Browser control.  Drives the kernel
// network state machine (via net_poll) until the response is in, then
// returns the body through a static buffer the icall copies into a string.
static const char* gui_cb_http_get(const char* url) {
    static char buf[16384];
    buf[0] = 0;
    if (g_net_initialized) net_http_get(url, buf, (int)sizeof(buf));
    return buf;
}

// ---- File-mutation callbacks (context-menu: New Folder / Rename / Delete) ----
// Mirror the 32-bit kernel's gui_cb_mkdir/remove/rename.  fs==3 means the
// Desktop subfolder of MKFS, which the managed shell uses for shortcuts.
static int gui_cb_mkdir(int fs, const char* name) {
    while (name && *name == ' ') name++;
    if (!name || !name[0]) return -1;
    if (fs == 3) {                 // Desktop folder
        if (!mkfs.mounted) return -1;
        int di = mkfs.find("Desktop");
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        int r = mkfs.mkdir(name);
        g_cwd = saved;
        return r;
    }
    if (fs != 0 || !mkfs.mounted) return -1;
    return mkfs.mkdir(name);
}
static int gui_cb_remove(int fs, const char* name) {
    while (name && *name == ' ') name++;
    if (!name || !name[0]) return -1;
    if (fs == 3) {                 // Desktop folder
        if (!mkfs.mounted) return -1;
        int di = mkfs.find("Desktop");
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        int r = mkfs.remove(name);
        g_cwd = saved;
        return r;
    }
    if (fs != 0 || !mkfs.mounted) return -1;
    return mkfs.remove(name);
}
static int gui_cb_rename(int fs, const char* old_name, const char* new_name) {
    while (old_name && *old_name == ' ') old_name++;
    while (new_name && *new_name == ' ') new_name++;
    if (!old_name || !old_name[0] || !new_name || !new_name[0]) return -1;
    if (fs == 3) {                 // Desktop folder
        if (!mkfs.mounted) return -1;
        int di = mkfs.find("Desktop");
        if (di < 0) return -1;
        uint16_t saved = g_cwd; g_cwd = (uint16_t)di;
        int r = -1;
        if (mkfs.copy(old_name, new_name) >= 0) r = mkfs.remove(old_name);
        g_cwd = saved;
        return r;
    }
    if (fs != 0 || !mkfs.mounted) return -1;
    if (mkfs.copy(old_name, new_name) < 0) return -1;
    return mkfs.remove(old_name);
}

// ---- Session persistence clear ----
static int gui_cb_session_clear(const char* name) {
    if (!mkfs.mounted) return -1;
    if (mkfs.find(name) >= 0) return mkfs.remove(name);
    return 0;
}

// ---- Terminal command execution callback ----
// Captures terminal output by redirecting term.write into g_exec_output while
// a command runs, then copies the captured text back to the C# caller.
static void gui_cb_exec_command(const char* cmd, char* output, int outsize) {
    g_exec_output_len = 0;
    g_exec_output[0] = 0;
    g_capturing = true;
    // Execute the command using the existing shell command dispatcher.
    run_command(cmd);
    g_capturing = false;
    int len = g_exec_output_len;
    if (len > outsize - 1) len = outsize - 1;
    memcpy_(output, g_exec_output, len);
    output[len] = 0;
}

// ---- Hardware info callbacks for GUI (Settings -> About) ----
static const char* gui_cb_cpu_vendor(void) {
    static char v[13];
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    v[0]=(char)(b&0xff); v[1]=(char)((b>>8)&0xff); v[2]=(char)((b>>16)&0xff); v[3]=(char)((b>>24)&0xff);
    v[4]=(char)(d&0xff); v[5]=(char)((d>>8)&0xff); v[6]=(char)((d>>16)&0xff); v[7]=(char)((d>>24)&0xff);
    v[8]=(char)(c&0xff); v[9]=(char)((c>>8)&0xff); v[10]=(char)((c>>16)&0xff); v[11]=(char)((c>>24)&0xff);
    v[12]=0;
    return v;
}
static const char* gui_cb_disk_model(void)      { return "NexOS Virtual Disk"; }
static uint32_t    gui_cb_disk_size_mb(void)    { return 0; }   // ATA identify not wired in 64-bit yet
static int         gui_cb_nic_present(void)     { return g_net_initialized ? 1 : 0; }
static int         gui_cb_mouse_present(void)   { return 1; }
static int         gui_cb_keyboard_present(void){ return 1; }
static uint32_t    gui_cb_pci_count(void)       { return 0; }
static int         gui_cb_bga_available(void)   { return gui_bga_available(); }
static int         gui_cb_vbe_mode_set(void)    { return gui_vbe_mode_set_by_bios(); }

// ---- Register all callbacks ----
// =====================================================================
//  Graphical sign-in bridge (mirrors the 32-bit kernel)
// ---------------------------------------------------------------------
//  csharp/apps/Shell/Login.cs draws the lock screen; the hashes stay
//  here.  login_check() verifies one credential pair and, on success,
//  commits the session exactly like the console login_prompt() does.
// =====================================================================
static int gui_cb_login_check(const char* user, const char* pass) {
    if (!user || !pass) return -1;
    for (int i = 0; i < g_user_count; i++) {
        if (!g_users[i].exists) continue;
        if (strcmp_(g_users[i].name, user) != 0) continue;
        char hash[17];
        hash_password(g_users[i].name, pass, hash);
        if (strcmp_(hash, g_users[i].hash) != 0) {
            serial_puts("[K64-LOGIN] reject user="); serial_puts(user); serial_puts("\n");
            return -1;
        }
        g_login_idx   = i;
        g_euid        = g_users[i].uid;
        g_sudo_active = false;
        serial_puts("[K64-LOGIN] OK user="); serial_puts(g_users[i].name); serial_puts("\n");
        return (int)g_users[i].uid;
    }
    serial_puts("[K64-LOGIN] no such user\n");
    return -1;
}

static int gui_cb_login_uid(void) {
    if (g_login_idx < 0) return -1;
    return (int)g_users[g_login_idx].uid;
}

static int gui_cb_user_count(void) {
    int n = 0;
    for (int i = 0; i < g_user_count; i++) if (g_users[i].exists) n++;
    return n;
}

static const char* gui_cb_user_name(int idx) {
    int n = 0;
    for (int i = 0; i < g_user_count; i++) {
        if (!g_users[i].exists) continue;
        if (n == idx) return g_users[i].name;
        n++;
    }
    return "";
}

static void register_gui_callbacks(void) {
    // Value-initialise: gui.cpp null-checks every optional slot, so anything
    // we do not implement here must read back as a null pointer, not as
    // whatever happened to be on the stack.
    GuiCallbacks cb{};
    cb.get_total_mem_kb    = gui_cb_total_mem;
    cb.get_free_pages      = gui_cb_free_pages;
    cb.get_used_pages      = gui_cb_used_pages;
    cb.get_total_pages     = gui_cb_total_pages;
    cb.get_heap_alloc_bytes= gui_cb_heap_alloc_bytes;
    cb.get_heap_free_bytes = gui_cb_heap_free_bytes;
    cb.get_heap_alloc_count= gui_cb_heap_alloc_count;
    cb.get_heap_free_count = gui_cb_heap_free_count;
    cb.optimize_memory     = gui_cb_optimize_memory;
    cb.list_files          = gui_cb_list_files;
    cb.read_file           = gui_cb_read_file;   // textures / wallpaper come through here
    cb.get_time            = gui_cb_get_time;
    cb.session_save        = gui_cb_session_save;
    cb.session_load        = gui_cb_session_load;
    cb.get_os_name         = gui_cb_os_name;
    cb.is_64bit            = gui_cb_is_64bit;
    cb.get_cpu_64bit_capable = gui_cb_cpu_64bit_capable;
    // Browser + network: let the 64-bit managed browser fetch real pages.
    cb.browser_navigate    = gui_cb_browser_navigate;
    cb.browser_status      = gui_cb_browser_status;
    cb.browser_get_page    = gui_cb_browser_get_page;
    cb.browser_reset       = gui_cb_browser_reset;
    cb.http_get            = gui_cb_http_get;
    cb.shutdown            = cmd_shutdown;
    cb.reboot              = cmd_reboot;
    // File-mutation (context-menu New Folder / Rename / Delete) + exec.
    cb.mkdir               = gui_cb_mkdir;
    cb.remove              = gui_cb_remove;
    cb.rename              = gui_cb_rename;
    cb.session_clear       = gui_cb_session_clear;
    cb.exec_command        = gui_cb_exec_command;
    // Hardware info (Settings -> About).
    cb.get_cpu_vendor      = gui_cb_cpu_vendor;
    cb.get_disk_model      = gui_cb_disk_model;
    cb.get_disk_size_mb    = gui_cb_disk_size_mb;
    cb.get_nic_present     = gui_cb_nic_present;
    cb.get_mouse_present   = gui_cb_mouse_present;
    cb.get_keyboard_present= gui_cb_keyboard_present;
    cb.get_pci_count       = gui_cb_pci_count;
    cb.get_bga_available   = gui_cb_bga_available;
    cb.get_vbe_mode_set    = gui_cb_vbe_mode_set;
    // Graphical lock screen (Login.cs).
    cb.login_check         = gui_cb_login_check;
    cb.login_uid           = gui_cb_login_uid;
    cb.user_count          = gui_cb_user_count;
    cb.user_name           = gui_cb_user_name;
    gui_set_callbacks(&cb);
}

// =====================================================================
//  kmain64  -  64-bit kernel entry point (called from entry64.asm)
// =====================================================================
// ---- Phase 0 ggml adapter self-test (Route A) -----------------------
// Exercises memory_adapter.c / file_adapter.c / gguf_loader.c on real
// hardware and reports over the serial port so a headless QEMU run can verify
// the adapter layer.  Harmless on a normal boot (alloc/free 4 KiB, probe one
// descriptor sector, parse a tiny in-RAM GGUF fixture).
void gguf_adapter_selftest(void){
    serial_puts("[ADAPT] === ggml adapter self-test ===\n");

    // 1) memory adapter: alloc / pattern / free
    void* p = ggml_alloc(4096);
    if (p){
        volatile uint8_t* m = (volatile uint8_t*)p;
        m[0] = 0xAB; m[4095] = 0xCD;
        int ok = (m[0] == 0xAB && m[4095] == 0xCD);
        ggml_free(p, 4096);
        serial_puts(ok ? "[ADAPT] memory: alloc/free OK\n"
                       : "[ADAPT] memory: FAIL pattern\n");
    } else {
        serial_puts("[ADAPT] memory: FAIL alloc\n");
    }

    // 2) file adapter: disk backend probe (reads the "MINIMDL1" descriptor)
    static const uint32_t hdr_lbas[] = {16383u, 8191u, 4095u, 0u};
    gguf_io* dio = gguf_io_disk_create(hdr_lbas, ata_read_sector);
    if (dio){
        char n[24];
        serial_puts("[ADAPT] disk: blob present, size=");
        uint_to_str_k64((uint32_t)dio->size(dio), n); serial_puts(n);
        serial_puts(" bytes -> parsing via loader\n");
        // gguf_ctx carries tensors[1024] (~123 KB) -- far too big for the
        // 64 KiB kernel stack.  Static storage avoids the random triple-fault
        // that appeared once disk parsing reached the deep tensor list.
        static gguf_ctx dctx;
        if (gguf_load(dio, &dctx) == 0){
            serial_puts("[ADAPT] disk parse OK: arch=");
            serial_puts(dctx.arch); serial_puts(" quant=");
            serial_puts(dctx.quant); serial_puts(" tensors=");
            uint_to_str_k64((uint32_t)dctx.n_tensors, n); serial_puts(n);
            serial_puts("\n");
        } else {
            serial_puts("[ADAPT] disk parse FAIL\n");
        }
        gguf_io_disk_destroy(dio);
    } else {
        serial_puts("[ADAPT] disk: no model blob on image (probe OK; embed one for full disk read)\n");
    }

    // 3) gguf loader over the in-RAM fixture (always runs, no disk needed)
    gguf_io* mio = gguf_io_mem_create(k_gguf_fixture, k_gguf_fixture_size);
    if (mio){
        char n[24];
        serial_puts("[ADAPT] fixture size=");
        uint_to_str_k64((uint32_t)k_gguf_fixture_size, n); serial_puts(n);
        serial_puts(" magic=");
        for (int i = 0; i < 4; i++){ n[0] = (char)('0' + ((k_gguf_fixture[i] >> 4) & 0xF)); n[1] = (char)('0' + (k_gguf_fixture[i] & 0xF)); n[2] = 0; serial_puts(n); }
        serial_puts("\n");
        static gguf_ctx ctx;   // ~123 KB -- too big for the 64 KiB stack
        int rc = gguf_load(mio, &ctx);
        serial_puts("[ADAPT] rc="); uint_to_str_k64((uint32_t)rc, n); serial_puts(n);
        serial_puts(" tc="); uint_to_str_k64((uint32_t)ctx.tensor_count, n); serial_puts(n);
        serial_puts(" kc="); uint_to_str_k64((uint32_t)ctx.kv_count, n); serial_puts(n);
        serial_puts("\n");
        if (rc == 0){
            serial_puts("[ADAPT] gguf parse OK: arch=");
            serial_puts(ctx.arch); serial_puts(" quant=");
            serial_puts(ctx.quant); serial_puts(" version=");
            uint_to_str_k64(ctx.version, n); serial_puts(n);
            serial_puts(" tensors="); uint_to_str_k64((uint32_t)ctx.n_tensors, n); serial_puts(n);
            serial_puts(" embed="); uint_to_str_k64(ctx.embed_length, n); serial_puts(n);
            serial_puts(" vocab="); uint_to_str_k64(ctx.vocab_size, n); serial_puts(n);
            serial_puts(" bos="); uint_to_str_k64(ctx.bos_id, n); serial_puts(n);
            serial_puts(" type0="); serial_puts(gguf_loader_type_name(ctx.tensors[0].type));
            serial_puts("\n");
        } else {
            serial_puts("[ADAPT] gguf parse FAIL\n");
        }
        gguf_io_mem_destroy(mio);
    } else {
        serial_puts("[ADAPT] mem io FAIL\n");
    }

    serial_puts("[ADAPT] === done ===\n");
}

// Boot beacon: painted by the kernel to mark progress without a serial
// console.  Declared C-linkage to match gui.cpp's definition so both the
// 32-bit and 64-bit kernels link against the same unmangled symbol.
extern "C" void boot_beacon(uint8_t, uint8_t, uint8_t);

// ===========================================================================
//  High-framebuffer mapping self-check (long mode)
// ---------------------------------------------------------------------------
//  Every previous real-hardware black-screen theory rested on *assuming* what
//  switch32to64.asm did or did not map.  That guesswork cost weeks.  Instead,
//  walk the live CR3 tables and print exactly what is there -- and if the
//  >4 GiB GOP framebuffer really is unmapped, install a 1 GiB identity page
//  ourselves so the GUI can never silently draw into nowhere again.
//
//  Safe to run this early: the same tables identity-map the low 4 GiB, so the
//  page-table pages themselves (0x90000-0x95FFF) are directly addressable.
// ===========================================================================
#define FB_SELFCHK_PDPT_PHYS 0x95000ULL   // spare page in the 0x90000-0x9FBFF gap
#define PT_ADDR_MASK         0x000FFFFFFFFFF000ULL

static inline uint64_t rd_cr3(void){
    uint64_t v; __asm__ __volatile__("mov %%cr3,%0" : "=r"(v)); return v;
}
static inline void fb_flush_tlb(void){
    uint64_t v = rd_cr3();
    __asm__ __volatile__("mov %0,%%cr3" :: "r"(v) : "memory");
}

// Walk the active 4-level tables for `va`.
// Returns 0 = NOT mapped, 1 = 1 GiB page, 2 = 2 MiB page, 3 = 4 KiB page.
static int vmm_probe(uint64_t va, uint64_t* out_phys){
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(rd_cr3() & PT_ADDR_MASK);
    uint64_t e = pml4[(va >> 39) & 511];
    if (!(e & 1)) return 0;
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(e & PT_ADDR_MASK);
    e = pdpt[(va >> 30) & 511];
    if (!(e & 1)) return 0;
    if (e & 0x80) {                                  // PS=1 -> 1 GiB page
        if (out_phys) *out_phys = (e & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL);
        return 1;
    }
    uint64_t* pd = (uint64_t*)(uintptr_t)(e & PT_ADDR_MASK);
    e = pd[(va >> 21) & 511];
    if (!(e & 1)) return 0;
    if (e & 0x80) {                                  // PS=1 -> 2 MiB page
        if (out_phys) *out_phys = (e & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL);
        return 2;
    }
    uint64_t* pt = (uint64_t*)(uintptr_t)(e & PT_ADDR_MASK);
    e = pt[(va >> 12) & 511];
    if (!(e & 1)) return 0;
    if (out_phys) *out_phys = (e & PT_ADDR_MASK) | (va & 0xFFFULL);
    return 3;
}

// Ensure `phys` is identity-mapped; install a 1 GiB page if it is not.
// Returns true if the address is mapped on exit.
static bool vmm_ensure_identity_1g(uint64_t phys){
    if (vmm_probe(phys, 0) != 0) return true;
    uint64_t* pml4  = (uint64_t*)(uintptr_t)(rd_cr3() & PT_ADDR_MASK);
    uint64_t  pml4i = (phys >> 39) & 511;
    uint64_t  e     = pml4[pml4i];
    uint64_t* pdpt;
    if (e & 1) {
        pdpt = (uint64_t*)(uintptr_t)(e & PT_ADDR_MASK);
    } else {
        pdpt = (uint64_t*)(uintptr_t)FB_SELFCHK_PDPT_PHYS;
        for (int i = 0; i < 512; i++) pdpt[i] = 0;
        pml4[pml4i] = FB_SELFCHK_PDPT_PHYS | 0x3;      // P|W
    }
    // 1 GiB page: address field must be 1 GiB aligned.  P|W|PS.
    pdpt[(phys >> 30) & 511] = (phys & ~0x3FFFFFFFULL) | 0x83;
    fb_flush_tlb();
    return vmm_probe(phys, 0) != 0;
}

static void fbmap_report(const char* tag, uint64_t va){
    uint64_t phys = 0;
    int gran = vmm_probe(va, &phys);
    serial_puts("[FBMAP] "); serial_puts(tag); serial_puts(" va=");
    serial_hex64(va);
    if (gran == 0) {
        serial_puts(" -> NOT MAPPED\n");
        return;
    }
    serial_puts(gran == 1 ? " -> 1GiB page phys=" :
                gran == 2 ? " -> 2MiB page phys=" : " -> 4KiB page phys=");
    serial_hex64(phys);
    serial_puts(phys == va ? " (identity)\n" : " (NOT identity!)\n");
}

// Verify the framebuffer the 64-bit GUI is about to draw into is genuinely
// reachable, BEFORE the first store.  Call from kmain64 ahead of any beacon.
static void fb_mapping_selfcheck(void){
    volatile uint8_t* vb = (volatile uint8_t*)0x5000;
    uint64_t fb64  = *(volatile uint64_t*)(vb + 0x10);
    uint32_t fb32  = *(volatile uint32_t*)(vb + 0x00);
    uint16_t h     = *(volatile uint16_t*)(vb + 0x06);
    uint16_t pitch = *(volatile uint16_t*)(vb + 0x09);

    serial_puts("[FBMAP] fb64="); serial_hex64(fb64);
    serial_puts(" fb32="); serial_hex(fb32); serial_puts("\n");

    if (fb64 <= 0xFFFFFFFFULL) {
        serial_puts("[FBMAP] low framebuffer: covered by the 0-8GiB identity pages\n");
        fbmap_report("low-fb", (uint64_t)(fb64 ? fb64 : fb32));
        return;
    }

    // >4 GiB GOP LFB -- the real-hardware case.  Check first AND last byte:
    // an 8.3 MiB 1920x1080 surface can straddle a 1 GiB boundary.
    uint64_t span = (uint64_t)pitch * (uint64_t)h;
    if (span == 0) span = 0x800000ULL;               // 8 MiB fallback guess
    uint64_t last = fb64 + span - 1;

    fbmap_report("high-fb.first", fb64);
    fbmap_report("high-fb.last ", last);

    // The 0xF0000000 window is what the 32-bit kernel drew through.  Report it
    // too: on real metal it identity-maps onto the PCI hole, which is exactly
    // the trap the old boot_beacon()/diag_mark() fell into.
    fbmap_report("compat-window", 0xF0000000ULL);

    bool ok = true;
    if (vmm_probe(fb64, 0) == 0) {
        serial_puts("[FBMAP] first page missing -> installing 1GiB identity page\n");
        ok &= vmm_ensure_identity_1g(fb64);
    }
    if (vmm_probe(last, 0) == 0) {
        serial_puts("[FBMAP] last page missing -> installing 1GiB identity page\n");
        ok &= vmm_ensure_identity_1g(last);
    }
    serial_puts(ok ? "[FBMAP] high framebuffer reachable\n"
                   : "[FBMAP] FAILED to map high framebuffer!\n");
}

// === UEFI real-hardware diagnostic freeze (REMOVABLE after we learn the
//     Huawei GOP format).  Paints a 2x2 format probe (R/G/B/W) plus a CYAN
//     bottom strip, then cli;hlt so the screen stays frozen for visual
//     inspection.  Interpretation on the target:
//       * quadrant + CYAN visible  -> 64-bit FB is reachable; the quadrant
//         colours reveal the real GOP pixel format / R-B swap.
//       * screen NOT updated (stays at the 32-bit beacon / black) -> the very
//         first 64-bit framebuffer store went nowhere.
//
//  HISTORICAL CORRECTION (2026-08-19): the bullet above used to conclude
//  ">4GiB LFB not identity-mapped in long mode (the known real-hardware
//  black-screen root cause)".  That conclusion was WRONG, and it was wrong
//  because of a bug in diag_mark() itself (see the fb64 test below): the
//  marker never wrote to the high LFB at all, it wrote to the 32-bit
//  kernel's 0xF0000000 window == physical 0xF0000000 == PCI hole.  The
//  1 GiB identity page for the high framebuffer IS built (switch32to64.asm
//  '.compat_path' emits serial 'H' after installing PDPT[fb>>30]); we simply
//  were not drawing through it.  Do not re-derive "mapping missing" from a
//  blank screen without first confirming the draw address. ===
static uint32_t diag_pack(uint8_t fmt, uint8_t r, uint8_t g, uint8_t b){
    if (fmt == 1)      return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;   // RGBX32
    else if (fmt == 0) return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;   // BGRX32
    else if (fmt == 3) return (uint32_t)((((uint16_t)r >> 3) << 11) |
                                        (((uint16_t)g >> 2) << 5) |
                                        ((uint16_t)b >> 3));                  // RGB565
    else               return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;   // RGB24/unknown
}
// Progressive milestone marker: fill the WHOLE screen with one colour (no
// freeze unless `freeze` is set).  Because each call overwrites the screen,
// the LAST colour the Huawei target shows is the last milestone that
// executed before the boot hung.  Insert one at each step; the final one
// (freeze=1) stops the machine before gui_enter so kmain64 vs gui_enter is
// isolated.  Colours are chosen to be distinct from the kernel's own
// GREEN(0,255,0)/YELLOW(255,255,0)/BLACK/WHITE beacons.
static void diag_mark(uint8_t r, uint8_t g, uint8_t b, bool freeze){
    volatile uint8_t* vb = (volatile uint8_t*)0x5000;
    uint16_t w     = *(volatile uint16_t*)(vb + 0x04);
    uint16_t h     = *(volatile uint16_t*)(vb + 0x06);
    uint16_t pitch = *(volatile uint16_t*)(vb + 0x09);
    uint8_t  fmt   = *(volatile uint8_t*)(vb + 0x0F);
    if (w == 0 || h == 0) { __asm__ __volatile__("cli;hlt"); }
    uint64_t fb64 = *(volatile uint64_t*)(vb + 0x10);
    uintptr_t fbva;
    // ROOT-CAUSE FIX (2026-08-19): the condition used to be
    //     if (fb64 == 0 || fb64 > 0xFFFFFFFF) -> use fb32
    // i.e. a genuine >4 GiB UEFI GOP framebuffer was DISCARDED in favour of
    // framebuffer_phys, which the 32-bit kernel had already rewritten to its
    // 0xF0000000 compat window.  In long mode that address identity-maps onto
    // physical 0xF0000000 (the PCI hole), so every milestone marker painted
    // undecoded MMIO instead of the screen: black display, no #PF, possible
    // bus wedge.  Same bug as gui.cpp boot_beacon(); see the note there.
    if (fb64 > 0xFFFFFFFFULL) {
        fbva = (uintptr_t)fb64;
    } else {
        uint32_t fb32 = *(volatile uint32_t*)(vb + 0x00);
        fbva = (uintptr_t)fb32;
    }
    uint32_t pstride = (pitch != 0) ? (pitch / 4u) : (uint32_t)w;
    volatile uint32_t* fb = (volatile uint32_t*)fbva;
    uint32_t px = diag_pack(fmt, r, g, b);
    for (uint32_t y = 0; y < h; y++) {
        volatile uint32_t* row = fb + (uint32_t)y * pstride;
        for (uint32_t x = 0; x < w; x++) row[x] = px;
    }
    if (freeze) __asm__ __volatile__("cli;hlt");
}

extern "C" void kmain64(){
    serial_puts("[K64-1] kmain64 entered\n");
    // Install the real 64-bit IDT so any synchronous fault (#PF/#GP/#UD/...)
    // is captured by fault_common() instead of triple-faulting silently.
    build_idt();
    serial_puts("[K64-IDT] installed\n");
    diag_step(1, "kmain64 post-build_idt");

    // --- Stage 8.1: bring up Application Processors (SMP) ---
    smp_init();
    diag_step(1, "kmain64 post-smp_init");

    // --- Init IDE Bus Master DMA (for fast bulk model loading) ---
    g_bm_base = ata_bm_base();
    g_prdt_phys = (uint32_t)(uintptr_t)g_prdt;
    serial_puts("[ATA] BM-DMA base=");
    serial_hex(g_bm_base);
    serial_puts(" prdt=");
    serial_hex(g_prdt_phys);
    serial_puts("\n");

    // --- DIAG: report what the 64-bit side sees for the framebuffer ---
    {
        volatile uint8_t* dbg_vb = (volatile uint8_t*)0x5000;
        uint64_t dbg_fb64 = *(volatile uint64_t*)(dbg_vb + 0x10);
        uint32_t dbg_fb32 = *(volatile uint32_t*)(dbg_vb + 0x00);
        uint16_t dbg_w = *(volatile uint16_t*)(dbg_vb + 0x04);
        uint16_t dbg_h = *(volatile uint16_t*)(dbg_vb + 0x06);
        uint8_t  dbg_fmt = *(volatile uint8_t*)(dbg_vb + 0x0F);
        serial_puts("[K64-FB] fb64="); serial_hex64(dbg_fb64);
        serial_puts(" fb32="); serial_hex(dbg_fb32);
        serial_puts(" w="); serial_dec(dbg_w);
        serial_puts(" h="); serial_dec(dbg_h);
        serial_puts(" fmt="); serial_dec(dbg_fmt);
        serial_puts("\n");
    }

    // Verify -- do not assume -- that the address the GUI is about to draw
    // into is actually mapped, BEFORE the first framebuffer store.  Installs a
    // 1 GiB identity page if switch32to64.asm somehow did not.  This is the
    // guard that would have caught the 0xF0000000-into-the-PCI-hole bug on
    // day one instead of after weeks of colour-beacon guesswork.
    fb_mapping_selfcheck();
    diag_step(3, "kmain64 post-fb_mapping_selfcheck");

    // Boot beacon: we are now in 64-bit long mode with the high framebuffer
    // identity-mapped by switch32to64.asm.  A solid GREEN screen proves the
    // 32->64 switch and page tables succeeded; a hang/fault here shows green.
    *(volatile uint8_t*)0x5101 = 7;   // milestone: 64-bit kernel entered
    boot_beacon(0, 255, 0);           // GREEN
    diag_step(2, "kmain64 post-boot_beacon");
    diag_mark(255, 0, 0, false);      // M1 RED  : post boot_beacon, FB reachable (64-bit)
    g_auto_gui = 1;   // default: enter GUI automatically after login
    // Under a hypervisor (QEMU) the default is the text shell so headless
    // tests get a console and every CPU cycle goes to GGUF inference; on
    // bare metal the graphical desktop stays the default.  The 32-bit
    // kernel can override this by setting byte 0x501C = 1 before switching
    // (os.img's auto-gui path hands the desktop to us).
    //
    // 0x501C, NOT 0x5010: 0x5010 is VbeInfo.framebuffer_phys64's first byte, so
    // the old flag address corrupted the framebuffer address itself (harmless
    // for <4GB framebuffers, fatal for the 0x4000000000 LFB real GOP hardware
    // reports).  0x501C is VbeInfo.reserved[0].
    {
        uint32_t a1=0, b1=0, c1=0, d1=0;
        __asm__ __volatile__("cpuid" : "=a"(a1), "=b"(b1), "=c"(c1), "=d"(d1) : "a"(1));
        if (c1 & (1u << 31)){
            if (*(volatile uint8_t*)0x501C == 1){
                g_auto_gui = 1;
                serial_puts("[K64] VM detected but GUI requested by 32-bit kernel\n");
            } else {
                g_auto_gui = 0;
                serial_puts("[K64] VM detected - staying in text shell (GUI off)\n");
            }
        }
    }

    // ---- Display setup: framebuffer console if VBE graphics is already ----
    // active, else fall back to VGA text mode.
    // When the 32-bit kernel (or stage2) already placed the display in a VBE
    // graphics mode, keep it and render the shell into the framebuffer -- the
    // same LFB that `make play` / GTK / SDL windows actually display.  This is
    // exactly what 32-bit kernel.cpp does (g_fb_console_mode + VGA_SHADOW +
    // fb_console_init).  The VBE info block at 0x5000 survives the switch into
    // long mode, so we can read it here.  Reading the raw flag bytes avoids a
    // dependency on the VbeInfo layout duplicated from gui.cpp.
    {
        volatile uint8_t* vbe = (volatile uint8_t*)0x5000;
        bool vbe_ok       = (vbe[0x0D] == 1);   // vbe_ok
        bool vbe_mode_set = (vbe[0x0E] == 1);   // vbe_mode_set
        if (vbe_ok && vbe_mode_set) {
            g_fb_console_mode = true;
            VGA_MEMORY = VGA_SHADOW;             // terminal writes to shadow
            for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
                VGA_SHADOW[i] = make_entry(' ', make_color(LIGHT_GREY, BLACK));
            fb_console_init();
            serial_puts("[K64-2] framebuffer console active (VBE graphics mode)\n");
        } else {
            vga_set_text_mode();
            serial_puts("[K64-2] VGA text mode set\n");
        }
        diag_mark(0, 0, 255, false);   // M2 BLUE : post fb_console / VGA setup
    }

    // ---- Check if VBE info is available (for GUI) ----
    volatile uint8_t* vbe_flag = (volatile uint8_t*)0x500D;
    if(*vbe_flag == 1){
        g_vbe_active = true;
        serial_puts("[K64-VBE] VBE info available (GUI ready)\n");
    }

    memset_(g_hist,0,sizeof(g_hist));
    memset_(g_diskbuf,0,sizeof(g_diskbuf));
    memset_(g_iobuf,0,sizeof(g_iobuf));
    memset_(g_writebuf,0,sizeof(g_writebuf));

    term.init();
    serial_puts("[K64-3] terminal init done\n");
    kbd  = Keyboard();
    mouse.init();
    serial_puts("[K64-4] mouse init done\n");

    // ---- RAM-backed SFS handoff (UEFI path) ----
    // bootuefi.c stages the whole SFS image in RAM before ExitBootServices and
    // publishes {magic, base, size} at 0x0900, because legacy IDE PIO
    // (ata_read_sector) returns all zeros on the q35/AHCI machines UEFI boots
    // on.  Without this the superblock probe fails, shell.mex is "file not
    // found", and the managed Win11 shell renders an empty (black) screen.
    // Identical handoff to the 32-bit CD-boot path, so one loader feeds both.
    {
        const volatile uint32_t* hs = (const volatile uint32_t*)(uintptr_t)0x0900;
        if (hs[0] == SFS_RAM_MAGIC && hs[1] != 0 && hs[2] != 0) {
            sfs.set_ram(hs[1], hs[2]);
            serial_hex(hs[1]);
            serial_puts(" size=");
            serial_hex(hs[2]);
            serial_puts("\n");
        } else {
        }
    }

    mkfs.init();
    sfs.init();
    fat32.init();
    if (sfs.mounted) serial_puts("[K64-6] SFS mounted\n");
    else             serial_puts("[K64-6] *** SFS NOT mounted ***\n");
    serial_puts("[K64-6] filesystem init done\n");
    diag_step(sfs.mounted ? 360u : 361u, sfs.mounted ? "sfs mounted ok" : "sfs NOT mounted");
    diag_mark(0, 255, 255, false);     // M3 CYAN : post filesystem init

    // ---- Memory management ----
    pmm_init();
    vmm_init();
    heap_init();
    serial_puts("[K64-7] memory management init done\n");
    diag_mark(255, 0, 255, false);     // M4 MAGENTA : post memory management (vmm_init)

    // ---- GUI initialization (if VBE info is available) ----
    // Initialize GUI but do NOT auto-enter. Boot to command line.
    // Users can type 'gui' to enter the desktop.
    if(g_vbe_active){
        register_gui_callbacks();
        gui_init();
        if(gui_available()){
            serial_puts("[K64-VBE] GUI initialized successfully (available via 'gui' command)\n");
        } else {
            serial_puts("[K64-VBE] GUI init failed - command line only\n");
            g_vbe_active = false;
        }
    }
    diag_mark(255, 128, 0, false);     // M5 ORANGE : post gui_init()

    // ---- Network initialization ----
    serial_puts("[K64-8] Initializing network...\n");
    {
        int net_ret = net_init();
        if(net_ret == 0){
            g_net_initialized = true;
            serial_puts("[K64-8] Network initialized successfully\n");
        } else {
            serial_puts("[K64-8] Network init failed (no NIC?)\n");
        }
    }

    term.set_color(make_color(GREEN,BLACK));
    term.write("Hello world from C++ 64-bit kernel!\n");
    serial_puts("[K64-5] Hello world written\n");
    term.set_color(make_color(WHITE,BLACK));
    term.write("64-bit long mode kernel active. Type 'switch' to return to 32-bit.\n\n");

    // File system status
    if (mkfs.mounted) {
        term.set_color(make_color(CYAN,BLACK));
        term.write("MKFS: mounted ("); term.write_dec(mkfs.sb.file_count);
        term.write(" files)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("MKFS: not formatted (use 'mkfs' to format)\n");
    }
    if (sfs.mounted) {
        term.set_color(make_color(CYAN,BLACK));
        term.write("SFS:  mounted ("); term.write_dec(sfs.sb.file_count);
        term.write(" files)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("SFS:  not found\n");
    }
    term.set_color(make_color(BROWN,BLACK));
    term.write("FAT32: not mounted (use 'part' + 'mount')\n");

    // Memory management status
    term.set_color(make_color(CYAN,BLACK));
    term.write("MEM:  "); term.write_dec((int)(pmm_mem_kb/1024)); term.write(" MiB RAM, ");
    term.write_dec((int)pmm_free_pages); term.write(" free pages, paging ");
    term.write(vmm_paging_on ? "ON" : "OFF");
    if (vmm_our_paging)       term.write(" (32-bit PSE)\n");
    else if (vmm_long_mode)   term.write(" (64-bit long mode)\n");
    else                      term.write(" (firmware)\n");

    // Network status
    if(g_net_initialized){
        term.set_color(make_color(GREEN,BLACK));
        term.write("NET:  UP  HTTP server on http://");
        term.write(net_ip_str());
        term.write(":8080\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("NET:  not detected (use 'netstart' to retry)\n");
    }

    // GUI status
    if(g_vbe_active){
        term.set_color(make_color(GREEN,BLACK));
        term.write("GUI:  VBE "); term.write_dec(gui_get_width());
        term.write("x"); term.write_dec(gui_get_height());
        term.write(" (auto-starts on boot; 'nogui' to disable)\n");
    } else {
        term.set_color(make_color(BROWN,BLACK));
        term.write("GUI:  not available (text mode)\n");
    }

    term.set_color(make_color(CYAN,BLACK));
    term.write("\nShell ready. Type 'help' for commands.\n");
    term.write("Tab=autocomplete, Arrows=cursor/history, PgUp/PgDn=scroll, Home/End, Ctrl+C/V/L.\n\n");

    // ---- AUTOTEST: probe embedded GGUF and run one real inference (serial) ----
    serial_puts("[AUTOTEST] probing embedded GGUF...\n");
    {
        uint64_t plba = 0;
        uint64_t psize = model_blob_probe(&plba);
        if (psize) {
            serial_puts("[AUTOTEST] model blob found at LBA ");
            serial_dec((uint32_t)plba);
            serial_puts(" size=");
            serial_dec((uint32_t)(psize>>20));
            serial_puts(" MB\n");
            serial_puts("[AUTOTEST] loading model (this may take a while)...\n");
            if (model_try_load()) {
                serial_puts("[AUTOTEST] model loaded; real inference (DeepSeek)...\n");
                qwen_generate("DeepSeek is which company?", 96, 0.8f, ask_emit);
                serial_puts("\n[AUTOTEST] inference done.\n");
            } else {
                serial_puts("[AUTOTEST] model load or inference FAILED.\n");
            }
        } else {
            serial_puts("[AUTOTEST] no model blob found.\n");
        }
    }

    // ---- Security subsystem init (parity with 32-bit kernel.cpp kmain) ----
    perm_init(perm_ui_console);   // wire the Y/N consent prompt UI hook
    userdb_load();
    seed_default_users();         // creates root/admin + guest/guest on first boot
    permdb_load();

    // ---- Sign-in ----
    // With a framebuffer present the desktop starts automatically and the
    // managed lock screen (csharp/apps/Shell/Login.cs) collects the
    // credentials, so the console prompt would just ask for the same
    // password twice.  Text-mode boots still fall back to it.
    const bool graphical_login = (g_auto_gui != 0) && g_vbe_active;
    if (graphical_login) {
        serial_puts("[K64] sign-in deferred to the graphical lock screen\n");
        term.write("Sign-in deferred to the graphical lock screen.\n");
    } else {
        login_prompt();           // blocks until a valid user logs in
    }

    // ---- Default-enable GUI (parity with 32-bit kernel) ----
    diag_mark(255, 255, 255, false);   // M6 WHITE : kmain64 reached gui_enter entry (no freeze)
    // Enter the GUI (and paint the desktop) when graphics are available.
    // The old voice-test block forced g_skip_enter_gui=true, which skipped the
    // entire native desktop paint and left real hardware stuck at the
    // firmware-default light-gray screen.  The GUI hang it worked around never
    // reproduced (enter_gui() returns cleanly), so we use the normal path.
    if (g_auto_gui && g_vbe_active) cmd_gui(nullptr);
    serial_puts("[M-AG]\n");

    // Phase 0 ggml adapter self-test (serial-only diagnostic; harmless).
    serial_puts("[M-BA]\n");
    gguf_adapter_selftest();
    serial_puts("[M-AA]\n");

    serial_puts("[M-ENDBLOCK]\n");

    char inbuf[HIST_LEN];
    int  hist_recall = -1;      // command history recall index (-1 = not recalling)

    serial_puts("[M-FOR]\n");

    for(;;){
        // Voice transport: poll COM2 (0x2F8) for a framed phrase on EVERY
        // main-loop iteration, independent of GUI state.  This guarantees the
        // voice pipe is always drained even if the GUI branch below is not
        // entered on a given boot -- the auto-GUI path can tear the desktop
        // down before we reach this loop, which previously left the poll
        // unreachable (serial_voice_poll only ran inside `if(gui_is_active())`).
        serial_voice_poll();

        // ---- GUI event loop ----
        // If GUI mode is active (entered via 'gui' command), run a separate
        // event loop that routes mouse/keyboard to the window manager.
        // ESC exits GUI mode and returns to text terminal.
        if(gui_is_active()){
            bool gui_prev_left = false;
            bool gui_prev_right = false;
            int gui_tick_counter = 0;
            while(gui_is_active()){
                if(g_net_initialized) net_poll();

                // Drive window animations / the managed shell repaint at
                // ~60fps -- without this the C# desktop only ever shows the
                // initial blank frame (32-bit kernel does the same).
                gui_animate_frame();

                // Voice transport: drain COM2 (0x2F8) every GUI frame so a
                // spoken phrase is always picked up while the desktop is live.
                serial_voice_poll();

                // Update clock every ~50 iterations
                if(++gui_tick_counter > 50){
                    gui_tick();
                    gui_tick_counter = 0;
                }

                uint8_t st = inb(0x64);
                if(st & 0x01){
                    uint8_t data = inb(0x60);
                    if(st & 0x20){
                        // ----- Mouse event -----
                        MouseEvent me = mouse.process(data);
                        if(me.valid){
                            // Mouse movement (PS/2 Y is inverted, GUI Y is down-positive)
                            if(me.dx != 0 || me.dy != 0){
                                gui_mouse_move(me.dx, -me.dy);
                            }
                            // Left button: down/up for dragging and clicking
                            if(me.left && !gui_prev_left){
                                gui_mouse_down();
                            } else if(!me.left && gui_prev_left){
                                gui_mouse_up();
                            }
                            gui_prev_left = me.left;
                            // Right button: fire the context-menu event (desktop /
                            // window / tray hit-testing happens inside gui.cpp).
                            if(me.right && !gui_prev_right){
                                gui_mouse_down_right();
                            }
                            gui_prev_right = me.right;
                        }
                    } else {
                        // ----- Keyboard event -----
                        KbdEvent e = kbd.process(data);
                        if(e.type == K_CHAR){
                            // ESC used to call gui_exit() here, so a single
                            // stray keypress tore the desktop down and looked
                            // like a crash.  It is now an ordinary key: the
                            // GUI only uses it to cancel the IME.  The text
                            // terminal stays reachable through the Start menu
                            // / desktop "Terminal" shortcut.
                            gui_handle_key(e.ch);
                        }
                    }
                }
            }
            // The desktop was torn down deliberately.  If this boot deferred
            // sign-in to the lock screen and it was never satisfied, collect
            // the credentials before handing over a shell.
            if (g_login_idx < 0) login_prompt();
            // After GUI exits, re-render terminal to framebuffer
            term.render();
            continue;  // Go back to prompt
        }

        if (g_mode == MODE_WRITE) {
            term.set_color(make_color(MAGENTA,BLACK));
            term.write(">> ");
        } else {
            // PowerShell-style prompt: "PS /path> "
            term.set_color(make_color(GREEN,BLACK));
            term.write("PS ");
            char pathbuf[FS_NAME_LEN * 4];
            build_prompt_path(pathbuf, sizeof(pathbuf));
            term.set_color(make_color(CYAN, BLACK));
            term.write(pathbuf);
            term.set_color(make_color(GREEN, BLACK));
            term.write("> ");
        }
        term.set_color(make_color(LIGHT_GREY,BLACK));
        // Mark where user input begins (after prompt text)
        term.begin_input();
        term.render();

        int inlen=0;
        hist_recall = -1;
        for(;;){
            // Poll network for incoming packets (HTTP server, ARP, etc.)
            if(g_net_initialized) net_poll();

            // Voice transport: drain COM2 (0x2F8) every text-input iteration
            // so a spoken phrase is handled even on the console boot path.
            serial_voice_poll();

            // Handle a staged 32-bit `ask64` question even with no keypress.
            auto_ask_backdoor();
            
            uint8_t st=inb(0x64);
            if(st&0x01){
                uint8_t data=inb(0x60);
                if(st&0x20){
                    // ----- Mouse event -----
                    MouseEvent me = mouse.process(data);
                    if(me.valid){
                        // Wheel scrolling
                        if(me.dz > 0)      term.scroll_view(-3);
                        else if(me.dz < 0) term.scroll_view(3);

                        // Mouse movement
                        if(me.dx != 0 || me.dy != 0){
                            term.update_mouse(me.dx, me.dy);
                        }

                        // Left button: drag selection
                        static bool prev_left = false;
                        if(me.left && !prev_left){
                            term.mouse_left_down();
                        } else if(me.left && prev_left){
                            term.mouse_left_drag();
                        } else if(!me.left && prev_left){
                            term.mouse_left_up();
                        }
                        prev_left = me.left;

                        // Right click: refocus to input
                        if(me.right){
                            term.mouse_click();
                        }
                    }
                } else {
                    // ----- Keyboard event -----
                    KbdEvent e=kbd.process(data);

                    if(e.type==K_TAB){
                        // Tab completion
                        term.snap_bottom();
                        // Sync inbuf from terminal before completion
                        term.get_line(inbuf, &inlen);
                        do_tab_complete(inbuf, &inlen);
                        // Update terminal display with result
                        term.set_line(inbuf, inlen);
                    } else if(e.type==K_CTRL_C){
                        // Ctrl+C: if text selected, copy to clipboard; otherwise abort input
                        if(term.has_selection()){
                            // Selection already auto-copied on mouse-up; just clear it
                            term.clear_selection();
                        } else {
                            // Abort current input line
                            term.snap_bottom();
                            term.put_char('^'); term.put_char('C');
                            term.put_char('\n');
                            inbuf[0] = 0;
                            inlen = 0;
                            break;  // exit inner loop, re-prompt
                        }
                    } else if(e.type==K_CTRL_V){
                        // Ctrl+V: paste clipboard
                        term.snap_bottom();
                        for(int i=0; i<g_clipboard_len; i++){
                            char c = g_clipboard[i];
                            if(c == '\n') continue;  // skip newlines
                            term.put_char(c);
                        }
                        term.get_line(inbuf, &inlen);
                        term.render();
                    } else if(e.type==K_CTRL_L){
                        // Ctrl+L: refocus to input (snap to bottom + clear selection)
                        term.clear_selection();
                        term.snap_bottom();
                    } else if(e.type==K_CTRL_UP){
                        // Ctrl+Up: previous clipboard history
                        clipboard_hist_prev();
                        term.snap_bottom();
                        term.set_color(make_color(CYAN, BLACK));
                        term.write("\n[Clipboard]: ");
                        term.write(g_clipboard);
                        term.put_char('\n');
                        term.set_color(make_color(LIGHT_GREY, BLACK));
                        term.render();
                    } else if(e.type==K_CTRL_DOWN){
                        // Ctrl+Down: next clipboard history
                        clipboard_hist_next();
                        term.snap_bottom();
                        term.set_color(make_color(CYAN, BLACK));
                        term.write("\n[Clipboard]: ");
                        term.write(g_clipboard);
                        term.put_char('\n');
                        term.set_color(make_color(LIGHT_GREY, BLACK));
                        term.render();
                    } else if(e.type==K_CHAR){
                        term.snap_bottom();
                        if(e.ch=='\n'){
                            // Sync inbuf before committing the line
                            term.get_line(inbuf, &inlen);
                            term.put_char('\n');
                            term.render();
                            break;
                        }
                        // Backspace and character insertion handled by term.put_char
                        // (which now supports cursor-position editing)
                        term.put_char(e.ch);
                        // Sync inbuf from terminal
                        term.get_line(inbuf, &inlen);
                        // Any character input resets history recall
                        hist_recall = -1;
                        term.render();
                    } else if(e.type==K_UP){
                        // PowerShell-style: Up recalls previous command from history
                        if(term.is_at_bottom() && g_mode == MODE_NORMAL && g_hist_count > 0){
                            if(hist_recall < 0){
                                // Start recalling from most recent
                                hist_recall = g_hist_count - 1;
                            } else if(hist_recall > 0){
                                hist_recall--;
                            }
                            if(hist_recall >= 0 && hist_recall < g_hist_count){
                                int hlen = strlen_(g_hist[hist_recall]);
                                term.set_line(g_hist[hist_recall], hlen);
                                term.get_line(inbuf, &inlen);
                            }
                        } else {
                            // Scrolled back: scroll view up
                            term.scroll_view(-1);
                        }
                    } else if(e.type==K_DOWN){
                        // PowerShell-style: Down goes to next command in history
                        if(term.is_at_bottom() && g_mode == MODE_NORMAL && hist_recall >= 0){
                            if(hist_recall < g_hist_count - 1){
                                hist_recall++;
                                int hlen = strlen_(g_hist[hist_recall]);
                                term.set_line(g_hist[hist_recall], hlen);
                                term.get_line(inbuf, &inlen);
                            } else {
                                // At the latest: clear the line
                                hist_recall = -1;
                                term.set_line("", 0);
                                term.get_line(inbuf, &inlen);
                            }
                        } else {
                            // Scrolled back: scroll view down
                            term.scroll_view(1);
                        }
                    } else if(e.type==K_LEFT){
                        // Left arrow: move cursor left
                        term.snap_bottom();
                        term.cursor_left();
                    } else if(e.type==K_RIGHT){
                        // Right arrow: move cursor right
                        term.snap_bottom();
                        term.cursor_right();
                    } else if(e.type==K_PAGEUP){
                        // Page Up: scroll view up by 10 lines
                        term.scroll_view(-10);
                    } else if(e.type==K_PAGEDN){
                        // Page Down: scroll view down by 10 lines
                        term.scroll_view(10);
                    } else if(e.type==K_HOME){
                        // Home: move cursor to start of input
                        term.snap_bottom();
                        term.cursor_home();
                    } else if(e.type==K_END){
                        // End: move cursor to end of input
                        term.snap_bottom();
                        term.cursor_end();
                    }
                }
            }
        }
        inbuf[inlen]=0;

        if (g_mode == MODE_WRITE) {
            if (inlen == 0) {
                // Empty line = save and exit write mode
                int ret = mkfs.create(g_write_name, g_writebuf, g_write_len);
                if (ret >= 0) {
                    term.write("Saved: "); term.write(g_write_name);
                    term.write(" ("); term.write_dec(g_write_len); term.write(" bytes)\n");
                } else {
                    term.write("Save failed (code "); term.write_dec(ret); term.write(")\n");
                }
                g_mode = MODE_NORMAL;
                g_write_len = 0;
            } else {
                if (g_write_len + inlen + 1 < FS_WRITEBUF_SIZE) {
                    memcpy_(g_writebuf + g_write_len, inbuf, inlen);
                    g_write_len += inlen;
                    g_writebuf[g_write_len++] = '\n';
                } else {
                    term.write("Buffer full! Auto-saving...\n");
                    mkfs.create(g_write_name, g_writebuf, g_write_len);
                    term.write("Saved: "); term.write(g_write_name);
                    term.write(" ("); term.write_dec(g_write_len); term.write(" bytes)\n");
                    g_mode = MODE_NORMAL;
                    g_write_len = 0;
                }
            }
        } else {
            run_command(inbuf);
        }
        term.render();
    }
}

// =====================================================================
//  C bridge used by the 64-bit Win32/Win64 subsystem (win32.cpp) so the
//  IE browser's AI button can reach the same on-kernel AI engine that
//  `ai init` / `ai ask` use.  Mirrors the 32-bit kernel.cpp definitions.
// =====================================================================
extern "C" int kern_ai_ready(void){ return g_ai_initialized ? 1 : 0; }

extern "C" int kern_ai_boot(void){
    if(g_ai_initialized) return 0;
    int r = ai_init("/boot/model.gguf");
    if(r == 0) g_ai_initialized = true;
    return r;
}

extern "C" int kern_ai_ask(const char* prompt, char* out, int outsz){
    if(!prompt || !out || outsz < 2) return -1;

    // Prefer the real GGUF transformer (qwen) running in 64-bit long mode.
    // This is what makes `distnet compute` answer with actual model weights
    // instead of the offline Markov fallback, even inside QEMU.
    if (qwen_ready() || model_try_load()){
        ask_collect_reset();
        char sys[768];
        kb_build_prompt(prompt, sys, (int)sizeof(sys));
        int n = qwen_chat(sys, prompt, 96, 0.8f, ask_collect);
        (void)n;
        g_dn_ai_buf[g_dn_ai_len] = 0;
        int i = 0;
        while (i < g_dn_ai_len && i < outsz - 1){ out[i] = g_dn_ai_buf[i]; i++; }
        out[i] = 0;
        return i;
    }

    // Offline fallback: Markov toy engine (requires ai_init to have booted).
    if(!g_ai_initialized && kern_ai_boot() != 0) return -2;
    char* r = ai_generate(prompt, 160);
    if(!r) return -3;
    int i = 0;
    while(r[i] && i < outsz - 1){ out[i] = r[i]; i++; }
    out[i] = 0;
    kfree(r);
    return i;
}
