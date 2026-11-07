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

// =====================================================================
//  TSS layout -- must match the CPU mode we actually run in
// ---------------------------------------------------------------------
//  THIS WAS THE RING-3 HANG.
//
//  The BIOS image boots into plain 32-bit protected mode (stage2 sets
//  CR0.PE only; EFER.LME is never touched).  In that mode a TSS uses the
//  CLASSIC 32-BIT LAYOUT:
//
//        offset 0 : prev_tss (32-bit)
//        offset 4 : esp0     <-- ring-0 stack the CPU loads on a
//        offset 8 : ss0          ring3 -> ring0 trap through a gate
//
//  gdt.cpp previously declared a 64-bit (IA-32e) Tss64, whose `rsp0`
//  lives at offset 8 and leaves offset 4 as ALIGNMENT PADDING.  Under a
//  32-bit TSS the CPU read esp0 from offset 4, got the zero padding, and
//  on the very first `int 0x80` from ring 3 it tried to switch stacks to
//  address 0 -- faulting with no usable stack, so nothing could even run
//  a handler.  Result: a silent hang, exactly what we observed:
//
//     * iret into ring 3 SUCCEEDED (a store from the guest landed) -- iret
//       does not consult the TSS;
//     * the first int 0x80 HUNG -- the gate does.
//
//  Fix: keep BOTH layouts and select the right one at gdt_init() time by
//  testing EFER.LMA, so the same source is correct for the BIOS path
//  (32-bit protected mode) and for a future IA-32e/long-mode path.
// =====================================================================

// Classic 32-bit protected-mode TSS.  esp0 at offset 4 is the field that
// matters here.
struct Tss32 {
    uint32_t prev_tss;      //  0
    uint32_t esp0;          //  4   <- ring-0 stack pointer
    uint32_t ss0;           //  8
    uint32_t esp1;          // 12
    uint32_t ss1;           // 16
    uint32_t esp2;          // 20
    uint32_t ss2;           // 24
    uint32_t cr3;           // 28
    uint32_t eip;           // 32
    uint32_t eflags;        // 36
    uint32_t eax;           // 40
    uint32_t ecx;           // 44
    uint32_t edx;           // 48
    uint32_t ebx;           // 52
    uint32_t esp;           // 56
    uint32_t ebp;           // 60
    uint32_t esi;           // 64
    uint32_t edi;           // 68
    uint32_t es;            // 72
    uint32_t cs;            // 76
    uint32_t ss;            // 80
    uint32_t ds;            // 84
    uint32_t fs;            // 88
    uint32_t gs;            // 92
    uint32_t ldt;           // 96
    uint16_t trap;          // 100
    uint16_t iomap_base;    // 102
};

// IA-32e (long mode) TSS.  rsp0 at offset 8; offset 4 is padding.
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

static Tss32   g_tss32;
static Tss64   g_tss64;
// Which descriptor is live (1 = the 64-bit form was selected).
static int     g_tss_is_64 = 0;

// Dedicated ring-0 stack used during ring-3 -> ring-0 transitions.
static uint8_t g_kstack[16384];
// Dedicated stack for #DF / #MC (IST[1]).  Long mode REQUIRES these
// critical exceptions to use a non-zero IST; IST=0 makes the CPU shut
// down instead of delivering the fault.
static uint8_t g_crit_stack[8192];

// True when the CPU is executing in IA-32e long mode.
//
// NOTE: `rdmsr` of EFER (0xC0000080) is NOT safe here.  On a CPU running
// in plain 32-bit protected mode without IA-32e support, that MSR does not
// exist and `rdmsr` raises #GP.  gdt_init() runs BEFORE idt_init(), so a
// #GP at this point has no handler and hangs the boot instantly (that is
// exactly what a naive rdmsr probe did).
//
// Instead we ask CPUID whether IA-32e is even available, and only then
// read EFER.  If the extended-function bit is absent or the long-mode bit
// (EDX bit 29 of leaf 0x80000001) is clear, we are in plain 32-bit mode.
static int cpu_is_long_mode(){
    sp("[GDT] probing long mode...\n");
    uint32_t max_ext;
    __asm__ __volatile__("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx", "ecx", "edx");
    sp("[GDT] cpuid 80000000 ok\n");
    if (max_ext < 0x80000001u) return 0;          // no extended features

    uint32_t edx;
    __asm__ __volatile__("cpuid"
                         : "=d"(edx)
                         : "a"(0x80000001), "c"(0)
                         : "ebx");
    sp("[GDT] cpuid 80000001 ok\n");
    if (!(edx & (1u << 29))) return 0;            // no IA-32e support at all

    // IA-32e exists; EFER is now safe to read.
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    sp("[GDT] rdmsr efer ok\n");
    return (lo & (1u << 10)) ? 1 : 0;
}

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

    // 5: TSS 0x28 -- descriptor form depends on the CPU mode we booted in.
    //
    //   32-bit protected mode (BIOS image)  -> classic 16-bit-limit 32-bit
    //                                          TSS, type 0xB (busy 32-bit), 8-byte
    //   IA-32e long mode (UEFI image)      -> 16-byte 64-bit TSS, type 0x9
    //
    //   Getting this wrong is what hung ring-3: a 64-bit TSS loaded while
    //   the CPU was in 32-bit protected mode leaves offset 4 (32-bit
    //   TSS.esp0) holding zero, so the first ring3->ring0 trap switched
    //   stacks to address 0.
    g_tss_is_64 = cpu_is_long_mode();
    sp(g_tss_is_64 ? "[GDT] mode=LONG (64-bit TSS)\n"
                   : "[GDT] mode=32BIT (32-bit TSS)\n");
    uint64_t base;
    uint32_t lim;
    uint8_t  tss_type;
    if (g_tss_is_64){
        base = (uint64_t)(uintptr_t)&g_tss64;
        lim  = (uint32_t)(sizeof(Tss64) - 1);
        tss_type = 0x89;              // P=1 DPL=0 type=9 available 64-bit TSS
        // ring-0 stack + critical-exception IST
        g_tss64.rsp0   = (uint64_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
        g_tss64.ist[1] = (uint64_t)(uintptr_t)(g_crit_stack + sizeof(g_crit_stack));
        g_tss64.iomap  = (uint16_t)sizeof(Tss64);
    } else {
        base = (uint64_t)(uintptr_t)&g_tss32;
        lim  = (uint32_t)(sizeof(Tss32) - 1);
        tss_type = 0x89;              // P=1 DPL=0 type=9 AVAILABLE 32-bit TSS
        // NOTE: type 0xB is the BUSY form.  `ltr` requires an AVAILABLE
        // TSS -- loading a busy descriptor raises #GP, and because
        // gdt_init() runs before idt_init() there is no handler yet, so
        // the boot hangs with no report.  The CPU flips it to 0xB itself.
        // THE FIX: esp0 lives at offset 4 in a 32-bit TSS.
        g_tss32.ss0        = 0x10;    // ring-0 data selector (kdata)
        g_tss32.esp0       = (uint32_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
        g_tss32.iomap_base = (uint16_t)sizeof(Tss32);   // past struct: no I/O map
    }
    uint64_t w0 = ((uint64_t)(lim & 0xFFFF) << 0)
                | ((uint64_t)(base & 0xFFFF) << 16)
                | ((uint64_t)((base >> 16) & 0xFF) << 32)
                | ((uint64_t)tss_type << 40)
                | ((uint64_t)((lim >> 16) & 0x0F) << 48)
                | ((uint64_t)((base >> 24) & 0xFF) << 56);
    uint64_t w1 = (uint64_t)(base >> 32) & 0xFFFFFFFFULL;   // base bits 32..63
    g_gdt.tss_lo = w0;
    g_gdt.tss_hi = w1;

    // 6: TLS 0x38  DPL3 data, base 0 (set later by set_thread_area).
    g_gdt.tls = gdt_flat(0xF2);

    // 7: TLS pool 0x40..0xB8 (slots 8..23) — one descriptor per guest thread.
    for (int i = 0; i < 16; i++){ g_gdt.tls_pool[i] = 0; g_tls_used[i] = 0; }

    // TSS: ring-0 stack for traps coming from ring 3.
    if (!g_tss_is_64){
        g_tss32.ss0        = 0x10;
        g_tss32.esp0       = (uint32_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
        g_tss32.iomap_base = (uint16_t)sizeof(Tss32);
    } else {
        g_tss64.rsp0   = (uint64_t)(uintptr_t)(g_kstack + sizeof(g_kstack));
        g_tss64.ist[1] = (uint64_t)(uintptr_t)(g_crit_stack + sizeof(g_crit_stack));
        g_tss64.iomap  = (uint16_t)sizeof(Tss64);
    }

    g_gdtr.limit = (uint16_t)(sizeof(GdtTable) - 1);
    g_gdtr.base  = (uint32_t)(uintptr_t)&g_gdt;

    __asm__ __volatile__("lgdt %0" :: "m"(g_gdtr));
    sp("[GDT] lgdt ok\n");
    __asm__ __volatile__("ltr %%ax" :: "a"((uint16_t)0x28));
    sp("[GDT] ltr ok\n");

    // Report which TSS form went live: this is the single most useful clue
    // when a ring3 -> ring0 transition misbehaves.
    if (g_tss_is_64)
        sp("[GDT] TSS: 64-bit (IA-32e), rsp0 set\n");
    else
        sp("[GDT] TSS: 32-bit protected mode, esp0 @offset4 set\n");

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
