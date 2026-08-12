// =====================================================================
//  gdt.cpp  -  GDT + TSS setup for ring-3 isolation (Foundation 0)
// ---------------------------------------------------------------------
//  Extends the flat protected-mode GDT with user-mode (DPL 3) code/data
//  segments and a TSS. The TSS provides the ring-0 stack (esp0) that the
//  CPU switches to whenever a ring-3 process traps (int 0x80, #PF, IRQ).
// =====================================================================

#include "gdt.h"
#include <stdint.h>

// ---- tiny serial trace ----
static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static void sp(const char* s){ while(*s) outb(0x3F8,(uint8_t)*s++); }

struct GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
};

// 32-bit TSS (we only need esp0/ss0 for ring-3 -> ring-0 switches).
struct Tss {
    uint32_t backlink;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint32_t iomap;
};

static GdtEntry g_gdt[6];
static Tss      g_tss;
// Dedicated ring-0 stack used during ring-3 -> ring-0 transitions.
static uint8_t  g_kstack[16384];

static void gdt_set(uint32_t i, uint32_t base, uint32_t limit,
                    uint8_t access, uint8_t gran){
    g_gdt[i].limit_low = (uint16_t)(limit & 0xFFFF);
    g_gdt[i].base_low  = (uint16_t)(base & 0xFFFF);
    g_gdt[i].base_mid  = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[i].access    = access;
    g_gdt[i].gran      = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    g_gdt[i].base_high = (uint8_t)((base >> 24) & 0xFF);
}

struct Gdtr { uint16_t limit; uint32_t base; } __attribute__((packed));
static Gdtr g_gdtr;

void gdt_init(){
    // 0: null
    gdt_set(0, 0, 0, 0, 0);
    // 1: kcode 0x08  DPL 0, code
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xC0);
    // 2: kdata 0x10  DPL 0, data
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xC0);
    // 3: ucode 0x1B  DPL 3, code
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xC0);
    // 4: udata 0x23  DPL 3, data
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xC0);
    // 5: TSS 0x28
    uint32_t tss_base = (uint32_t)(uintptr_t)&g_tss;
    g_gdt[5].limit_low = (uint16_t)(sizeof(Tss) - 1);
    g_gdt[5].base_low  = (uint16_t)(tss_base & 0xFFFF);
    g_gdt[5].base_mid  = (uint8_t)((tss_base >> 16) & 0xFF);
    g_gdt[5].access    = 0x89;   // P=1, DPL=0, 32-bit TSS (10001001)
    g_gdt[5].gran      = 0x00;   // G=0, limit is in bytes
    g_gdt[5].base_high = (uint8_t)((tss_base >> 24) & 0xFF);

    // TSS: ring-0 stack for traps coming from ring 3.
    g_tss.esp0 = (uint32_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
    g_tss.ss0  = 0x10;   // kdata

    g_gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdtr.base  = (uint32_t)(uintptr_t)g_gdt;

    __asm__ __volatile__("lgdt %0" :: "m"(g_gdtr));
    __asm__ __volatile__("ltr %%ax" :: "a"((uint16_t)0x28));

    // Reload data segments to the new kdata selector (CS stays valid: the
    // new kcode descriptor equals the one stage2 installed, so the cached
    // CS is unchanged and correct).
    __asm__ __volatile__(
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw %%ax, %%ss\n"
        ::: "eax", "memory");

    sp("[GDT] ring-0 + ring-3 segments + TSS loaded\n");
}
