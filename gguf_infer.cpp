// gguf_infer.cpp - real transformer inference over GGUF weights.
//
// Supports the qwen2 / qwen3 / llama tensor layouts as produced by
// llama.cpp's convert_hf_to_gguf.py.  Weights are *never* dequantised into
// RAM as a whole - a 1.7B model at Q4_K_M is ~1.1 GiB quantised but 6.8 GiB
// as float32, which does not fit.  Instead every matmul walks the quantised
// blocks and dequantises on the fly into the dot-product accumulator.
//
// Freestanding: no libc, no libm, no STL.  Scratch memory comes from the
// 64-bit kernel's page allocator (big_alloc), which manages the low 4 GiB.

#include "gguf_infer.h"

extern "C" void* big_alloc(uint32_t bytes);
extern "C" void  big_free(void* p, uint32_t bytes);

// ---------------------------------------------------------------------
//  tiny runtime helpers
// ---------------------------------------------------------------------
#ifdef GGUF_HOST_TEST
// Host-side harness build (tools/test_gguf_infer.cpp): no port I/O available.
// The harness provides the sink; keep libc out of this translation unit.
extern "C" void gguf_host_log(const char* s);
static void host_write(const char* s){ gguf_host_log(s); }
static void slog(const char* s){ host_write(s); }
static void slog_u(uint64_t v){
    char b[24]; int n = 0;
    if (!v){ host_write("0"); return; }
    while (v){ b[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    char o[25]; int i = 0;
    while (n) o[i++] = b[--n];
    o[i] = 0; host_write(o);
}
#else
static inline void gi_outb(uint16_t p, uint8_t v){
    __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(p));
}
static void slog(const char* s){ while (*s) gi_outb(0x3F8, (uint8_t)*s++); }
static void slog_u(uint64_t v){
    char b[24]; int n = 0;
    if (!v){ slog("0"); return; }
    while (v){ b[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    char o[25]; int i = 0;
    while (n) o[i++] = b[--n];
    o[i] = 0; slog(o);
}
#endif

static void gi_memset(void* d, int v, uint64_t n){
    uint8_t* p = (uint8_t*)d; for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)v;
}
static void gi_memcpy(void* d, const void* s, uint64_t n){
    uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s;
    for (uint64_t i = 0; i < n; i++) a[i] = b[i];
}
static int gi_strcmp(const char* a, const char* b){
    while (*a && *a == *b){ a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static int gi_strlen(const char* s){ int n = 0; while (s[n]) n++; return n; }

// Build "blk.<i>.<suffix>" into buf.
static void tname(char* buf, int idx, const char* suffix){
    int p = 0;
    buf[p++]='b'; buf[p++]='l'; buf[p++]='k'; buf[p++]='.';
    if (idx >= 100){ buf[p++] = (char)('0' + idx/100); }
    if (idx >= 10) { buf[p++] = (char)('0' + (idx/10)%10); }
    buf[p++] = (char)('0' + idx%10);
    buf[p++] = '.';
    for (int i = 0; suffix[i]; i++) buf[p++] = suffix[i];
    buf[p] = 0;
}

// ---------------------------------------------------------------------
//  math (no libm)
// ---------------------------------------------------------------------
static inline float mi_sqrtf(float x){
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u; u.f = x;
    u.i = (u.i >> 1) + 0x1FC00000u;          // rough seed
    float r = u.f;
    r = 0.5f * (r + x / r);
    r = 0.5f * (r + x / r);
    r = 0.5f * (r + x / r);
    return r;
}

static inline float mi_expf(float x){
    if (x > 88.0f)  return 3.0e38f;
    if (x < -87.0f) return 0.0f;
    float t = x * 1.44269504f;                       // x / ln2
    int   n = (int)(t >= 0 ? t + 0.5f : t - 0.5f);
    float r = x - (float)n * 0.69314718055994531f;   // |r| <= 0.347
    float p = 1.0f + r*(1.0f + r*(0.5f + r*(0.166666667f +
              r*(0.0416666667f + r*(0.00833333333f + r*0.00138888889f)))));
    int e = n + 127;
    if (e <= 0)   return 0.0f;
    if (e >= 255) return 3.0e38f;
    union { float f; uint32_t i; } u; u.i = ((uint32_t)e) << 23;
    return p * u.f;
}

// sin/cos with double-precision range reduction (RoPE angles reach pos*1.0
// where pos can be thousands, so float reduction would lose all accuracy).
static void mi_sincos(double x, float* so, float* co){
    const double TWO_PI  = 6.28318530717958648;
    const double PI      = 3.14159265358979324;
    const double HALF_PI = 1.57079632679489662;
    double k = x / TWO_PI;
    long long ki = (long long)(k >= 0 ? k + 0.5 : k - 0.5);
    x -= (double)ki * TWO_PI;                    // |x| <= pi
    int flip = 0;
    if (x >  HALF_PI){ x =  PI - x; flip = 1; }
    else if (x < -HALF_PI){ x = -PI - x; flip = 1; }
    double x2 = x * x;
    double sn = x * (1.0 - x2/6.0*(1.0 - x2/20.0*(1.0 - x2/42.0*(1.0 - x2/72.0))));
    double cs = 1.0 - x2/2.0*(1.0 - x2/12.0*(1.0 - x2/30.0*(1.0 - x2/56.0)));
    if (flip) cs = -cs;
    *so = (float)sn; *co = (float)cs;
}

static inline uint16_t rd16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }

static inline float f16_to_f32(uint16_t h){
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (uint32_t)((h >> 10) & 0x1Fu);
    uint32_t man  = (uint32_t)(h & 0x3FFu);
    uint32_t bits;
    if (exp == 0){
        if (man == 0) bits = sign;
        else {
            uint32_t e = 127 - 15 + 1;
            while (!(man & 0x400u)){ man <<= 1; e--; }
            man &= 0x3FFu;
            bits = sign | (e << 23) | (man << 13);
        }
    } else if (exp == 31){
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    union { uint32_t i; float f; } u; u.i = bits; return u.f;
}

static inline float bf16_to_f32(uint16_t h){
    union { uint32_t i; float f; } u; u.i = ((uint32_t)h) << 16; return u.f;
}

// ---------------------------------------------------------------------
//  K-quant helpers
// ---------------------------------------------------------------------
// 6-bit scale/min pairs packed into 12 bytes (llama.cpp get_scale_min_k4)
static inline void scale_min_k4(int j, const uint8_t* q, uint8_t* d, uint8_t* m){
    if (j < 4){
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (uint8_t)((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        *m = (uint8_t)((q[j + 4] >> 4)  | ((q[j - 0] >> 6) << 4));
    }
}

// ---------------------------------------------------------------------
//  quantised dot products:  sum_i w[i] * x[i]   over a row of n elements
// ---------------------------------------------------------------------
static float dot_f32(const uint8_t* w, const float* x, int n){
    const float* f = (const float*)w; float s = 0.0f;
    for (int i = 0; i < n; i++) s += f[i] * x[i];
    return s;
}
static float dot_f16(const uint8_t* w, const float* x, int n){
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += f16_to_f32(rd16(w + 2*i)) * x[i];
    return s;
}
static float dot_bf16(const uint8_t* w, const float* x, int n){
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += bf16_to_f32(rd16(w + 2*i)) * x[i];
    return s;
}
// Q4_0: 32 weights / 18 B, symmetric, w = d * (q - 8)
static float dot_q4_0(const uint8_t* w, const float* x, int n){
    float sum = 0.0f;
    int nb = n / 32;
    for (int i = 0; i < nb; i++){
        const uint8_t* b = w + i * 18;
        float d = f16_to_f32(rd16(b));
        const uint8_t* q = b + 2;
        const float* xx = x + i * 32;
        float acc = 0.0f;
        for (int j = 0; j < 16; j++){
            acc += (float)((int)(q[j] & 0xF) - 8) * xx[j];
            acc += (float)((int)(q[j] >>  4) - 8) * xx[j + 16];
        }
        sum += d * acc;
    }
    return sum;
}
// Q4_1: 32 weights / 20 B, w = d * q + m
static float dot_q4_1(const uint8_t* w, const float* x, int n){
    float sum = 0.0f;
    int nb = n / 32;
    for (int i = 0; i < nb; i++){
        const uint8_t* b = w + i * 20;
        float d = f16_to_f32(rd16(b));
        float m = f16_to_f32(rd16(b + 2));
        const uint8_t* q = b + 4;
        const float* xx = x + i * 32;
        float a = 0.0f, s = 0.0f;
        for (int j = 0; j < 16; j++){
            a += (float)(q[j] & 0xF) * xx[j] + (float)(q[j] >> 4) * xx[j + 16];
            s += xx[j] + xx[j + 16];
        }
        sum += d * a + m * s;
    }
    return sum;
}
// Q5_0: 32 weights / 22 B (d + 4B high bits + 16B nibbles), w = d * (q - 16)
static float dot_q5_0(const uint8_t* w, const float* x, int n){
    float sum = 0.0f;
    int nb = n / 32;
    for (int i = 0; i < nb; i++){
        const uint8_t* b = w + i * 22;
        float d = f16_to_f32(rd16(b));
        uint32_t qh = (uint32_t)b[2] | ((uint32_t)b[3] << 8) |
                      ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);
        const uint8_t* q = b + 6;
        const float* xx = x + i * 32;
        float acc = 0.0f;
        for (int j = 0; j < 16; j++){
            int lo = (int)(q[j] & 0xF) | (int)(((qh >> j) & 1u) << 4);
            int hi = (int)(q[j] >>  4) | (int)(((qh >> (j + 16)) & 1u) << 4);
            acc += (float)(lo - 16) * xx[j] + (float)(hi - 16) * xx[j + 16];
        }
        sum += d * acc;
    }
    return sum;
}
// Q8_0: 32 weights / 34 B, w = d * q
static float dot_q8_0(const uint8_t* w, const float* x, int n){
    float sum = 0.0f;
    int nb = n / 32;
    for (int i = 0; i < nb; i++){
        const uint8_t* b = w + i * 34;
        float d = f16_to_f32(rd16(b));
        const int8_t* q = (const int8_t*)(b + 2);
        const float* xx = x + i * 32;
        float acc = 0.0f;
        for (int j = 0; j < 32; j++) acc += (float)q[j] * xx[j];
        sum += d * acc;
    }
    return sum;
}
// Q4_K: 256 weights / 144 B. w = q * (sc * d) - (m * dmin), 8 sub-blocks of 32.
static float dot_q4_K(const uint8_t* w, const float* x, int n){
    float sum = 0.0f;
    int nb = n / 256;
    for (int i = 0; i < nb; i++){
        const uint8_t* b  = w + i * 144;
        float d    = f16_to_f32(rd16(b));
        float dmin = f16_to_f32(rd16(b + 2));
        const uint8_t* sc = b + 4;      // 12 bytes of packed 6-bit scales/mins
        const uint8_t* q  = b + 16;     // 128 bytes of nibbles
        const float* xx = x + i * 256;
        int is = 0;
        for (int j = 0; j < 256; j += 64){
            uint8_t s1, m1, s2, m2;
            scale_min_k4(is,     sc, &s1, &m1);
            scale_min_k4(is + 1, sc, &s2, &m2);
            float a1 = 0.0f, t1 = 0.0f, a2 = 0.0f, t2 = 0.0f;
            for (int l = 0; l < 32; l++){
                float v1 = xx[j + l];
                float v2 = xx[j + 32 + l];
                a1 += (float)(q[l] & 0xF) * v1; t1 += v1;
                a2 += (float)(q[l] >>  4) * v2; t2 += v2;
            }
            sum += d * ((float)s1 * a1 + (float)s2 * a2)
                 - dmin * ((float)m1 * t1 + (float)m2 * t2);
            q += 32; is += 2;
        }
    }
    return sum;
}
// Q5_K: 256 weights / 176 B (d, dmin, 12B scales, 32B high bits, 128B nibbles)
static float dot_q5_K(const uint8_t* w, const float* x, int n){
    float sum = 0.0f;
    int nb = n / 256;
    for (int i = 0; i < nb; i++){
        const uint8_t* b  = w + i * 176;
        float d    = f16_to_f32(rd16(b));
        float dmin = f16_to_f32(rd16(b + 2));
        const uint8_t* sc = b + 4;
        const uint8_t* qh = b + 16;
        const uint8_t* q  = b + 48;
        const float* xx = x + i * 256;
        int is = 0;
        uint8_t u1 = 1, u2 = 2;
        for (int j = 0; j < 256; j += 64){
            uint8_t s1, m1, s2, m2;
            scale_min_k4(is,     sc, &s1, &m1);
            scale_min_k4(is + 1, sc, &s2, &m2);
            float a1 = 0.0f, t1 = 0.0f, a2 = 0.0f, t2 = 0.0f;
            for (int l = 0; l < 32; l++){
                float v1 = xx[j + l];
                float v2 = xx[j + 32 + l];
                int lo = (int)(q[l] & 0xF) + ((qh[l] & u1) ? 16 : 0);
                int hi = (int)(q[l] >>  4) + ((qh[l] & u2) ? 16 : 0);
                a1 += (float)lo * v1; t1 += v1;
                a2 += (float)hi * v2; t2 += v2;
            }
            sum += d * ((float)s1 * a1 + (float)s2 * a2)
                 - dmin * ((float)m1 * t1 + (float)m2 * t2);
            q += 32; is += 2; u1 <<= 2; u2 <<= 2;
        }
    }
    return sum;
}
// Q6_K: 256 weights / 210 B (128B low, 64B high, 16 int8 scales, d)
static float dot_q6_K(const uint8_t* w, const float* x, int n){
    float sum = 0.0f;
    int nb = n / 256;
    for (int i = 0; i < nb; i++){
        const uint8_t* b  = w + i * 210;
        const uint8_t* ql = b;
        const uint8_t* qh = b + 128;
        const int8_t*  sc = (const int8_t*)(b + 192);
        float d = f16_to_f32(rd16(b + 208));
        const float* y = x + i * 256;
        float acc = 0.0f;
        for (int seg = 0; seg < 256; seg += 128){
            (void)seg;
            for (int l = 0; l < 32; l++){
                int is = l / 16;
                int q1 = (int)((ql[l]      & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = (int)((ql[l]      >>  4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = (int)((ql[l + 32] >>  4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                acc += (float)(sc[is + 0] * q1) * y[l]
                     + (float)(sc[is + 2] * q2) * y[l + 32]
                     + (float)(sc[is + 4] * q3) * y[l + 64]
                     + (float)(sc[is + 6] * q4) * y[l + 96];
            }
            y += 128; ql += 64; qh += 32; sc += 8;
        }
        sum += d * acc;
    }
    return sum;
}

// Block size required by a type (row length must be a multiple of it).
static int type_block(uint32_t t){
    switch (t){
        case GGUF_T_Q4_0: case GGUF_T_Q4_1: case GGUF_T_Q5_0:
        case GGUF_T_Q5_1: case GGUF_T_Q8_0: return 32;
        case GGUF_T_Q2_K: case GGUF_T_Q3_K: case GGUF_T_Q4_K:
        case GGUF_T_Q5_K: case GGUF_T_Q6_K: case GGUF_T_Q8_K: return 256;
        default: return 1;
    }
}
static int type_supported(uint32_t t){
    switch (t){
        case GGUF_T_F32: case GGUF_T_F16: case GGUF_T_BF16:
        case GGUF_T_Q4_0: case GGUF_T_Q4_1: case GGUF_T_Q5_0:
        case GGUF_T_Q8_0: case GGUF_T_Q4_K: case GGUF_T_Q5_K:
        case GGUF_T_Q6_K: return 1;
        default: return 0;
    }
}

static float dot_row(const uint8_t* w, uint32_t type, const float* x, int n){
    switch (type){
        case GGUF_T_F32:  return dot_f32(w, x, n);
        case GGUF_T_F16:  return dot_f16(w, x, n);
        case GGUF_T_BF16: return dot_bf16(w, x, n);
        case GGUF_T_Q4_0: return dot_q4_0(w, x, n);
        case GGUF_T_Q4_1: return dot_q4_1(w, x, n);
        case GGUF_T_Q5_0: return dot_q5_0(w, x, n);
        case GGUF_T_Q8_0: return dot_q8_0(w, x, n);
        case GGUF_T_Q4_K: return dot_q4_K(w, x, n);
        case GGUF_T_Q5_K: return dot_q5_K(w, x, n);
        case GGUF_T_Q6_K: return dot_q6_K(w, x, n);
        default: return 0.0f;
    }
}

// y[j] = <W_row_j, x>   for j in [0, n_out); each row is n_in elements.
static void matmul(float* y, const float* x, const uint8_t* w, uint32_t type,
                   int n_in, int n_out){
    uint64_t row_bytes = ai_gguf_type_size(type, (uint64_t)n_in);
    for (int j = 0; j < n_out; j++)
        y[j] = dot_row(w + (uint64_t)j * row_bytes, type, x, n_in);
}

// Dequantise a whole (small) tensor row into floats - used for norms/biases
// and for pulling a single embedding row out of token_embd.
static void dequant_row(float* dst, const uint8_t* w, uint32_t type, int n){
    switch (type){
        case GGUF_T_F32:
            gi_memcpy(dst, w, (uint64_t)n * 4); return;
        case GGUF_T_F16:
            for (int i = 0; i < n; i++) dst[i] = f16_to_f32(rd16(w + 2*i));
            return;
        case GGUF_T_BF16:
            for (int i = 0; i < n; i++) dst[i] = bf16_to_f32(rd16(w + 2*i));
            return;
        default: break;
    }
    // Quantised: reuse the dot kernels with one-hot probes would be O(n^2);
    // instead decode block-wise through a temporary unit basis.  Simpler:
    // decode by calling dot_row on unit vectors is too slow, so handle the
    // block formats directly for the ones we ship.
    int blk = type_block(type);
    if (blk == 1){ gi_memset(dst, 0, (uint64_t)n * 4); return; }
    for (int i = 0; i < n; i++) dst[i] = 0.0f;
    if (type == GGUF_T_Q4_0){
        int nb = n / 32;
        for (int i = 0; i < nb; i++){
            const uint8_t* b = w + i * 18;
            float d = f16_to_f32(rd16(b));
            const uint8_t* q = b + 2;
            for (int j = 0; j < 16; j++){
                dst[i*32 + j]      = d * (float)((int)(q[j] & 0xF) - 8);
                dst[i*32 + j + 16] = d * (float)((int)(q[j] >>  4) - 8);
            }
        }
    } else if (type == GGUF_T_Q8_0){
        int nb = n / 32;
        for (int i = 0; i < nb; i++){
            const uint8_t* b = w + i * 34;
            float d = f16_to_f32(rd16(b));
            const int8_t* q = (const int8_t*)(b + 2);
            for (int j = 0; j < 32; j++) dst[i*32 + j] = d * (float)q[j];
        }
    } else if (type == GGUF_T_Q4_K){
        int nb = n / 256;
        for (int i = 0; i < nb; i++){
            const uint8_t* b  = w + i * 144;
            float d    = f16_to_f32(rd16(b));
            float dmin = f16_to_f32(rd16(b + 2));
            const uint8_t* sc = b + 4;
            const uint8_t* q  = b + 16;
            float* o = dst + i * 256;
            int is = 0;
            for (int j = 0; j < 256; j += 64){
                uint8_t s1, m1, s2, m2;
                scale_min_k4(is,     sc, &s1, &m1);
                scale_min_k4(is + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++){
                    o[j + l]      = d * (float)s1 * (float)(q[l] & 0xF) - dmin * (float)m1;
                    o[j + 32 + l] = d * (float)s2 * (float)(q[l] >>  4) - dmin * (float)m2;
                }
                q += 32; is += 2;
            }
        }
    } else if (type == GGUF_T_Q6_K){
        int nb = n / 256;
        for (int i = 0; i < nb; i++){
            const uint8_t* b  = w + i * 210;
            const uint8_t* ql = b;
            const uint8_t* qh = b + 128;
            const int8_t*  sc = (const int8_t*)(b + 192);
            float d = f16_to_f32(rd16(b + 208));
            float* o = dst + i * 256;
            for (int seg = 0; seg < 2; seg++){
                for (int l = 0; l < 32; l++){
                    int is = l / 16;
                    int q1 = (int)((ql[l]      & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                    int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                    int q3 = (int)((ql[l]      >>  4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                    int q4 = (int)((ql[l + 32] >>  4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                    o[l]      = d * (float)(sc[is + 0] * q1);
                    o[l + 32] = d * (float)(sc[is + 2] * q2);
                    o[l + 64] = d * (float)(sc[is + 4] * q3);
                    o[l + 96] = d * (float)(sc[is + 6] * q4);
                }
                o += 128; ql += 64; qh += 32; sc += 8;
            }
        }
    } else if (type == GGUF_T_Q5_K){
        int nb = n / 256;
        for (int i = 0; i < nb; i++){
            const uint8_t* b  = w + i * 176;
            float d    = f16_to_f32(rd16(b));
            float dmin = f16_to_f32(rd16(b + 2));
            const uint8_t* sc = b + 4;
            const uint8_t* qh = b + 16;
            const uint8_t* q  = b + 48;
            float* o = dst + i * 256;
            int is = 0; uint8_t u1 = 1, u2 = 2;
            for (int j = 0; j < 256; j += 64){
                uint8_t s1, m1, s2, m2;
                scale_min_k4(is,     sc, &s1, &m1);
                scale_min_k4(is + 1, sc, &s2, &m2);
                for (int l = 0; l < 32; l++){
                    int lo = (int)(q[l] & 0xF) + ((qh[l] & u1) ? 16 : 0);
                    int hi = (int)(q[l] >>  4) + ((qh[l] & u2) ? 16 : 0);
                    o[j + l]      = d * (float)s1 * (float)lo - dmin * (float)m1;
                    o[j + 32 + l] = d * (float)s2 * (float)hi - dmin * (float)m2;
                }
                q += 32; is += 2; u1 <<= 2; u2 <<= 2;
            }
        }
    }
}

// ---------------------------------------------------------------------
//  runtime state
// ---------------------------------------------------------------------
struct LayerW {
    const uint8_t *wq, *wk, *wv, *wo, *wg, *wu, *wd;
    uint32_t       tq,  tk,  tv,  to,  tg,  tu,  td;
    float *attn_norm, *ffn_norm;
    float *bq, *bk, *bv;      // qwen2 QKV biases (null on qwen3/llama)
    float *qn, *kn;           // qwen3 QK-RMSNorm (null otherwise)
};

struct RT {
    int      loaded;
    const uint8_t* blob;
    uint64_t blob_size;
    GGUFModelInfo* info;
    const uint8_t* tdata;          // blob + tensor_data_offset

    int n_layer, n_embd, n_head, n_kv, head_dim, kv_dim, n_ff, n_vocab, n_ctx;
    float rope_theta, eps;
    int rope_neox, has_qk_norm, tie_embd;

    LayerW* L;
    float*  out_norm;
    const uint8_t* out_w;  uint32_t out_t;
    const uint8_t* tok_w;  uint32_t tok_t;  uint64_t tok_row_bytes;

    // activations
    float *x, *xb, *xb2, *hb, *hb2, *q, *att, *logits;
    float *kcache, *vcache;        // [layer][pos][kv_dim]

    // tokenizer
    uint32_t* tok_off;             // byte offset of token text inside blob
    uint16_t* tok_len;
    int32_t*  htab;
    uint32_t  hmask;
    uint8_t*  mapbuf;              // byte-level mapped input
    uint32_t  mapcap;

    uint64_t rt_bytes;
    int      pos;
    uint32_t rng;
};

static RT R;
static char  g_err[96] = {0};
static uint8_t* g_arena = 0;
static uint64_t g_arena_sz = 0, g_arena_used = 0;
static uint64_t g_info_bytes = 0;

static void set_err(const char* s){
    int i = 0; while (s[i] && i < 95){ g_err[i] = s[i]; i++; } g_err[i] = 0;
}
static void* apalloc(uint64_t n){
    n = (n + 63) & ~63ull;
    if (g_arena_used + n > g_arena_sz) return 0;
    void* p = g_arena + g_arena_used;
    g_arena_used += n;
    gi_memset(p, 0, n);
    return p;
}

// GPT-2 byte-level alphabet (same table transformers' bytes_to_unicode builds)
static uint16_t g_b2u[256];
static int16_t  g_u2b[512];
static int      g_bytemap_ready = 0;
static void init_bytemap(void){
    if (g_bytemap_ready) return;
    for (int i = 0; i < 512; i++) g_u2b[i] = -1;
    int n = 0;
    for (int b = 0; b < 256; b++){
        int direct = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
        if (direct) g_b2u[b] = (uint16_t)b;
        else        g_b2u[b] = (uint16_t)(256 + n++);
        g_u2b[g_b2u[b]] = (int16_t)b;
    }
    g_bytemap_ready = 1;
}
// encode one code point (< 0x800) as UTF-8, returns bytes written
static int cp_to_utf8(uint32_t cp, uint8_t* out){
    if (cp < 0x80){ out[0] = (uint8_t)cp; return 1; }
    out[0] = (uint8_t)(0xC0 | (cp >> 6));
    out[1] = (uint8_t)(0x80 | (cp & 0x3F));
    return 2;
}
// decode one UTF-8 sequence, returns bytes consumed (cp out)
static int utf8_to_cp(const uint8_t* s, int avail, uint32_t* cp){
    if (avail <= 0) return 0;
    uint8_t c = s[0];
    if (c < 0x80){ *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && avail >= 2){
        *cp = (uint32_t)((c & 0x1F) << 6) | (s[1] & 0x3F); return 2;
    }
    if ((c & 0xF0) == 0xE0 && avail >= 3){
        *cp = (uint32_t)((c & 0x0F) << 12) | (uint32_t)((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && avail >= 4){
        *cp = (uint32_t)((c & 0x07) << 18) | (uint32_t)((s[1] & 0x3F) << 12) |
              (uint32_t)((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    }
    *cp = c; return 1;
}

static uint32_t fnv1a(const uint8_t* s, int n){
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++){ h ^= s[i]; h *= 16777619u; }
    return h;
}
static int tok_lookup(const uint8_t* s, int n){
    if (!R.htab || n <= 0) return -1;
    uint32_t h = fnv1a(s, n) & R.hmask;
    for (uint32_t probe = 0; probe <= R.hmask; probe++){
        int32_t slot = R.htab[h];
        if (slot == 0) return -1;
        int id = slot - 1;
        if ((int)R.tok_len[id] == n){
            const uint8_t* t = R.blob + R.tok_off[id];
            int same = 1;
            for (int i = 0; i < n; i++) if (t[i] != s[i]){ same = 0; break; }
            if (same) return id;
        }
        h = (h + 1) & R.hmask;
    }
    return -1;
}

// ---------------------------------------------------------------------
//  loading
// ---------------------------------------------------------------------
static const uint8_t* tptr(const char* name, uint32_t* type, uint64_t* nelem){
    const GGUFTensor* t = ai_gguf_tensor(R.info, name);
    if (!t) return 0;
    uint64_t n = 1;
    for (uint32_t i = 0; i < t->n_dims && i < 4; i++) if (t->dims[i]) n *= t->dims[i];
    if (type)  *type  = t->type;
    if (nelem) *nelem = n;
    return R.tdata + t->offset;
}
static float* load_vec(const char* name, int n){
    uint32_t ty = 0; uint64_t ne = 0;
    const uint8_t* p = tptr(name, &ty, &ne);
    if (!p) return 0;
    if ((int)ne < n) n = (int)ne;
    float* v = (float*)apalloc((uint64_t)n * 4);
    if (!v) return 0;
    dequant_row(v, p, ty, n);
    return v;
}

void qwen_unload(void){
    if (g_arena){ big_free(g_arena, (uint32_t)g_arena_sz); g_arena = 0; }
    if (R.info){ big_free(R.info, (uint32_t)g_info_bytes); }
    g_arena_sz = g_arena_used = 0;
    gi_memset(&R, 0, sizeof(R));
    R.rng = 0x1234567u;
}

int qwen_load(const uint8_t* blob, uint64_t size, uint32_t max_ctx){
    qwen_unload();
    init_bytemap();
    g_err[0] = 0;

    g_info_bytes = sizeof(GGUFModelInfo);
    R.info = (GGUFModelInfo*)big_alloc((uint32_t)g_info_bytes);
    if (!R.info){ set_err("out of memory (model info)"); return -1; }

    int rc = ai_gguf_parse(blob, size, R.info);
    if (rc != 0){ set_err("not a valid GGUF file"); big_free(R.info, (uint32_t)g_info_bytes); R.info = 0; return -2; }

    R.blob = blob; R.blob_size = size;
    R.tdata = blob + R.info->tensor_data_offset;

    // ---- geometry (prefer tensor shapes over metadata: they never lie) ----
    uint64_t ne = 0;
    const GGUFTensor* te = ai_gguf_tensor(R.info, "token_embd.weight");
    if (!te){ set_err("token_embd.weight missing"); return -3; }
    R.n_embd  = (int)te->dims[0];
    R.n_vocab = (int)te->dims[1];
    if (R.info->embed_length && (int)R.info->embed_length != R.n_embd)
        R.n_embd = (int)R.info->embed_length;
    R.n_layer = (int)R.info->block_count;
    if (R.n_layer <= 0){
        char nm[64];
        for (int i = 0; i < 200; i++){
            tname(nm, i, "attn_norm.weight");
            if (!ai_gguf_tensor(R.info, nm)) break;
            R.n_layer = i + 1;
        }
    }
    R.n_head = (int)R.info->head_count;
    R.n_kv   = (int)R.info->head_count_kv;
    if (R.n_head <= 0) R.n_head = 1;
    if (R.n_kv   <= 0) R.n_kv   = R.n_head;

    char nm[64];
    tname(nm, 0, "attn_k.weight");
    const GGUFTensor* tk0 = ai_gguf_tensor(R.info, nm);
    R.head_dim = tk0 ? (int)(tk0->dims[1] / (uint64_t)R.n_kv) : (R.n_embd / R.n_head);
    if (R.head_dim <= 0) R.head_dim = R.n_embd / R.n_head;
    R.kv_dim = R.n_kv * R.head_dim;

    tname(nm, 0, "ffn_gate.weight");
    const GGUFTensor* tg0 = ai_gguf_tensor(R.info, nm);
    if (!tg0){ tname(nm, 0, "ffn_up.weight"); tg0 = ai_gguf_tensor(R.info, nm); }
    R.n_ff = tg0 ? (int)tg0->dims[1] : (int)R.info->feed_forward_length;
    if (R.n_ff <= 0){ set_err("cannot determine ffn width"); return -4; }

    R.n_ctx = (int)R.info->context_length;
    if (R.n_ctx <= 0) R.n_ctx = 512;
    if (max_ctx > 0 && (int)max_ctx < R.n_ctx) R.n_ctx = (int)max_ctx;
    if (R.n_ctx > 4096) R.n_ctx = 4096;      // KV cache guard

    R.rope_theta = R.info->rope_theta > 0 ? R.info->rope_theta : 10000.0f;
    R.eps        = R.info->rms_eps    > 0 ? R.info->rms_eps    : 1e-6f;
    R.rope_neox  = (gi_strcmp(R.info->arch, "llama") == 0) ? 0 : 1;

    tname(nm, 0, "attn_q_norm.weight");
    R.has_qk_norm = ai_gguf_tensor(R.info, nm) ? 1 : 0;

    // ---- arena sizing ----
    uint64_t need = 0;
    need += (uint64_t)R.n_layer * sizeof(LayerW);
    need += (uint64_t)R.n_layer * 2 * R.n_embd * 4;                  // norms
    need += (uint64_t)R.n_layer * (R.n_embd + 2 * R.kv_dim) * 4;     // biases
    if (R.has_qk_norm) need += (uint64_t)R.n_layer * 2 * R.head_dim * 4;
    need += (uint64_t)R.n_embd * 4;                                  // out_norm
    need += (uint64_t)(3 * R.n_embd + 2 * R.n_ff + 3 * R.n_head * R.head_dim) * 4;
    need += (uint64_t)R.n_head * R.n_ctx * 4;                        // att
    need += (uint64_t)R.n_vocab * 4;                                 // logits
    need += 2ull * R.n_layer * R.n_ctx * R.kv_dim * 4;               // kv cache
    need += (uint64_t)R.n_vocab * (4 + 2);                           // tok tables
    uint32_t hsz = 1024; while (hsz < (uint32_t)R.n_vocab * 2) hsz <<= 1;
    need += (uint64_t)hsz * 4;
    need += 8192;                                                    // map buffer
    need += 64ull * 128;                                             // slack

    if (need > 0xF0000000ull){ set_err("model too large for 4 GiB pool"); return -5; }
    g_arena_sz = need;
    g_arena = (uint8_t*)big_alloc((uint32_t)need);
    if (!g_arena){ set_err("out of memory (runtime arena)"); return -6; }
    g_arena_used = 0;

    R.L = (LayerW*)apalloc((uint64_t)R.n_layer * sizeof(LayerW));
    if (!R.L){ set_err("arena exhausted (layers)"); return -7; }

    // ---- bind per-layer tensors ----
    for (int l = 0; l < R.n_layer; l++){
        LayerW* w = &R.L[l];
        tname(nm, l, "attn_q.weight");      w->wq = tptr(nm, &w->tq, &ne);
        tname(nm, l, "attn_k.weight");      w->wk = tptr(nm, &w->tk, &ne);
        tname(nm, l, "attn_v.weight");      w->wv = tptr(nm, &w->tv, &ne);
        tname(nm, l, "attn_output.weight"); w->wo = tptr(nm, &w->to, &ne);
        tname(nm, l, "ffn_gate.weight");    w->wg = tptr(nm, &w->tg, &ne);
        tname(nm, l, "ffn_up.weight");      w->wu = tptr(nm, &w->tu, &ne);
        tname(nm, l, "ffn_down.weight");    w->wd = tptr(nm, &w->td, &ne);
        if (!w->wq || !w->wk || !w->wv || !w->wo || !w->wu || !w->wd){
            set_err("layer tensor missing"); return -8;
        }
        if (!type_supported(w->tq) || !type_supported(w->tk) ||
            !type_supported(w->tv) || !type_supported(w->to) ||
            !type_supported(w->tu) || !type_supported(w->td) ||
            (w->wg && !type_supported(w->tg))){
            set_err("unsupported quantisation in layer"); return -9;
        }
        tname(nm, l, "attn_norm.weight"); w->attn_norm = load_vec(nm, R.n_embd);
        tname(nm, l, "ffn_norm.weight");  w->ffn_norm  = load_vec(nm, R.n_embd);
        if (!w->attn_norm || !w->ffn_norm){ set_err("norm tensor missing"); return -10; }
        tname(nm, l, "attn_q.bias"); w->bq = load_vec(nm, R.n_head * R.head_dim);
        tname(nm, l, "attn_k.bias"); w->bk = load_vec(nm, R.kv_dim);
        tname(nm, l, "attn_v.bias"); w->bv = load_vec(nm, R.kv_dim);
        if (R.has_qk_norm){
            tname(nm, l, "attn_q_norm.weight"); w->qn = load_vec(nm, R.head_dim);
            tname(nm, l, "attn_k_norm.weight"); w->kn = load_vec(nm, R.head_dim);
        }
    }

    R.out_norm = load_vec("output_norm.weight", R.n_embd);
    if (!R.out_norm){ set_err("output_norm.weight missing"); return -11; }

    R.tok_w = tptr("token_embd.weight", &R.tok_t, &ne);
    R.tok_row_bytes = ai_gguf_type_size(R.tok_t, (uint64_t)R.n_embd);
    R.out_w = tptr("output.weight", &R.out_t, &ne);
    if (!R.out_w){ R.out_w = R.tok_w; R.out_t = R.tok_t; R.tie_embd = 1; }
    if (!type_supported(R.tok_t) || !type_supported(R.out_t)){
        set_err("unsupported quantisation in embeddings"); return -12;
    }

    // ---- activations ----
    R.x      = (float*)apalloc((uint64_t)R.n_embd * 4);
    int xbn  = (R.n_head * R.head_dim > R.n_embd) ? R.n_head * R.head_dim : R.n_embd;
    R.xb     = (float*)apalloc((uint64_t)xbn * 4);
    R.xb2    = (float*)apalloc((uint64_t)R.n_embd * 4);
    R.hb     = (float*)apalloc((uint64_t)R.n_ff * 4);
    R.hb2    = (float*)apalloc((uint64_t)R.n_ff * 4);
    R.q      = (float*)apalloc((uint64_t)R.n_head * R.head_dim * 4);
    R.att    = (float*)apalloc((uint64_t)R.n_head * R.n_ctx * 4);
    R.logits = (float*)apalloc((uint64_t)R.n_vocab * 4);
    R.kcache = (float*)apalloc((uint64_t)R.n_layer * R.n_ctx * R.kv_dim * 4);
    R.vcache = (float*)apalloc((uint64_t)R.n_layer * R.n_ctx * R.kv_dim * 4);
    if (!R.x || !R.xb || !R.xb2 || !R.hb || !R.hb2 || !R.q || !R.att ||
        !R.logits || !R.kcache || !R.vcache){
        set_err("arena exhausted (activations)"); return -13;
    }

    // ---- tokenizer index ----
    if (R.info->tokens_offset && R.n_vocab > 0){
        R.tok_off = (uint32_t*)apalloc((uint64_t)R.n_vocab * 4);
        R.tok_len = (uint16_t*)apalloc((uint64_t)R.n_vocab * 2);
        R.htab    = (int32_t*) apalloc((uint64_t)hsz * 4);
        R.mapbuf  = (uint8_t*) apalloc(8192);
        R.mapcap  = 8192;
        R.hmask   = hsz - 1;
        if (R.tok_off && R.tok_len && R.htab){
            uint64_t p = R.info->tokens_offset;
            for (int i = 0; i < R.n_vocab && p + 8 <= size; i++){
                uint64_t len = 0;
                for (int b = 0; b < 8; b++) len |= (uint64_t)blob[p + b] << (8 * b);
                p += 8;
                if (len > 0xFFFF || p + len > size){ R.tok_len[i] = 0; continue; }
                R.tok_off[i] = (uint32_t)p;
                R.tok_len[i] = (uint16_t)len;
                p += len;
                uint32_t h = fnv1a(blob + R.tok_off[i], (int)len) & R.hmask;
                while (R.htab[h]) h = (h + 1) & R.hmask;
                R.htab[h] = i + 1;
            }
        }
    }

    R.rt_bytes = g_arena_used + g_info_bytes;
    R.pos = 0;
    R.rng = 0x2545F491u;
    R.loaded = 1;

    slog("[GGUF] loaded arch="); slog(R.info->arch);
    slog(" layers=");  slog_u((uint64_t)R.n_layer);
    slog(" embd=");    slog_u((uint64_t)R.n_embd);
    slog(" heads=");   slog_u((uint64_t)R.n_head);
    slog("/");         slog_u((uint64_t)R.n_kv);
    slog(" hd=");      slog_u((uint64_t)R.head_dim);
    slog(" ff=");      slog_u((uint64_t)R.n_ff);
    slog(" vocab=");   slog_u((uint64_t)R.n_vocab);
    slog(" ctx=");     slog_u((uint64_t)R.n_ctx);
    slog(" rt=");      slog_u(R.rt_bytes >> 20); slog("MiB\n");
    return 0;
}

int  qwen_ready(void){ return R.loaded; }
const GGUFModelInfo* qwen_info(void){ return R.loaded ? R.info : 0; }
const char* qwen_error(void){ return g_err; }
uint64_t qwen_runtime_bytes(void){ return R.loaded ? R.rt_bytes : 0; }
void qwen_seed(uint32_t s){ R.rng = s ? s : 0x2545F491u; }
void qwen_reset(void){ R.pos = 0; }

// ---------------------------------------------------------------------
//  tokenizer (byte-level BPE vocabulary, greedy longest match)
// ---------------------------------------------------------------------
int qwen_tokenize(const char* text, int32_t* out, int max_out){
    if (!R.loaded || !R.htab || !text || !out || max_out <= 0) return 0;
    init_bytemap();
    int n = gi_strlen(text);
    uint32_t mlen = 0;
    for (int i = 0; i < n && mlen + 2 < R.mapcap; i++)
        mlen += (uint32_t)cp_to_utf8(g_b2u[(uint8_t)text[i]], R.mapbuf + mlen);

    int cnt = 0; uint32_t i = 0;
    while (i < mlen && cnt < max_out){
        int best = -1; uint32_t bestlen = 0;
        uint32_t maxtry = mlen - i; if (maxtry > 48) maxtry = 48;
        for (uint32_t l = maxtry; l >= 1; l--){
            // never split a UTF-8 sequence
            if (i + l < mlen && (R.mapbuf[i + l] & 0xC0) == 0x80) continue;
            int id = tok_lookup(R.mapbuf + i, (int)l);
            if (id >= 0){ best = id; bestlen = l; break; }
        }
        if (best < 0){
            uint32_t cp; int adv = utf8_to_cp(R.mapbuf + i, (int)(mlen - i), &cp);
            i += (uint32_t)(adv > 0 ? adv : 1);
            continue;
        }
        out[cnt++] = best;
        i += bestlen;
    }
    return cnt;
}

static char g_tokbuf[512];
const char* qwen_detokenize(int32_t id){
    g_tokbuf[0] = 0;
    if (!R.loaded || !R.tok_len || id < 0 || id >= R.n_vocab) return g_tokbuf;
    init_bytemap();
    int len = (int)R.tok_len[id];
    if (len <= 0) return g_tokbuf;
    const uint8_t* s = R.blob + R.tok_off[id];
    int o = 0, i = 0;
    while (i < len && o < (int)sizeof(g_tokbuf) - 4){
        uint32_t cp = 0;
        int adv = utf8_to_cp(s + i, len - i, &cp);
        if (adv <= 0) break;
        if (cp < 512 && g_u2b[cp] >= 0){
            g_tokbuf[o++] = (char)(uint8_t)g_u2b[cp];
        } else {
            for (int k = 0; k < adv; k++) g_tokbuf[o++] = (char)s[i + k];
        }
        i += adv;
    }
    g_tokbuf[o] = 0;
    return g_tokbuf;
}

// ---------------------------------------------------------------------
//  transformer forward
// ---------------------------------------------------------------------
static void rmsnorm(float* o, const float* x, const float* w, int n, float eps){
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (float)n + eps;
    float inv = 1.0f / mi_sqrtf(ss);
    for (int i = 0; i < n; i++) o[i] = w[i] * (x[i] * inv);
}
static void softmax_n(float* v, int n){
    if (n <= 0) return;
    float m = v[0];
    for (int i = 1; i < n; i++) if (v[i] > m) m = v[i];
    float s = 0.0f;
    for (int i = 0; i < n; i++){ v[i] = mi_expf(v[i] - m); s += v[i]; }
    if (s <= 0.0f) s = 1.0f;
    float inv = 1.0f / s;
    for (int i = 0; i < n; i++) v[i] *= inv;
}
static float mi_logf(float x){
    if (x <= 0.0f) return -1.0e30f;
    union { float f; uint32_t i; } u; u.f = x;
    int e = (int)((u.i >> 23) & 0xFFu) - 127;
    u.i = (u.i & 0x807FFFFFu) | 0x3F800000u;
    float m = u.f;
    float t = (m - 1.0f) / (m + 1.0f);
    float t2 = t * t;
    float s = t * (1.0f + t2*(0.33333333f + t2*(0.2f + t2*(0.14285714f + t2*0.11111111f))));
    return 2.0f * s + (float)e * 0.69314718f;
}

// RoPE.  qwen2/qwen3 GGUFs are *not* permuted by the converter, so they use
// the NEOX pairing (i, i+hd/2); llama GGUFs are permuted and use (2i, 2i+1).
static void rope_apply(float* v, int nheads, int hd, int pos, float theta, int neox){
    int half = hd / 2;
    float lt = mi_logf(theta);
    for (int i = 0; i < half; i++){
        float freq = mi_expf(-((float)(2 * i) / (float)hd) * lt);
        float s, c;
        mi_sincos((double)pos * (double)freq, &s, &c);
        for (int h = 0; h < nheads; h++){
            float* p = v + h * hd;
            int a = neox ? i        : 2 * i;
            int b = neox ? i + half : 2 * i + 1;
            float x0 = p[a], x1 = p[b];
            p[a] = x0 * c - x1 * s;
            p[b] = x0 * s + x1 * c;
        }
    }
}

const float* qwen_forward(int32_t token, int32_t pos){
    if (!R.loaded) return 0;
    if (pos < 0) pos = 0;
    if (pos >= R.n_ctx) pos = R.n_ctx - 1;
    const int E = R.n_embd, HD = R.head_dim, KVD = R.kv_dim, H = R.n_head;
    if (token < 0 || token >= R.n_vocab) token = 0;

    dequant_row(R.x, R.tok_w + (uint64_t)token * R.tok_row_bytes, R.tok_t, E);

    const float scale = 1.0f / mi_sqrtf((float)HD);
    int rep = H / R.n_kv; if (rep < 1) rep = 1;

    for (int l = 0; l < R.n_layer; l++){
        LayerW* w = &R.L[l];
        rmsnorm(R.xb, R.x, w->attn_norm, E, R.eps);

        float* krow = R.kcache + ((uint64_t)l * R.n_ctx + pos) * KVD;
        float* vrow = R.vcache + ((uint64_t)l * R.n_ctx + pos) * KVD;
        matmul(R.q,  R.xb, w->wq, w->tq, E, H * HD);
        matmul(krow, R.xb, w->wk, w->tk, E, KVD);
        matmul(vrow, R.xb, w->wv, w->tv, E, KVD);
        if (w->bq) for (int i = 0; i < H * HD; i++) R.q[i]  += w->bq[i];
        if (w->bk) for (int i = 0; i < KVD;    i++) krow[i] += w->bk[i];
        if (w->bv) for (int i = 0; i < KVD;    i++) vrow[i] += w->bv[i];

        if (R.has_qk_norm && w->qn && w->kn){
            for (int h = 0; h < H;      h++) rmsnorm(R.q + h*HD,  R.q + h*HD,  w->qn, HD, R.eps);
            for (int h = 0; h < R.n_kv; h++) rmsnorm(krow + h*HD, krow + h*HD, w->kn, HD, R.eps);
        }
        rope_apply(R.q,  H,      HD, pos, R.rope_theta, R.rope_neox);
        rope_apply(krow, R.n_kv, HD, pos, R.rope_theta, R.rope_neox);

        for (int h = 0; h < H; h++){
            const float* qh = R.q + h * HD;
            float* a = R.att + (uint64_t)h * R.n_ctx;
            int kvh = h / rep;
            for (int t = 0; t <= pos; t++){
                const float* kk = R.kcache + ((uint64_t)l * R.n_ctx + t) * KVD + kvh * HD;
                float s = 0.0f;
                for (int i = 0; i < HD; i++) s += qh[i] * kk[i];
                a[t] = s * scale;
            }
            softmax_n(a, pos + 1);
            float* ob = R.xb + h * HD;
            for (int i = 0; i < HD; i++) ob[i] = 0.0f;
            for (int t = 0; t <= pos; t++){
                const float* vv = R.vcache + ((uint64_t)l * R.n_ctx + t) * KVD + kvh * HD;
                float aw = a[t];
                if (aw == 0.0f) continue;
                for (int i = 0; i < HD; i++) ob[i] += aw * vv[i];
            }
        }
        matmul(R.xb2, R.xb, w->wo, w->to, H * HD, E);
        for (int i = 0; i < E; i++) R.x[i] += R.xb2[i];

        rmsnorm(R.xb, R.x, w->ffn_norm, E, R.eps);
        matmul(R.hb, R.xb, w->wu, w->tu, E, R.n_ff);
        if (w->wg){
            matmul(R.hb2, R.xb, w->wg, w->tg, E, R.n_ff);
            for (int i = 0; i < R.n_ff; i++){
                float g = R.hb2[i];
                g = g * (1.0f / (1.0f + mi_expf(-g)));      // SiLU(gate)
                R.hb[i] = g * R.hb[i];                      // * up
            }
        } else {
            for (int i = 0; i < R.n_ff; i++){
                float g = R.hb[i];
                R.hb[i] = g * (1.0f / (1.0f + mi_expf(-g)));
            }
        }
        matmul(R.xb2, R.hb, w->wd, w->td, R.n_ff, E);
        for (int i = 0; i < E; i++) R.x[i] += R.xb2[i];
    }

    rmsnorm(R.x, R.x, R.out_norm, E, R.eps);
    matmul(R.logits, R.x, R.out_w, R.out_t, E, R.n_vocab);
    R.pos = pos + 1;
    return R.logits;
}

// ---------------------------------------------------------------------
//  sampling
// ---------------------------------------------------------------------
static uint32_t rnd32(void){
    R.rng ^= R.rng << 13; R.rng ^= R.rng >> 17; R.rng ^= R.rng << 5;
    return R.rng;
}

int32_t qwen_sample(const float* logits, float temp, float top_p){
    if (!R.loaded || !logits) return 0;
    const int n = R.n_vocab;
    if (temp <= 0.0f){
        int best = 0; float bv = logits[0];
        for (int i = 1; i < n; i++) if (logits[i] > bv){ bv = logits[i]; best = i; }
        return best;
    }
    // partial top-k selection (k = 40) keeps this O(n) with a tiny constant
    const int K = 40;
    int   idx[40]; float val[40];
    int cnt = 0;
    for (int i = 0; i < n; i++){
        float v = logits[i];
        if (cnt < K){
            int j = cnt++;
            while (j > 0 && val[j-1] < v){ val[j] = val[j-1]; idx[j] = idx[j-1]; j--; }
            val[j] = v; idx[j] = i;
        } else if (v > val[K-1]){
            int j = K - 1;
            while (j > 0 && val[j-1] < v){ val[j] = val[j-1]; idx[j] = idx[j-1]; j--; }
            val[j] = v; idx[j] = i;
        }
    }
    if (cnt == 0) return 0;
    float p[40];
    float m = val[0], s = 0.0f;
    for (int i = 0; i < cnt; i++){ p[i] = mi_expf((val[i] - m) / temp); s += p[i]; }
    if (s <= 0.0f) return idx[0];
    for (int i = 0; i < cnt; i++) p[i] /= s;

    float lim = (top_p > 0.0f && top_p < 1.0f) ? top_p : 1.0f;
    float acc = 0.0f; int last = cnt - 1;
    for (int i = 0; i < cnt; i++){ acc += p[i]; if (acc >= lim){ last = i; break; } }
    float r = (float)(rnd32() >> 8) / 16777216.0f * acc;
    float c = 0.0f;
    for (int i = 0; i <= last; i++){ c += p[i]; if (r <= c) return idx[i]; }
    return idx[last];
}

// ---------------------------------------------------------------------
//  end-to-end generation
// ---------------------------------------------------------------------
static int32_t g_ptoks[1024];

int qwen_generate(const char* prompt, int max_new, float temp, QwenEmitFn emit){
    if (!R.loaded) return -1;
    qwen_reset();
    int n = qwen_tokenize(prompt, g_ptoks, 1024);
    if (n <= 0){
        g_ptoks[0] = (R.info->bos_id != 0xFFFFFFFFu) ? (int32_t)R.info->bos_id : 0;
        n = 1;
    }
    if (n > R.n_ctx - 1) n = R.n_ctx - 1;

    const float* lg = 0;
    int pos = 0;
    for (int i = 0; i < n; i++) lg = qwen_forward(g_ptoks[i], pos++);

    int produced = 0;
    while (produced < max_new && pos < R.n_ctx && lg){
        int32_t next = qwen_sample(lg, temp, 0.9f);
        if (R.info->eos_id != 0xFFFFFFFFu && next == (int32_t)R.info->eos_id) break;
        const char* piece = qwen_detokenize(next);
        if (emit && piece[0]) emit(piece);
        produced++;
        lg = qwen_forward(next, pos++);
    }
    return produced;
}
