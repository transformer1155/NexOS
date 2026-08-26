// file_adapter.c -- GGUF blob I/O backend abstraction (Route A, phase 0).
//
// Freestanding: no libc, no malloc.  Backend instances live in a tiny static
// pool (a handful are ever alive at once); the pool is reclaimed on reboot.
// See file_adapter.h.
#include "file_adapter.h"

// ---- local byte copy (no libc) --------------------------------------
static void gg_copy(void* d, const void* s, uint64_t n){
    const uint8_t* ss = (const uint8_t*)s;
    uint8_t*       dd = (uint8_t*)d;
    for (uint64_t i = 0; i < n; i++) dd[i] = ss[i];
}

// ---- static instance pool -------------------------------------------
#define GGUF_IO_MAX 4
static gguf_io  g_io_pool[GGUF_IO_MAX];
static int      g_io_count = 0;

// ---- mem backend ----------------------------------------------------
typedef struct { const uint8_t* base; uint64_t size; } mem_ctx;
static mem_ctx g_mem_pool[GGUF_IO_MAX];

static uint64_t mem_read(gguf_io* io, uint64_t off, void* dst, uint64_t len){
    mem_ctx* c = (mem_ctx*)io->ctx;
    if (off >= c->size) return 0;
    if (off + len > c->size) len = c->size - off;
    gg_copy(dst, c->base + off, len);
    return len;
}
static uint64_t mem_size(gguf_io* io){ return ((mem_ctx*)io->ctx)->size; }

gguf_io* gguf_io_mem_create(const uint8_t* base, uint64_t size){
    if (g_io_count >= GGUF_IO_MAX) return 0;
    int i = g_io_count++;
    g_mem_pool[i].base = base;
    g_mem_pool[i].size = size;
    g_io_pool[i].ctx  = &g_mem_pool[i];
    g_io_pool[i].read = mem_read;
    g_io_pool[i].size = mem_size;
    return &g_io_pool[i];
}

void gguf_io_mem_destroy(gguf_io* io){ (void)io; /* static pool; reclaimed on reboot */ }

// ---- disk backend ---------------------------------------------------
typedef struct {
    uint32_t            payload_lba;
    uint64_t            size;
    gguf_sector_read_fn read_fn;
} disk_ctx;
static disk_ctx g_disk_pool[GGUF_IO_MAX];

static uint64_t disk_read(gguf_io* io, uint64_t off, void* dst, uint64_t len){
    disk_ctx* c = (disk_ctx*)io->ctx;
    if (off >= c->size) return 0;
    if (off + len > c->size) len = c->size - off;
    uint64_t pos = 0;
    uint8_t*  out = (uint8_t*)dst;
    while (pos < len){
        uint64_t lba   = c->payload_lba + (off + pos) / 512ull;
        uint64_t insec = (off + pos) & 511ull;
        uint16_t sec[256];
        c->read_fn((uint32_t)lba, sec);
        uint64_t chunk = 512ull - insec;
        if (chunk > len - pos) chunk = len - pos;
        gg_copy(out + pos, ((const uint8_t*)sec) + insec, chunk);
        pos += chunk;
    }
    return len;
}
static uint64_t disk_size(gguf_io* io){ return ((disk_ctx*)io->ctx)->size; }

gguf_io* gguf_io_disk_create(const uint32_t* hdr_lbas, gguf_sector_read_fn read_fn){
    if (g_io_count >= GGUF_IO_MAX) return 0;
    uint8_t sec[512];
    for (int h = 0; hdr_lbas[h]; h++){
        read_fn(hdr_lbas[h], (uint16_t*)sec);
        // magic "MINIMDL1"
        const char* m = "MINIMDL1";
        int hit = 1;
        for (int i = 0; i < 8; i++) if (sec[i] != (uint8_t)m[i]) { hit = 0; break; }
        if (!hit) continue;
        uint64_t size = 0, start = 0;
        for (int i = 0; i < 8; i++) size  |= (uint64_t)sec[8  + i] << (8 * i);
        for (int i = 0; i < 8; i++) start |= (uint64_t)sec[16 + i] << (8 * i);
        if (size == 0 || size > (1ull << 32)) continue;
        int idx = g_io_count++;
        g_disk_pool[idx].payload_lba = (uint32_t)(start ? start : 16384u);
        g_disk_pool[idx].size        = size;
        g_disk_pool[idx].read_fn     = read_fn;
        g_io_pool[idx].ctx  = &g_disk_pool[idx];
        g_io_pool[idx].read = disk_read;
        g_io_pool[idx].size = disk_size;
        return &g_io_pool[idx];
    }
    return 0;
}

void gguf_io_disk_destroy(gguf_io* io){ (void)io; /* static pool; reclaimed on reboot */ }
