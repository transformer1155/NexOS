// gguf_loader.c -- GGUF (v2/v3) parser over the backend-agnostic gguf_io.
//
// Strict Route A port, phase 0.  Freestanding: no libc, no STL, no dynamic
// allocation (the parser walks the file through gguf_io->read, which works
// equally over an in-RAM blob or raw disk sectors).  The parsing logic mirrors
// the proven gguf.cpp reader but is fully self-contained so the parallel ggml
// subsystem owns its own model front-end.
//
// GGUF value-type enum (stable across v2/v3 for types 0..12):
//   0 U8 1 I8 2 U16 3 I16 4 U32 5 I32 6 F32 7 BOOL 8 STRING 9 ARRAY
//   10 U64 11 I64 12 F64
#include "gguf_loader.h"

// ---- freestanding helpers -------------------------------------------
static int  gl_strcmp(const char* a, const char* b){
    while (*a && *a == *b){ a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static int  gl_strncmp(const char* a, const char* b, int n){
    for (int i = 0; i < n; i++){
        if (!*a || *a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        a++; b++;
    }
    return 0;
}
static int  gl_strlen(const char* s){ int n = 0; while (s[n]) n++; return n; }
static void gl_strcpy(char* d, const char* s, int cap){
    int i = 0; while (s[i] && i < cap - 1){ d[i] = s[i]; i++; } d[i] = 0;
}

static uint32_t rd32(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p){
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// Read a GGUF string (uint64 length + bytes) from `io` at *pos into out.
static void read_str(gguf_io* io, uint64_t* pos, char* out, int cap){
    uint8_t tmp[8];
    io->read(io, *pos, tmp, 8); *pos += 8;
    uint64_t len = rd64(tmp);
    int n = (int)len; if (n >= cap) n = cap - 1; if (n < 0) n = 0;
    uint64_t got = 0;
    while (got < (uint64_t)n){
        uint8_t chunk[64];
        uint64_t c = n - got; if (c > 64) c = 64;
        io->read(io, *pos + got, chunk, c);
        for (uint64_t i = 0; i < c; i++) out[got + i] = (char)chunk[i];
        got += c;
    }
    out[n] = 0; *pos += len;
}

// Byte size of a metadata VALUE scalar of the given type.
static int meta_elem_size(uint32_t t){
    if (t <= 1 || t == 7) return 1;          // u8 / i8 / bool
    if (t == 2 || t == 3) return 2;          // u16 / i16
    if ((t >= 4 && t <= 6) || t == 8) return 4; // u32/i32/f32 (+placeholder)
    if (t >= 10 && t <= 12) return 8;        // u64 / i64 / f64
    return 4;
}

int gguf_load(gguf_io* io, gguf_ctx* info){
    uint64_t size = io->size(io);
    if (size < 16) return -1;
    uint8_t magic[4];
    io->read(io, 0, magic, 4);
    if (rd32(magic) != 0x46554747u) return -2;   // "GGUF"

    uint8_t hdr[24];
    io->read(io, 0, hdr, 24);
    info->version      = rd32(hdr + 4);
    info->tensor_count = rd64(hdr + 8);
    info->kv_count     = rd64(hdr + 16);
    info->alignment    = 32;
    info->arch[0] = 0; info->quant[0] = 0;
    info->block_count = 0; info->embed_length = 0; info->head_count = 0;
    info->head_count_kv = 0; info->context_length = 0; info->feed_forward_length = 0;
    info->vocab_size = 0; info->rope_theta = 10000.0f; info->rms_eps = 1e-6f;
    info->tokens_offset = 0; info->merges_offset = 0; info->merges_count = 0;
    info->bos_id = 0xFFFFFFFFu; info->eos_id = 0xFFFFFFFFu;
    info->n_tensors = 0;

    uint64_t pos = 24;   // KV metadata starts after magic(4)+version(4)+
                         // tensor_count(8)+kv_count(8)

    // ---- metadata KV ----
    for (uint64_t k = 0; k < info->kv_count && pos < size; k++){
        char key[96]; read_str(io, &pos, key, sizeof(key));
        uint8_t tbuf[4];
        io->read(io, pos, tbuf, 4);
        uint32_t type = rd32(tbuf); pos += 4;

        if (type == 8){                    // STRING value
            char sval[128]; read_str(io, &pos, sval, sizeof(sval));
            if (gl_strcmp(key, "general.architecture") == 0)
                gl_strcpy(info->arch, sval, sizeof(info->arch));
            else if (gl_strcmp(key, "general.quantization_type") == 0)
                gl_strcpy(info->quant, sval, sizeof(info->quant));
            continue;
        }
        if (type == 9){                    // ARRAY value
            uint8_t sub_buf[4], cnt_buf[8];
            io->read(io, pos, sub_buf, 4); uint32_t sub = rd32(sub_buf); pos += 4;
            io->read(io, pos, cnt_buf, 8); uint64_t cnt = rd64(cnt_buf); pos += 8;
            uint64_t arr_start = pos;
            if (sub == 8){
                for (uint64_t i = 0; i < cnt; i++){ uint8_t l[8]; io->read(io, pos, l, 8); uint64_t sl = rd64(l); pos += 8 + sl; }
            } else {
                pos += cnt * (uint64_t)meta_elem_size(sub);
            }
            if (gl_strcmp(key, "tokenizer.ggml.tokens") == 0){
                info->vocab_size    = (uint32_t)cnt;
                info->tokens_offset = arr_start;
            } else if (gl_strcmp(key, "tokenizer.ggml.merges") == 0){
                info->merges_count  = cnt;
                info->merges_offset = arr_start;
            }
            continue;
        }

        // scalar value
        uint64_t u = 0; double fd = 0;
        int es = meta_elem_size(type);
        uint8_t vbuf[8];
        io->read(io, pos, vbuf, (uint64_t)es);
        if (es == 1)      u = vbuf[0];
        else if (es == 2) u = (uint64_t)(rd32(vbuf) & 0xFFFFu);
        else if (es == 4){
            u = rd32(vbuf);
            if (type == 6){ union { uint32_t u; float f; } uf; uf.u = rd32(vbuf); fd = (double)uf.f; }
        }
        else if (es == 8){
            u = rd64(vbuf);
            if (type == 12){ union { uint64_t u; double d; } ud; ud.u = rd64(vbuf); fd = ud.d; }
        }
        pos += (uint64_t)es;

        if (gl_strcmp(key, "general.alignment") == 0) info->alignment = u;
        else if (gl_strcmp(key, "tokenizer.ggml.bos_token_id") == 0) info->bos_id = (uint32_t)u;
        else if (gl_strcmp(key, "tokenizer.ggml.eos_token_id") == 0) info->eos_id = (uint32_t)u;
        else {
            int pl = gl_strlen(info->arch);
            if (pl > 0 && gl_strncmp(key, info->arch, pl) == 0 && key[pl] == '.'){
                const char* suf = key + pl + 1;
                if      (gl_strcmp(suf, "block_count") == 0)          info->block_count = (uint32_t)u;
                else if (gl_strcmp(suf, "embedding_length") == 0)     info->embed_length = (uint32_t)u;
                else if (gl_strcmp(suf, "attention.head_count") == 0) info->head_count = (uint32_t)u;
                else if (gl_strcmp(suf, "attention.head_count_kv") == 0) info->head_count_kv = (uint32_t)u;
                else if (gl_strcmp(suf, "context_length") == 0)       info->context_length = (uint32_t)u;
                else if (gl_strcmp(suf, "feed_forward_length") == 0)  info->feed_forward_length = (uint32_t)u;
                else if (gl_strcmp(suf, "vocab_size") == 0)           info->vocab_size = (uint32_t)u;
                else if (gl_strcmp(suf, "rope.freq_base") == 0)       info->rope_theta = (float)fd;
                else if (gl_strcmp(suf, "attention.layer_norm_rms_epsilon") == 0) info->rms_eps = (float)fd;
            }
        }
    }
    if (info->head_count_kv == 0) info->head_count_kv = info->head_count;
    if (info->alignment == 0)     info->alignment = 32;

    // ---- tensor info ----
    for (uint64_t t = 0; t < info->tensor_count && pos < size &&
         (uint64_t)info->n_tensors < GGUF_LOADER_MAX_TENSORS; t++){
        gguf_loader_tensor* T = &info->tensors[info->n_tensors];
        read_str(io, &pos, T->name, sizeof(T->name));
        uint8_t nd[4]; io->read(io, pos, nd, 4); T->n_dims = rd32(nd); pos += 4;
        for (int dd = 0; dd < 4; dd++) T->dims[dd] = 0;
        for (uint32_t dd = 0; dd < T->n_dims && dd < 4; dd++){
            uint8_t d[8]; io->read(io, pos, d, 8); T->dims[dd] = rd64(d); pos += 8;
        }
        uint8_t tb[4], ob[8];
        io->read(io, pos, tb, 4); T->type = rd32(tb); pos += 4;
        io->read(io, pos, ob, 8); T->offset = rd64(ob); pos += 8;
        info->n_tensors++;
    }

    // tensor data begins after the info arrays, aligned to `alignment`
    uint64_t tdata = pos;
    uint64_t align = info->alignment;
    if (align > 1) tdata = (tdata + align - 1) & ~(align - 1);
    info->tensor_data_offset = tdata;
    return 0;
}

const char* gguf_loader_type_name(uint32_t type){
    switch (type){
        case 0:  return "F32";   case 1:  return "F16";   case 20: return "BF16";
        case 2:  return "Q4_0";  case 3:  return "Q4_1";  case 6:  return "Q5_0";
        case 7:  return "Q5_1";  case 8:  return "Q8_0";  case 9:  return "Q8_1";
        case 10: return "Q2_K";  case 11: return "Q3_K";  case 12: return "Q4_K";
        case 13: return "Q5_K";  case 14: return "Q6_K";  case 15: return "Q8_K";
        case 23: return "I8";    case 24: return "I16";   case 25: return "I32";
        default: return "?";
    }
}
