// Address Management Registry -- see addrman.h
//
// Self-contained: carries its own minimal COM1 (0x3F8) serial writer so it
// has zero dependency on kernel.cpp / kernel64.cpp's private static serial
// helpers (those are file-local).  Compiled into BOTH the 32-bit kernel.bin
// and the 64-bit kernel64.bin.

#include "addrman.h"

struct am_entry {
    uint64_t phys;
    uint64_t virt;
    uint64_t size;
    uint32_t flags;
    uint32_t set;
};

static am_entry g_addr[ADDR_COUNT];

// ---- self-contained serial output (no external deps) -------------------
static inline void am_outb(uint16_t p, uint8_t v) {
    __asm__ __volatile__("outb %0,%1" :: "a"(v), "Nd"(p));
}
static void am_puts(const char* s) { while (*s) am_outb(0x3F8, (uint8_t)*s++); }
static void am_puthex(uint64_t v) {
    static const char hexd[] = "0123456789ABCDEF";
    char buf[17];
    for (int i = 0; i < 16; i++) {
        uint8_t n = (uint8_t)((v >> ((15 - i) * 4)) & 0xF);
        buf[i] = hexd[n];
    }
    buf[16] = 0;
    am_puts(buf);
}

const char* addr_name(addr_key_t key) {
    switch (key) {
        case ADDR_VBE_INFO:      return "VBE_INFO";
        case ADDR_STAGE_MARK:    return "STAGE_MARK";
        case ADDR_FAULT_MARK:    return "FAULT_MARK";
        case ADDR_VGA_TEXT:      return "VGA_TEXT";
        case ADDR_FB_WIN:        return "FB_WIN";
        case ADDR_FB_PHYS:       return "FB_PHYS";
        case ADDR_FB_VIRT:       return "FB_VIRT";
        case ADDR_HEAP:          return "HEAP";
        case ADDR_KERNEL64_LOAD: return "KERNEL64_LOAD";
        default:                 return "?";
    }
}

void addr_set(addr_key_t key, uint64_t phys, uint64_t virt, uint64_t size, uint32_t flags) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return;
    g_addr[key].phys  = phys;
    g_addr[key].virt  = virt;
    g_addr[key].size  = size;
    g_addr[key].flags = flags;
    g_addr[key].set   = 1;
}
void addr_set_phys(addr_key_t key, uint64_t phys) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return;
    g_addr[key].phys = phys;
    g_addr[key].set  = 1;
}
void addr_set_virt(addr_key_t key, uint64_t virt) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return;
    g_addr[key].virt = virt;
    g_addr[key].set  = 1;
}
void addr_set_size(addr_key_t key, uint64_t size) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return;
    g_addr[key].size = size;
    g_addr[key].set  = 1;
}
void addr_set_flags(addr_key_t key, uint32_t flags) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return;
    g_addr[key].flags = flags;
    g_addr[key].set   = 1;
}

uint64_t addr_phys(addr_key_t key) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return 0;
    return g_addr[key].phys;
}
uint64_t addr_virt(addr_key_t key) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return 0;
    return g_addr[key].virt;
}
uint64_t addr_size(addr_key_t key) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return 0;
    return g_addr[key].size;
}
uint32_t addr_flags(addr_key_t key) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return 0;
    return g_addr[key].flags;
}
int addr_is_set(addr_key_t key) {
    if ((int)key < 0 || (int)key >= ADDR_COUNT) return 0;
    return (int)g_addr[key].set;
}

void addr_dump(void) {
    am_puts("[ADDR] registry dump:\n");
    for (int i = 0; i < ADDR_COUNT; i++) {
        if (!g_addr[i].set) continue;
        am_puts("  ");
        am_puts(addr_name((addr_key_t)i));
        am_puts(" phys=0x");
        am_puthex(g_addr[i].phys);
        am_puts(" virt=0x");
        am_puthex(g_addr[i].virt);
        am_puts(" sz=0x");
        am_puthex(g_addr[i].size);
        am_puts(" fl=0x");
        am_puthex(g_addr[i].flags);
        am_puts("\n");
    }
}
