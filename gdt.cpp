// =====================================================================
//  gdt.cpp  -  GDT + TSS setup for ring-3 isolation (Foundation 0)
// ---------------------------------------------------------------------
//  NOTE: under UEFI the CPU runs in IA-32e LONG MODE (4-level paging
//  active) but the kernel executes 32-bit *compatibility-mode* code
//  (enter_kernel.S far-returns into a CS.L=0 / CS.D=1 segment).
//
//  In long mode the descriptor formats are:
//    * code / data / system segment descriptors ....... 8 bytes (OK)
//    * TSS / LDT descriptors ........................ 16 bytes (REQUIRED)
//    * IDT gates ..................................... 16 bytes (REQUIRED)
//
//  The previous 8-byte 32-bit TSS descriptor was ILLEGAL in long mode:
//  `ltr` decoded it as 16 bytes, read the adjacent Gdtr as the high 8
//  bytes, produced a non-canonical TSS base >4GB, took #GP, then a
//  triple fault -> infinite reset loop.  We now install a proper
//  16-byte 64-bit TSS descriptor (type 9).
// =====================================================================

#include "gdt.h"
#include <stdint.h>

// ---- tiny serial trace ----
static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static void sp(const char* s){ while(*s) outb(0x3F8,(uint8_t)*s++); }

// 8-byte GDT table, laid out as raw 64-bit words so the descriptor
// stride is guaranteed to be exactly 8 bytes and the GDTR base is
// contiguous.  Slot 5 (offset 0x28) is the 16-byte TSS descriptor
// (spans tss_lo + tss_hi).
struct GdtTable {
    uint64_t null_ent;   // 0x00
    uint64_t kcode;      // 0x08  DPL0 32-bit compat code
    uint64_t kdata;      // 0x10  DPL0 data
    uint64_t ucode;      // 0x18  DPL3 32-bit compat code
    uint64_t udata;      // 0x20  DPL3 data
    uint64_t tss_lo;     // 0x28  low 8 bytes of 16-byte TSS descriptor
    uint64_t tss_hi;     // 0x30  high 8 bytes of TSS descriptor
    uint64_t tls;        // 0x38  DPL3 data, base = guest TLS (set_thread_area)
    uint64_t tls_pool[16]; // 0x40..0xB8  per-thread TLS descriptors (Stage 1)
};
static GdtTable g_gdt;

// Per-slot allocation bitmap for the TLS pool (slots 8..23).
static int g_tls_used[16];

// 64-bit TSS (IA-32e).  We only need rsp0 (ring-3 -> ring-0 stack) and
// the IST array for critical exceptions.  iomap points past the struct
// so no I/O permission bitmap is present.
struct Tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap;
};
static Tss64   g_tss64;
// Dedicated ring-0 stack used during ring-3 -> ring-0 transitions.
static uint8_t g_kstack[16384];
// Dedicated stack for #DF / #MC (IST[1]).  Long mode REQUIRES these
// critical exceptions to use a non-zero IST; IST=0 makes the CPU shut
// down instead of delivering the fault.
static uint8_t g_crit_stack[8192];

// Build an 8-byte flat 4 GiB segment descriptor (base=0, limit=4GiB-1,
// G=1, 32-bit operand size).  `access` is the byte written at bits 40..47.
static inline uint64_t gdt_flat(uint8_t access){
    // upper 16 bits: base_high(0) | gran(0xCF: G=1,D=1,limit 19:16=0xF)
    // lower 32 bits: base_low(0) | limit_low(0xFFFF)
    return ((uint64_t)0xCF << 48) | ((uint64_t)access << 40) | 0x0000FFFFULL;
}

struct Gdtr { uint16_t limit; uint32_t base; } __attribute__((packed));
static Gdtr g_gdtr;

void gdt_init(){
    // 0: null
    g_gdt.null_ent = 0;
    // 1: kcode 0x08  DPL 0, 32-bit compat code
    g_gdt.kcode = gdt_flat(0x9A);
    // 2: kdata 0x10  DPL 0, data
    g_gdt.kdata = gdt_flat(0x92);
    // 3: ucode 0x1B  DPL 3, 32-bit compat code
    g_gdt.ucode = gdt_flat(0xFA);
    // 4: udata 0x23  DPL 3, data
    g_gdt.udata = gdt_flat(0xF2);

    // 5: TSS 0x28  -- 16-byte 64-bit TSS descriptor (type 9, P=1 DPL=0)
    uint64_t base = (uint64_t)(uintptr_t)&g_tss64;
    uint32_t lim  = (uint32_t)(sizeof(Tss64) - 1);   // >= 0x67 for IST
    uint64_t w0 = ((uint64_t)(lim & 0xFFFF) << 0)
                | ((uint64_t)(base & 0xFFFF) << 16)
                | ((uint64_t)((base >> 16) & 0xFF) << 32)
                | ((uint64_t)0x89 << 40)             // P=1 DPL=0 type=9 (available 64-bit TSS)
                | ((uint64_t)((lim >> 16) & 0x0F) << 48)
                | ((uint64_t)((base >> 24) & 0xFF) << 56);
    uint64_t w1 = (uint64_t)(base >> 32) & 0xFFFFFFFFULL;   // base bits 32..63 (0 below 4GB)
    g_gdt.tss_lo = w0;
    g_gdt.tss_hi = w1;

    // 6: TLS 0x38  DPL3 data, base 0 (set later by set_thread_area).
    g_gdt.tls = gdt_flat(0xF2);

    // 7: TLS pool 0x40..0xB8 (slots 8..23) — one descriptor per guest thread.
    for (int i = 0; i < 16; i++){ g_gdt.tls_pool[i] = 0; g_tls_used[i] = 0; }

    // TSS: ring-0 stack for traps coming from ring 3.
    g_tss64.rsp0 = (uint64_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
    g_tss64.ist[1] = (uint64_t)(uintptr_t)(g_crit_stack + sizeof(g_crit_stack));
    g_tss64.iomap = (uint16_t)sizeof(Tss64);   // points past struct => no I/O map

    g_gdtr.limit = (uint16_t)(sizeof(GdtTable) - 1);
    g_gdtr.base  = (uint32_t)(uintptr_t)&g_gdt;

    __asm__ __volatile__("lgdt %0" :: "m"(g_gdtr));
    __asm__ __volatile__("ltr %%ax" :: "a"((uint16_t)0x28));

    // Reload data segments to the new kdata selector (CS stays valid: the
    // new kcode descriptor equals the one stage2/enter_kernel installed,
    // so the cached CS is unchanged and correct).
    __asm__ __volatile__(
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        ::: "eax", "memory");

    sp("[GDT] ring-0 + ring-3 segments + 64-bit TSS loaded\n");
}

// ---- Linux-compat TLS: set_thread_area (syscall 243) ----
// Writes the DPL3 TLS data descriptor (GDT slot 7 = selector 0x3B) with the
// guest's requested base/limit.  Returns the GDT entry number (7) on success,
// -1 on bad args.  Linux's set_thread_area reports the chosen entry back to
// the guest; musl then builds its GS selector as entry*8 + 3.
int gdt_set_tls(uint32_t base, uint32_t limit){
    if (limit == 0) limit = 0xFFFFF;                 // 4 GiB - 1 pages if unset
    // 32-bit data segment, granularity 1 (4 KiB units), DPL 3, writable.
    // base 24 bits in low descriptor; limit 20 bits split 16+4.
    uint32_t lim20 = limit >> 12;
    if (lim20 > 0xFFFFF) lim20 = 0xFFFFF;
    uint64_t d = ((uint64_t)(lim20 & 0xFFFF) << 0)
               | ((uint64_t)(base & 0xFFFF) << 16)
               | ((uint64_t)((base >> 16) & 0xFF) << 32)
               | ((uint64_t)0xF2 << 40)              // P=1 DPL=3 type=2 (data RW)
               | ((uint64_t)((lim20 >> 16) & 0x0F) << 48)
               | ((uint64_t)((base >> 24) & 0xFF) << 56);
    g_gdt.tls = d;
    __asm__ __volatile__("lgdt %0" :: "m"(g_gdtr));
    return 7;                                        // GDT entry number (selector 0x3B)
}

// ---- Linux-compat TLS pool (Stage 1): one descriptor per guest thread ----
// Allocate a DPL3 TLS data descriptor from the pool (slots 8..23).  Returns
// the GS selector on success, -1 if the pool is exhausted.  Each guest
// thread gets its own slot so every thread can have an independent TLS block.
int gdt_alloc_tls(uint32_t base, uint32_t limit){
    if (limit == 0) limit = 0xFFFFF;                 // 4 GiB - 1 pages if unset
    uint32_t lim20 = limit >> 12;
    if (lim20 > 0xFFFFF) lim20 = 0xFFFFF;
    uint64_t d = ((uint64_t)(lim20 & 0xFFFF) << 0)
               | ((uint64_t)(base & 0xFFFF) << 16)
               | ((uint64_t)((base >> 16) & 0xFF) << 32)
               | ((uint64_t)0xF2 << 40)              // P=1 DPL=3 type=2 (data RW)
               | ((uint64_t)((lim20 >> 16) & 0x0F) << 48)
               | ((uint64_t)((base >> 24) & 0xFF) << 56);
    for (int i = 0; i < 16; i++){
        if (!g_tls_used[i]){
            g_gdt.tls_pool[i] = d;
            g_tls_used[i] = 1;
            __asm__ __volatile__("lgdt %0" :: "m"(g_gdtr));
            return (int)(0x40 + i * 8);              // selector for slot 8+i
        }
    }
    return -1;
}

// Free a TLS descriptor previously returned by gdt_alloc_tls.
void gdt_free_tls(int selector){
    int i = (selector - 0x40) / 8;
    if (i >= 0 && i < 16 && g_tls_used[i]){
        g_tls_used[i] = 0;
        g_gdt.tls_pool[i] = 0;
    }
}
