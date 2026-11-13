// =====================================================================
//  aiinfer_glue.cpp - fixed-address inference module for the 32-bit kernel
// ---------------------------------------------------------------------
//  kernel.bin is hard-capped at 576 KiB (0x10000..0xA0000 load window), so
//  the GGUF transformer engine (gguf_infer.cpp, ~50 KiB) cannot be linked
//  into it.  This module reuses the kernel64 pattern: it is linked as a
//  standalone flat binary at AIINFER_BASE = 0x18000000 (inside the
//  supervisor band the kernel identity-maps for the model), loaded from
//  disk at runtime, and called through the export table that lives at
//  binary offset 0 (.modhdr section, placed first by aiinfer.ld).
//
//  The module is SELF-SUFFICIENT for model bring-up: it talks to the
//  primary-master ATA disk and the 0x3F8 serial port directly, so the
//  kernel's only job is (a) stage the module itself into RAM and (b) call
//  ai_self_boot() / qwen_chat() through the table.  This keeps the kernel
//  side a few hundred bytes, which matters because kernel.bin is within
//  ~2 KB of the EBDA ceiling.
//
//  Memory layout (all identity-mapped by the kernel's 4 MiB PSE pages):
//    0x18000000  module code/data (~64 KiB) + .bss
//    0x18100000  model weights blob (loaded from disk, resident)
//    0x2D000000  runtime arena (apalloc/big_alloc bump, 32 MiB)
// =====================================================================
#include <stdint.h>
#include "../gguf_infer.h"

#define AI_DESC_LBA    61440u         // "MINIMDL1" descriptor sector
#define AI_BLOB_BASE   0x18100000u
#define AI_BLOB_LIMIT  0x2CF00000u
#define AI_ARENA_BASE  0x2D000000u
#define AI_ARENA_SIZE  0x02000000u    // 32 MiB

static uint32_t g_arena_used = 0;

// gguf_infer.cpp calls these two (declared there as extern "C").
extern "C" void* big_alloc(uint32_t bytes){
    bytes = (bytes + 63u) & ~63u;
    if (bytes > AI_ARENA_SIZE || g_arena_used + bytes > AI_ARENA_SIZE) return 0;
    uint8_t* p = (uint8_t*)(uintptr_t)(AI_ARENA_BASE + g_arena_used);
    g_arena_used += bytes;
    for (uint32_t i = 0; i < bytes; i++) p[i] = 0;
    return p;
}

extern "C" void big_free(void* /*p*/, uint32_t /*bytes*/){
    // Bump allocator: memory is reclaimed only by qwen_unload + ai_arena_reset
    // (a full reload).  Per-object free is intentionally a no-op; the engine
    // allocates a handful of large blocks per load, so waste is negligible.
}

extern "C" void ai_arena_reset(void){ g_arena_used = 0; }

// ---- minimal host I/O (primary-master ATA + COM1 serial) ------------
static inline void mi_outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t mi_inb(uint16_t p){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t mi_inw(uint16_t p){ uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(p)); return v; }

static void slog(const char* s){
    while (*s){ while(!(mi_inb(0x3F8+5)&0x20)); mi_outb(0x3F8, (uint8_t)*s++); }
}
static void slog_dec(uint32_t v){
    char b[12]; int i = 11; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v){ b[--i] = (char)('0' + v % 10); v /= 10; }
    slog(&b[i]);
}

static int mi_wait(uint16_t base, uint8_t mask, uint8_t want, uint32_t spins){
    for (uint32_t t = 0; t < spins; t++){
        uint8_t st = mi_inb(base+7);
        if (st & 0x01) return 0;                       // ERR
        if ((st & mask) == want) return 1;
    }
    return 0;
}

// Multi-sector PIO read from the primary master (LBA28, up to 200 sectors).
static int mi_read_block(uint32_t lba, uint16_t count, uint8_t* buf){
    uint16_t base = 0x1F0;
    if (!mi_wait(base, 0x80, 0, 8000000)) return 0;
    mi_outb(base+6, 0xE0 | ((lba>>24)&0x0F));
    mi_outb(base+1, 0x00);
    mi_outb(base+2, (uint8_t)count);
    mi_outb(base+3, lba & 0xFF);
    mi_outb(base+4, (lba>>8) & 0xFF);
    mi_outb(base+5, (lba>>16) & 0xFF);
    mi_outb(base+7, 0x20);                             // READ SECTORS
    for (uint16_t s = 0; s < count; s++){
        if (!mi_wait(base, 0x88, 0x08, 8000000)) return 0;   // DRQ set, BSY clear
        uint16_t* p = (uint16_t*)(buf + (uint32_t)s * 512);
        for (int i = 0; i < 256; i++) p[i] = mi_inw(base);
    }
    return 1;
}

// Self-contained model bring-up: descriptor -> blob -> qwen_load.
// Returns 0 when the engine is ready, negative otherwise (cached by the
// kernel so a failed boot is one cheap probe, not a 400 MB re-read).
extern "C" int ai_self_boot(void){
    static uint8_t desc[512];
    if (!mi_read_block(AI_DESC_LBA, 1, desc)){ slog("[AI32] desc fail\n"); return -1; }
    if (desc[0]!='M'||desc[1]!='I'||desc[2]!='N'||desc[3]!='I'||
        desc[4]!='M'||desc[5]!='D'||desc[6]!='L'||desc[7]!='1'){
        slog("[AI32] no model\n"); return -2;
    }
    unsigned long long msize = 0, mlba = 0;
    for (int i = 0; i < 8; i++){
        msize |= (unsigned long long)desc[8+i]  << (8*i);
        mlba  |= (unsigned long long)desc[16+i] << (8*i);
    }
    if (msize == 0 || msize > (AI_BLOB_LIMIT - AI_BLOB_BASE)){ slog("[AI32] bad size\n"); return -3; }
    uint32_t total_secs = (uint32_t)((msize + 511) / 512);
    slog("[AI32] model "); slog_dec((uint32_t)(msize / (1024*1024))); slog(" MB\n");
    uint8_t* blob = (uint8_t*)(uintptr_t)AI_BLOB_BASE;
    for (uint32_t off = 0; off < total_secs; off += 128){
        uint32_t cnt = (total_secs - off > 128) ? 128 : (total_secs - off);
        if (!mi_read_block((uint32_t)mlba + off, (uint16_t)cnt, blob + off * 512)){
            slog("[AI32] model read fail\n"); return -4;
        }
    }
    slog("[AI32] parse\n");
    int rc = qwen_load((const uint8_t*)(uintptr_t)AI_BLOB_BASE, msize, 512);
    if (rc != 0){ slog("[AI32] qwen_load fail\n"); return -5; }
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc":"=a"(lo),"=d"(hi));
    qwen_seed(lo ^ hi);
    slog("[AI32] ready\n");
    return 0;
}

// ---- export table (binary offset 0) --------------------------------
// The 32-bit kernel reads this table after copying the module into place.
struct AiModTable {
    uint32_t magic;           // 0x464E4941 "AINF"
    uint32_t version;         // 1
    uint32_t fn_self_boot;    // int      ai_self_boot(void)
    uint32_t fn_qwen_load;    // int      qwen_load(const uint8_t*, uint64_t, uint32_t)
    uint32_t fn_qwen_unload;  // void     qwen_unload(void)
    uint32_t fn_qwen_ready;   // int      qwen_ready(void)
    uint32_t fn_qwen_generate;// int      qwen_generate(const char*, int, float, QwenEmitFn)
    uint32_t fn_qwen_chat;    // int      qwen_chat(const char*, const char*, int, float, QwenEmitFn)
    uint32_t fn_qwen_reset;   // void     qwen_reset(void)
    uint32_t fn_arena_reset;  // void     ai_arena_reset(void)
    uint32_t bss_start;       // kernel zeroes [bss_start, bss_end) after load
    uint32_t bss_end;
};

extern "C" uint8_t __bss_start[];
extern "C" uint8_t __bss_end[];

__attribute__((used, section(".modhdr")))
AiModTable g_ai_mod_table = {
    0x464E4941u, 1u,
    (uint32_t)(uintptr_t)&ai_self_boot,
    (uint32_t)(uintptr_t)&qwen_load,
    (uint32_t)(uintptr_t)&qwen_unload,
    (uint32_t)(uintptr_t)&qwen_ready,
    (uint32_t)(uintptr_t)&qwen_generate,
    (uint32_t)(uintptr_t)&qwen_chat,
    (uint32_t)(uintptr_t)&qwen_reset,
    (uint32_t)(uintptr_t)&ai_arena_reset,
    (uint32_t)(uintptr_t)__bss_start,
    (uint32_t)(uintptr_t)__bss_end,
};
