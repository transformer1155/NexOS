// gguf.cpp - Minimal GGUF (v2/v3) parser for NexOS
//
// Freestanding: no libc, no STL, no dynamic allocation. Parses a GGUF file
// already loaded into a memory buffer and extracts the model hyper-parameters
// (architecture, quantization type, block count, embedding length, attention
// heads, context length, feed-forward length, vocab size, RoPE theta) plus the
// full tensor table (name / dims / type / offset). Real weight loading and
// inference are handled by gguf_infer.cpp.

#include "gguf.h"

// ---- freestanding string/mem helpers ----
static int gguf_strcmp(const char* a, const char* b){
    while (*a && *a == *b){ a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static int gguf_strncmp(const char* a, const char* b, int n){
    for (int i = 0; i < n; i++){
        if (!*a || *a != *b) return (int)(unsigned char)*a - (int)(unsigned char)*b;
        a++; b++;
    }
    return 0;
}
static int gguf_strlen(const char* s){ int n = 0; while (s[n]) n++; return n; }
static void gguf_strcpy(char* d, const char* s, int cap){
    int i = 0; while (s[i] && i < cap - 1){ d[i] = s[i]; i++; } d[i] = 0;
}
static void gguf_memcpy(void* d, const void* s, uint64_t n){
    const uint8_t* ss = (const uint8_t*)s; uint8_t* dd = (uint8_t*)d;
    for (uint64_t i = 0; i < n; i++) dd[i] = ss[i];
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

// Read a GGUF string (uint64 length + bytes) into out.
static void read_str(const uint8_t* d, uint64_t* pos, char* out, int cap){
    uint64_t len = rd64(d + *pos); *pos += 8;
    int n = (int)len; if (n >= cap) n = cap - 1; if (n < 0) n = 0;
    gguf_memcpy(out, d + *pos, (uint64_t)n); out[n] = 0; *pos += len;
}

// Byte size of a metadata VALUE type (scalar).
static int meta_elem_size(uint32_t t){
    if (t <= 1 || t == 7) return 1;        // u8 / i8 / bool
    if (t == 2 || t == 3) return 2;        // u16 / i16
    if ((t >= 4 && t <= 6) || t == 8) return 4; // u32/i32/f32 (+placeholder)
    if (t >= 10 && t <= 12) return 8;      // u64 / i64 / f64
    return 4;
}

// Skip a metadata VALUE of the given type, advancing pos.
__attribute__((unused)) static void skip_value(const uint8_t* d, uint64_t* pos, uint32_t type){
    if (type == 8){                        // STRING
        uint64_t len = rd64(d + *pos); *pos += 8 + len;
    } else if (type == 9){                  // ARRAY
        uint32_t sub = rd32(d + *pos); *pos += 4;
        uint64_t cnt = rd64(d + *pos); *pos += 8;
        if (sub == 8){
            for (uint64_t i = 0; i < cnt; i++){ uint64_t len = rd64(d + *pos); *pos += 8 + len; }
        } else {
            *pos += cnt * (uint64_t)meta_elem_size(sub);
        }
    } else {
        *pos += (uint64_t)meta_elem_size(type);
    }
}

int ai_gguf_parse(const uint8_t* data, uint64_t size, GGUFModelInfo* info){
    if (size < 16) return -1;
    if (rd32(data) != 0x46554747u) return -2;   // magic "GGUF"
    info->version = rd32(data + 4);
    uint64_t pos = 8;
    info->tensor_count = rd64(data + pos); pos += 8;
    info->kv_count     = rd64(data + pos); pos += 8;
    info->alignment    = 32;
    info->arch[0] = 0; info->quant[0] = 0;
    info->block_count = 0; info->embed_length = 0; info->head_count = 0;
    info->head_count_kv = 0; info->context_length = 0; info->feed_forward_length = 0;
    info->vocab_size = 0; info->rope_theta = 10000.0f;
    info->rms_eps = 1e-6f;
    info->tokens_offset = 0; info->merges_offset = 0; info->merges_count = 0;
    info->bos_id = 0xFFFFFFFFu; info->eos_id = 0xFFFFFFFFu;
    info->n_tensors = 0;

    // ---- metadata KV ----
    for (uint64_t k = 0; k < info->kv_count && pos < size; k++){
        char key[96]; read_str(data, &pos, key, sizeof(key));
        uint32_t type = rd32(data + pos); pos += 4;

        if (type == 8){                    // STRING value
            char sval[128]; read_str(data, &pos, sval, sizeof(sval));
            if (gguf_strcmp(key, "general.architecture") == 0)
                gguf_strcpy(info->arch, sval, sizeof(info->arch));
            else if (gguf_strcmp(key, "general.quantization_type") == 0)
                gguf_strcpy(info->quant, sval, sizeof(info->quant));
            continue;
        }
        if (type == 9){                    // ARRAY value
            uint32_t sub = rd32(data + pos); pos += 4;
            uint64_t cnt = rd64(data + pos); pos += 8;
            uint64_t arr_start = pos;      // first element (strings: uint64 len + bytes)
            if (sub == 8){
                for (uint64_t i = 0; i < cnt; i++){ uint64_t len = rd64(data + pos); pos += 8 + len; }
            } else {
                pos += cnt * (uint64_t)meta_elem_size(sub);
            }
            if (gguf_strcmp(key, "tokenizer.ggml.tokens") == 0){
                info->vocab_size    = (uint32_t)cnt;
                info->tokens_offset = arr_start;
            } else if (gguf_strcmp(key, "tokenizer.ggml.merges") == 0){
                info->merges_count  = cnt;
                info->merges_offset = arr_start;
            }
            continue;
        }

        // scalar value
        uint64_t u = 0; double fd = 0;
        int es = meta_elem_size(type);
        if (es == 1)      u = data[pos];
        else if (es == 2) u = rd32(data + pos) & 0xFFFFu;
        else if (es == 4){ u = rd32(data + pos);
                           if (type == 6){ uint32_t b = rd32(data + pos); float f; gguf_memcpy(&f, &b, 4); fd = (double)f; } }
        else if (es == 8){ u = rd64(data + pos);
                           if (type == 12){ uint64_t b = rd64(data + pos); gguf_memcpy(&fd, &b, 8); } }
        pos += (uint64_t)es;

        if (gguf_strcmp(key, "general.alignment") == 0) info->alignment = u;
        else if (gguf_strcmp(key, "tokenizer.ggml.bos_token_id") == 0) info->bos_id = (uint32_t)u;
        else if (gguf_strcmp(key, "tokenizer.ggml.eos_token_id") == 0) info->eos_id = (uint32_t)u;
        else {
            int pl = gguf_strlen(info->arch);
            if (pl > 0 && gguf_strncmp(key, info->arch, pl) == 0 && key[pl] == '.'){
                const char* suf = key + pl + 1;
                if      (gguf_strcmp(suf, "block_count") == 0)          info->block_count = (uint32_t)u;
                else if (gguf_strcmp(suf, "embedding_length") == 0)     info->embed_length = (uint32_t)u;
                else if (gguf_strcmp(suf, "attention.head_count") == 0) info->head_count = (uint32_t)u;
                else if (gguf_strcmp(suf, "attention.head_count_kv") == 0) info->head_count_kv = (uint32_t)u;
                else if (gguf_strcmp(suf, "context_length") == 0)       info->context_length = (uint32_t)u;
                else if (gguf_strcmp(suf, "feed_forward_length") == 0)  info->feed_forward_length = (uint32_t)u;
                else if (gguf_strcmp(suf, "vocab_size") == 0)           info->vocab_size = (uint32_t)u;
                else if (gguf_strcmp(suf, "rope.freq_base") == 0)       info->rope_theta = (float)fd;
                else if (gguf_strcmp(suf, "attention.layer_norm_rms_epsilon") == 0) info->rms_eps = (float)fd;
            }
        }
    }
    if (info->head_count_kv == 0) info->head_count_kv = info->head_count;
    if (info->alignment == 0)     info->alignment = 32;

    // ---- tensor info ----
    for (uint64_t t = 0; t < info->tensor_count && pos < size &&
         (uint64_t)info->n_tensors < GGUF_MAX_TENSORS; t++){
        GGUFTensor* T = &info->tensors[info->n_tensors];
        read_str(data, &pos, T->name, sizeof(T->name));
        T->n_dims = rd32(data + pos); pos += 4;
        for (int dd = 0; dd < 4; dd++) T->dims[dd] = 0;
        for (uint32_t dd = 0; dd < T->n_dims && dd < 4; dd++){
            T->dims[dd] = rd64(data + pos); pos += 8;
        }
        T->type   = rd32(data + pos); pos += 4;
        T->offset = rd64(data + pos); pos += 8;
        info->n_tensors++;
    }

    // tensor data begins after the info arrays, aligned to `alignment`
    uint64_t tdata = pos;
    uint64_t align = info->alignment;
    if (align > 1) tdata = (tdata + align - 1) & ~(align - 1);
    info->tensor_data_offset = tdata;
    return 0;
}

// ---------------------------------------------------------------------
//  ggml type geometry
// ---------------------------------------------------------------------
// (block_size, bytes_per_block) for every type we can touch.  A block size
// of 1 means the type is plain (F32/F16/I8...).
static void type_geom(uint32_t t, uint32_t* blk, uint32_t* bytes){
    switch (t){
        case GGUF_T_F32:  *blk = 1;   *bytes = 4;   return;
        case GGUF_T_F16:  *blk = 1;   *bytes = 2;   return;
        case GGUF_T_BF16: *blk = 1;   *bytes = 2;   return;
        case GGUF_T_I8:   *blk = 1;   *bytes = 1;   return;
        case GGUF_T_I16:  *blk = 1;   *bytes = 2;   return;
        case GGUF_T_I32:  *blk = 1;   *bytes = 4;   return;
        case GGUF_T_Q4_0: *blk = 32;  *bytes = 18;  return;  // d(f16) + 16B nibbles
        case GGUF_T_Q4_1: *blk = 32;  *bytes = 20;  return;  // d,m(f16) + 16B
        case GGUF_T_Q5_0: *blk = 32;  *bytes = 22;  return;
        case GGUF_T_Q5_1: *blk = 32;  *bytes = 24;  return;
        case GGUF_T_Q8_0: *blk = 32;  *bytes = 34;  return;  // d(f16) + 32B
        case GGUF_T_Q8_1: *blk = 32;  *bytes = 36;  return;
        case GGUF_T_Q2_K: *blk = 256; *bytes = 84;  return;
        case GGUF_T_Q3_K: *blk = 256; *bytes = 110; return;
        case GGUF_T_Q4_K: *blk = 256; *bytes = 144; return;  // d,dmin + 12B sc + 128B
        case GGUF_T_Q5_K: *blk = 256; *bytes = 176; return;
        case GGUF_T_Q6_K: *blk = 256; *bytes = 210; return;
        case GGUF_T_Q8_K: *blk = 256; *bytes = 292; return;
        default:          *blk = 1;   *bytes = 4;   return;
    }
}

uint64_t ai_gguf_type_size(uint32_t type, uint64_t n_elem){
    uint32_t blk = 1, bytes = 4;
    type_geom(type, &blk, &bytes);
    if (blk == 1) return n_elem * bytes;
    return (n_elem / blk) * bytes;
}

const char* ai_gguf_type_name(uint32_t type){
    switch (type){
        case GGUF_T_F32:  return "F32";
        case GGUF_T_F16:  return "F16";
        case GGUF_T_BF16: return "BF16";
        case GGUF_T_Q4_0: return "Q4_0";
        case GGUF_T_Q4_1: return "Q4_1";
        case GGUF_T_Q5_0: return "Q5_0";
        case GGUF_T_Q5_1: return "Q5_1";
        case GGUF_T_Q8_0: return "Q8_0";
        case GGUF_T_Q2_K: return "Q2_K";
        case GGUF_T_Q3_K: return "Q3_K";
        case GGUF_T_Q4_K: return "Q4_K";
        case GGUF_T_Q5_K: return "Q5_K";
        case GGUF_T_Q6_K: return "Q6_K";
        case GGUF_T_Q8_K: return "Q8_K";
        case GGUF_T_I8:   return "I8";
        case GGUF_T_I16:  return "I16";
        case GGUF_T_I32:  return "I32";
        default:          return "?";
    }
}

const GGUFTensor* ai_gguf_tensor(const GGUFModelInfo* info, const char* name){
    for (int i = 0; i < info->n_tensors; i++)
        if (gguf_strcmp(info->tensors[i].name, name) == 0) return &info->tensors[i];
    return (const GGUFTensor*)0;
}
