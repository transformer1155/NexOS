#!/usr/bin/env python3
"""One-shot patcher: model externalisation (descriptor v2 + lazy load)."""
import io, sys, os

PATH = os.path.join(os.path.dirname(__file__), '..', '.attic64', 'kernel64.cpp')
src = io.open(PATH, 'r', encoding='utf-8', newline='').read()

def sub(old, new, tag):
    global src
    o = old.replace('\n', '\r\n')
    n = new.replace('\n', '\r\n')
    if o not in src:
        print('MISS  %s' % tag); sys.exit(2)
    if src.count(o) != 1:
        print('DUP(%d) %s' % (src.count(o), tag)); sys.exit(3)
    src = src.replace(o, n)
    print('OK    %s' % tag)

# ------------------------------------------------- NEX_MODEL_LAZY switch ----
sub(
"""#ifndef NEX_HEADLESS_STAGE
#define NEX_HEADLESS_STAGE 0
#endif""",
"""#ifndef NEX_HEADLESS_STAGE
#define NEX_HEADLESS_STAGE 0
#endif

// Lazy model loading.  The weights no longer live in the system image, so
// booting must NOT drag 379 MiB into ring0 before the OS is usable.  With
// NEX_MODEL_LAZY=1 the boot only PRINTS the descriptor it found and the
// weights are pulled in on demand ('model load').  Set it to 0 to restore the
// old "load and run one inference during boot" behaviour, which is how the
// externalised data-disk path gets regression-tested.
#ifndef NEX_MODEL_LAZY
#define NEX_MODEL_LAZY 1
#endif""",
'NEX_MODEL_LAZY switch')

# --------------------------------------------------- descriptor v2 block ---
sub(
"""#define MODEL_DATA_LBA  16384u
static const uint32_t g_model_hdr_lbas[] = { 61440u, 16383u, 8191u, 4095u };

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
}""",
"""#define MODEL_DATA_LBA  16384u

// The model now lives on the DATA disk (IDE secondary, 0x170) at a high LBA
// instead of being welded into the system image.  The descriptor sits at
// MODEL_HDR_LBA_DATA and the payload starts on the next sector.  LBA 20000 is
// well past the MKFS area (900..16116), so the filesystem and the weights
// never share a sector.
#define MODEL_HDR_LBA_DATA  20000u

// Legacy descriptor, still probed so an old 429 MB system image keeps working.
struct ModelBlobHdr {
    char     magic[8];      // "MINIMDL1"
    uint64_t size;          // payload bytes
    uint64_t start_lba;     // where the payload begins
};

// Descriptor v2: self-describing, so the kernel does not have to guess which
// channel and which drive the payload sits on.
struct ModelDescV2 {
    char     magic[8];      // "NEXMODL2"
    uint32_t version;       // 2
    uint32_t header_size;   // sizeof(ModelDescV2)
    uint64_t size;          // payload bytes
    uint64_t start_lba;     // where the payload begins on that disk
    uint16_t io_base;       // 0x1F0 primary / 0x170 secondary
    uint8_t  drive;         // 0xE0 master
    uint8_t  flags;         // bit0 lazy, bit1 verified
    uint32_t crc32;         // descriptor + payload head checksum
    char     name[48];      // human-readable model name
    uint8_t  reserved[432];
};

// A resolved model location.
struct ModelDesc {
    uint16_t io_base;
    uint8_t  drive;
    uint64_t hdr_lba;
    uint64_t start_lba;
    uint64_t size;
    uint32_t version;
    char     name[48];
};

// Where to look for a descriptor.  The data disk comes first: that is where
// the weights live now.  The remaining entries are the legacy system-disk
// positions, so an old image still boots and still finds its model.
struct ModelCand {
    uint16_t io_base;
    uint8_t  drive;
    uint32_t hdr_lba;
};
static const struct ModelCand g_model_cands[] = {
    { ATA_SECONDARY, 0xE0, MODEL_HDR_LBA_DATA },
    { ATA_PRIMARY,   0xE0, 61440u },
    { ATA_PRIMARY,   0xE0, 16383u },
    { ATA_PRIMARY,   0xE0, 8191u  },
    { ATA_PRIMARY,   0xE0, 4095u  },
};

static uint8_t*  g_model_blob   = 0;   // big_alloc'd copy of the weights
static uint64_t  g_model_bytes  = 0;   // payload size handed to the parser
static uint64_t  g_model_alloc  = 0;   // bytes actually allocated (for free)

static bool magic_is(const uint8_t* sec, const char* m){
    for (int i = 0; i < 8; i++) if (sec[i] != (uint8_t)m[i]) return false;
    return true;
}

// Resolve the model descriptor.  Returns true and fills *out when a
// descriptor with a plausible payload size was found.
static bool model_locate(struct ModelDesc* out){
    uint8_t sec[512];
    for (unsigned c = 0; c < sizeof(g_model_cands)/sizeof(g_model_cands[0]); c++){
        const struct ModelCand* cd = &g_model_cands[c];
        ata_read_sector_drive(cd->io_base, cd->drive, cd->hdr_lba, (uint16_t*)sec);
        if (ata_last_err != 0) continue;
        struct ModelDesc d;
        memset(&d, 0, sizeof(d));
        d.io_base = cd->io_base;
        d.drive   = cd->drive;
        d.hdr_lba = cd->hdr_lba;
        d.version = 1;
        if (magic_is(sec, "NEXMODL2")){
            ModelDescV2 v2;
            memcpy_(&v2, sec, sizeof(v2));
            if (v2.version != 2) continue;
            if (v2.header_size != (uint32_t)sizeof(ModelDescV2)) continue;
            if (v2.size == 0 || v2.size > (1ull << 32)) continue;
            d.size      = v2.size;
            d.start_lba = v2.start_lba ? v2.start_lba : (uint64_t)(cd->hdr_lba + 1);
            if (v2.io_base) d.io_base = v2.io_base;
            if (v2.drive)   d.drive   = v2.drive;
            d.version = 2;
            memcpy_(d.name, v2.name, 47);
            d.name[47] = 0;
            *out = d;
            return true;
        }
        if (magic_is(sec, "MINIMDL1")){
            ModelBlobHdr h;
            memcpy_(&h, sec, sizeof(h));
            if (h.size == 0 || h.size > (1ull << 32)) continue;
            d.size      = h.size;
            d.start_lba = h.start_lba ? h.start_lba : MODEL_DATA_LBA;
            memcpy_(d.name, "legacy MINIMDL1", 15);
            *out = d;
            return true;
        }
    }
    return false;
}

// Legacy accessor: returns the payload size (0 = none) and the start LBA.
static uint64_t model_blob_probe(uint64_t* out_lba){
    struct ModelDesc d;
    if (!model_locate(&d)) return 0;
    if (out_lba) *out_lba = d.start_lba;
    return d.size;
}""",
'descriptor v2 + model_locate')

# ------------------------------------- bulk PIO + descriptor-driven read ---
sub(
"""    return dst;
}

static void model_report_loaded(){""",
"""    return dst;
}

// Bulk PIO over an arbitrary channel/drive.  The secondary channel (0x170)
// has no usable BM-DMA or virtio fast path, so pulling the weights off the
// data disk has to go through here: one READ SECTORS command per 128
// sectors instead of one per sector.
static bool model_read_bulk_pio(uint8_t* dst, uint16_t io_base, uint8_t drive,
                                uint64_t lba, uint64_t size){
    uint64_t sectors = (size + 511) / 512;
    uint64_t i = 0;
    while (i < sectors){
        uint32_t n = (uint32_t)(sectors - i);
        if (n > 128) n = 128;
        if (!ata_read_multi_drive(io_base, drive, (uint32_t)(lba + i),
                                  (uint8_t)n, (uint16_t*)(dst + (i << 9)))){
            serial_puts("[MDL] PIO error at sector ");
            serial_dec((uint32_t)i);
            serial_puts(" (stage ");
            serial_dec(ata_last_err);
            serial_puts(")\\n");
            return false;
        }
        i += n;
        if ((i & 0x3FFFu) == 0u){
            serial_puts("[MDL] ");
            serial_dec((uint32_t)(i >> 11));
            serial_puts(" MB\\n");
        }
    }
    return true;
}

// Read the payload described by `d` into a fresh big_alloc'd buffer.
static uint8_t* model_blob_read_desc(const struct ModelDesc* d){
    bool primary = (d->io_base == ATA_PRIMARY) && (d->drive == 0xE0);
    if (primary) return model_blob_read(d->start_lba, d->size);
    uint64_t alloc = (d->size + 511) & ~511ull;   // whole sectors only
    uint8_t* dst = (uint8_t*)big_alloc((uint32_t)alloc);
    if (!dst) return 0;
    serial_puts("[MDL] data-disk read: io=0x");
    serial_puthex((uint8_t)(d->io_base >> 8));
    serial_puthex((uint8_t)(d->io_base & 0xFF));
    serial_puts(" lba=");
    serial_dec((uint32_t)d->start_lba);
    serial_puts(" size=");
    serial_dec((uint32_t)(d->size >> 20));
    serial_puts(" MB via bulk PIO\\n");
    if (!model_read_bulk_pio(dst, d->io_base, d->drive, d->start_lba, d->size)){
        big_free(dst, (uint32_t)alloc);
        return 0;
    }
    serial_puts("[MDL] read OK, head: ");
    for (int b = 0; b < 8; b++){ serial_puthex(dst[b]); serial_putc(' '); }
    serial_puts("\\n");
    return dst;
}

static void model_report_loaded(){""",
'model_read_bulk_pio + model_blob_read_desc')

# --------------------------------------------- cmd_model load (disk path) ---
sub(
"""        } else {
            size = model_blob_probe(&lba);
            if(!size){
                term.write("No model blob on disk (descriptor sector missing).\\n");
                term.write("Rebuild the image with a GGUF in build/, or use\\n");
                term.write("'model load <sfsfile>' for a small model.\\n");
                return;
            }
            char n[24];
            term.write("Reading "); uint_to_str_k64((uint32_t)(size >> 10), n);
            term.write(n); term.write(" KB from LBA ");
            uint_to_str_k64((uint32_t)lba, n); term.write(n); term.write(" ");
            g_model_blob = model_blob_read(lba, size);
            if(!g_model_blob){ term.write("\\nOut of memory (need ");
                uint_to_str_k64(approx_mb_k64(size), n); term.write(n);
                term.write(" MB contiguous).\\n"); return; }
        }
        g_model_bytes = size;""",
"""        } else {
            struct ModelDesc md;
            if(!model_locate(&md)){
                term.write("No model descriptor found.\\n");
                term.write("The system image ships WITHOUT weights; the model lives\\n");
                term.write("on the DATA disk (IDE secondary, LBA 20000).\\n");
                term.write("Build it with: make model-pack\\n");
                return;
            }
            size = md.size;
            lba  = md.start_lba;
            char n[24];
            term.write("Reading "); uint_to_str_k64((uint32_t)(size >> 10), n);
            term.write(n); term.write(" KB from the ");
            term.write(md.io_base == ATA_SECONDARY ? "data disk" : "system disk");
            term.write(" at LBA ");
            uint_to_str_k64((uint32_t)lba, n); term.write(n); term.write(" ");
            if (md.name[0]){ term.write("["); term.write(md.name); term.write("] "); }
            g_model_blob = model_blob_read_desc(&md);
            if(!g_model_blob){ term.write("\\nRead failed (I/O error or out of memory).\\n"); return; }
        }
        g_model_bytes = size;""",
'cmd_model load disk path')

# ------------------------------------------------ cmd_model status output --
sub(
"""            uint64_t lba = 0, size = model_blob_probe(&lba);
            if(size){
                char n[24];
                term.write("  A blob is available on disk: ");
                uint_to_str_k64((uint32_t)(size >> 10), n); term.write(n);
                term.write(" KB at LBA "); uint_to_str_k64((uint32_t)lba, n);
                term.write(n); term.write(" -> run 'model load'\\n");
            } else {
                term.write("  No blob on disk. Built-in engine handles 'ask'.\\n");
            }""",
"""            struct ModelDesc md;
            if(model_locate(&md)){
                char n[24];
                term.write("  A blob is available on disk: ");
                uint_to_str_k64((uint32_t)(md.size >> 10), n); term.write(n);
                term.write(" KB at LBA "); uint_to_str_k64((uint32_t)md.start_lba, n);
                term.write(n);
                term.write(md.io_base == ATA_SECONDARY ? " on the DATA disk" : " on the system disk");
                if (md.name[0]){ term.write(" ["); term.write(md.name); term.write("]"); }
                term.write(" -> run 'model load'\\n");
            } else {
                term.write("  No blob on disk. Built-in engine handles 'ask'.\\n");
            }""",
'cmd_model status output')

# ----------------------------------------------------- model unload frees --
sub(
"""        qwen_unload();
        if(g_model_blob) big_free(g_model_blob, (uint32_t)g_model_bytes);
        g_model_blob = 0; g_model_bytes = 0;""",
"""        qwen_unload();
        if(g_model_blob) big_free(g_model_blob, (uint32_t)(g_model_alloc ? g_model_alloc : g_model_bytes));
        g_model_blob = 0; g_model_bytes = 0; g_model_alloc = 0;""",
'model unload frees the right size')

# ------------------------------------------------------------ model_try_load --
sub(
"""static int model_try_load(void){
    if (qwen_ready()) return 1;
    serial_puts("[K64] loading embedded GGUF model...\\n");
    uint64_t lba = 0;
    uint64_t size = model_blob_probe(&lba);
    if (!size){ serial_puts("[K64] no model blob on disk\\n"); return 0; }
    uint8_t* blob = model_blob_read(lba, size);
    if (!blob){ serial_puts("[K64] blob read failed\\n"); return 0; }
    int rc = qwen_load(blob, size, 512);
    if (rc != 0){
        serial_puts("[K64] qwen_load failed: ");
        serial_puts(qwen_error());
        serial_puts("\\n");
        return 0;
    }
    serial_puts("[K64] model loaded\\n");
    return 1;
}""",
"""static int model_try_load(void){
    if (qwen_ready()) return 1;
    struct ModelDesc md;
    if (!model_locate(&md)){
        serial_puts("[K64] no model descriptor found (data disk carries no NEXMODL2)\\n");
        return 0;
    }
    serial_puts("[K64] loading GGUF model: ");
    serial_dec((uint32_t)(md.size >> 20));
    serial_puts(" MB from the ");
    serial_puts(md.io_base == ATA_SECONDARY ? "data disk" : "system disk");
    serial_puts(" [");
    serial_puts(md.name[0] ? md.name : "unnamed");
    serial_puts("]\\n");
    uint8_t* blob = model_blob_read_desc(&md);
    if (!blob){ serial_puts("[K64] blob read failed\\n"); return 0; }
    g_model_blob  = blob;
    g_model_bytes = md.size;
    g_model_alloc = (md.size + 511) & ~511ull;
    int rc = qwen_load(blob, md.size, 512);
    if (rc != 0){
        serial_puts("[K64] qwen_load failed: ");
        serial_puts(qwen_error());
        serial_puts("\\n");
        big_free(blob, (uint32_t)g_model_alloc);
        g_model_blob = 0; g_model_bytes = 0; g_model_alloc = 0;
        return 0;
    }
    serial_puts("[K64] model loaded\\n");
    return 1;
}""",
'model_try_load uses the descriptor')

# ------------------------------------------------------ kmain64 AUTOTEST ---
sub(
"""    // ---- AUTOTEST: probe embedded GGUF and run one real inference (serial) ----
    serial_puts("[AUTOTEST] probing embedded GGUF...\\n");
    {
        uint64_t plba = 0;
        uint64_t psize = model_blob_probe(&plba);
        if (psize) {
            serial_puts("[AUTOTEST] model blob found at LBA ");
            serial_dec((uint32_t)plba);
            serial_puts(" size=");
            serial_dec((uint32_t)(psize>>20));
            serial_puts(" MB\\n");
            serial_puts("[AUTOTEST] loading model (this may take a while)...\\n");
            if (model_try_load()) {
                serial_puts("[AUTOTEST] model loaded; real inference (DeepSeek)...\\n");
                qwen_generate("DeepSeek is which company?", 96, 0.8f, ask_emit);
                serial_puts("\\n[AUTOTEST] inference done.\\n");
            } else {
                serial_puts("[AUTOTEST] model load or inference FAILED.\\n");
            }
        } else {
            serial_puts("[AUTOTEST] no model blob found.\\n");
        }
    }""",
"""    // ---- AUTOTEST: locate the GGUF descriptor (serial) --------------------
    // The weights are NOT in the system image any more: they live on the data
    // disk.  Booting therefore only PRINTS where they are; pulling 379 MiB
    // into ring0 is an explicit 'model load' (or NEX_MODEL_LAZY=0) away.
    serial_puts("[AUTOTEST] probing model descriptor...\\n");
    {
        struct ModelDesc md;
        if (model_locate(&md)) {
            serial_puts("[AUTOTEST] model descriptor: ");
            serial_dec((uint32_t)(md.size>>20));
            serial_puts(" MB, io=0x");
            serial_puthex((uint8_t)(md.io_base>>8));
            serial_puthex((uint8_t)(md.io_base&0xFF));
            serial_puts(", lba=");
            serial_dec((uint32_t)md.start_lba);
            serial_puts(", v");
            serial_dec(md.version);
            serial_puts(", name=");
            serial_puts(md.name[0] ? md.name : "(unnamed)");
            serial_puts("\\n");
#if NEX_MODEL_LAZY
            serial_puts("[AUTOTEST] lazy: weights stay on disk; use 'model load'.\\n");
#else
            serial_puts("[AUTOTEST] NEX_MODEL_LAZY=0 -> loading model now...\\n");
            if (model_try_load()) {
                serial_puts("[AUTOTEST] model loaded; real inference (DeepSeek)...\\n");
                qwen_generate("DeepSeek is which company?", 96, 0.8f, ask_emit);
                serial_puts("\\n[AUTOTEST] inference done.\\n");
            } else {
                serial_puts("[AUTOTEST] model load or inference FAILED.\\n");
            }
#endif
        } else {
            serial_puts("[AUTOTEST] no model descriptor found (system image ships without weights).\\n");
        }
    }""",
'kmain64 AUTOTEST lazy')

io.open(PATH, 'w', encoding='utf-8', newline='').write(src)
print('written: %s (%d bytes)' % (PATH, len(src)))
