/* tls_crypto.cpp -- self-contained crypto primitives for the in-kernel TLS 1.3
 * client.  See tls_crypto.h for the scope note.
 *
 * Freestanding rules obeyed here:
 *   * no libc / libm / STL; only <stdint.h> and our own helpers
 *   * no 64x64 multiplication (avoids libgcc __muldi3 / __udivdi3 under -m32)
 *   * no floating point
 */

#include "tls_crypto.h"

// ---------------------------------------------------------------------
//  tiny helpers
// ---------------------------------------------------------------------
static void zero_mem(void* p, int n){
    uint8_t* q = (uint8_t*)p;
    for (int i = 0; i < n; i++) q[i] = 0;
}
static void copy_mem(void* d, const void* s, int n){
    uint8_t* dp = (uint8_t*)d; const uint8_t* sp = (const uint8_t*)s;
    for (int i = 0; i < n; i++) dp[i] = sp[i];
}
static void store_be32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t load_be32(const uint8_t* p){
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
static void store_le32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t load_le32(const uint8_t* p){
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t rotl32(uint32_t v, int n){ return (v << n) | (v >> (32 - n)); }
static uint32_t rotr32(uint32_t v, int n){ return (v >> n) | (v << (32 - n)); }

/* Left-rotate a 64-bit (hi,lo) pair -- written without 64x64 or 64-bit shifts
 * that would need libgcc on some -m32 targets. */
static void rotl64(uint32_t* hi, uint32_t* lo, int n){
    uint32_t h = *hi, l = *lo, rh, rl;
    if (n == 0) return;
    if (n < 32){
        rh = (h << n) | (l >> (32 - n));
        rl = (l << n) | (h >> (32 - n));
    } else if (n == 32){
        rh = l; rl = h;
    } else {
        int m = n - 32;
        rh = (l << m) | (h >> (32 - m));
        rl = (h << m) | (l >> (32 - m));
    }
    *hi = rh; *lo = rl;
}

// ---------------------------------------------------------------------
//  SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------
static const uint32_t SHA256_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
    0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
    0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
    0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
    0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
    0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_block(uint32_t h[8], const uint8_t blk[64]){
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = load_be32(blk + 4 * i);
    for (int i = 16; i < 64; i++){
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++){
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_init(sha256_ctx* c){
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0; c->buflen = 0;
}

void sha256_update(sha256_ctx* c, const void* data, int len){
    const uint8_t* p = (const uint8_t*)data;
    if (len <= 0) return;
    c->len += (uint64_t)len;
    if (c->buflen){
        int need = 64 - c->buflen;
        int take = (len < need) ? len : need;
        copy_mem(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; len -= take;
        if (c->buflen == 64){ sha256_block(c->h, c->buf); c->buflen = 0; }
    }
    while (len >= 64){ sha256_block(c->h, p); p += 64; len -= 64; }
    if (len > 0){ copy_mem(c->buf, p, len); c->buflen = len; }
}

void sha256_final(sha256_ctx* c, uint8_t out[32]){
    /* Append the 0x80 pad + 64-bit length, then finalize.  The length is
       written from the byte counter BEFORE the padding bytes are absorbed. */
    uint64_t total = c->len;
    uint8_t pad[72];
    int padlen = 0;
    pad[padlen++] = 0x80;
    while (((c->buflen + padlen) & 63) != 56){
        pad[padlen++] = 0x00;
        if (padlen >= 64) break;
    }
    /* 64-bit big-endian BIT length, built from two 32-bit halves.
       SHA-256 absorbs the message length in BITS, not bytes -- writing the
       byte count directly is the classic padding bug (every non-empty message
       hashes wrong while the empty message still matches).
       total*8 is done as (hi,lo) << 3 so no 64-bit shift / libgcc helper is
       needed under -m32. */
    uint8_t lenbe[8];
    uint32_t hi = (uint32_t)(total >> 32);
    uint32_t lo = (uint32_t)total;
    uint32_t bhi = (hi << 3) | (lo >> 29);
    uint32_t blo = lo << 3;
    store_be32(lenbe + 0, bhi);
    store_be32(lenbe + 4, blo);
    sha256_update(c, pad, padlen);
    sha256_update(c, lenbe, 8);
    for (int i = 0; i < 8; i++) store_be32(out + 4 * i, c->h[i]);
}

void sha256(const void* data, int len, uint8_t out[32]){
    sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

// ---------------------------------------------------------------------
//  HMAC-SHA256 (RFC 2104)
// ---------------------------------------------------------------------
#define HMAC_BLOCK 64

void hmac_sha256_init(hmac_sha256_ctx* c, const uint8_t* key, int keylen){
    uint8_t k[HMAC_BLOCK];
    zero_mem(k, HMAC_BLOCK);
    if (keylen > HMAC_BLOCK){
        uint8_t hk[32]; sha256(key, keylen, hk);
        copy_mem(k, hk, 32);
    } else if (keylen > 0){
        copy_mem(k, key, keylen);
    }
    uint8_t ipad[HMAC_BLOCK], opad[HMAC_BLOCK];
    for (int i = 0; i < HMAC_BLOCK; i++){
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    sha256_init(&c->inner); sha256_update(&c->inner, ipad, HMAC_BLOCK);
    sha256_init(&c->outer); sha256_update(&c->outer, opad, HMAC_BLOCK);
    zero_mem(k, HMAC_BLOCK); zero_mem(ipad, HMAC_BLOCK); zero_mem(opad, HMAC_BLOCK);
}

void hmac_sha256_update(hmac_sha256_ctx* c, const void* data, int len){
    sha256_update(&c->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_ctx* c, uint8_t out[32]){
    uint8_t ih[32];
    sha256_final(&c->inner, ih);
    sha256_update(&c->outer, ih, 32);
    sha256_final(&c->outer, out);
}

void hmac_sha256(const uint8_t* key, int keylen, const void* data, int len,
                 uint8_t out[32]){
    hmac_sha256_ctx c;
    hmac_sha256_init(&c, key, keylen);
    hmac_sha256_update(&c, data, len);
    hmac_sha256_final(&c, out);
}

// ---------------------------------------------------------------------
//  HKDF (RFC 5869)
// ---------------------------------------------------------------------
void hkdf_extract(const uint8_t* salt, int saltlen,
                  const uint8_t* ikm, int ikmlen, uint8_t prk[32]){
    if (!salt || saltlen <= 0){
        uint8_t z[32]; zero_mem(z, 32);
        hmac_sha256(z, 32, ikm, ikmlen, prk);
    } else {
        hmac_sha256(salt, saltlen, ikm, ikmlen, prk);
    }
}

int hkdf_expand(const uint8_t prk[32], const uint8_t* info, int infolen,
                uint8_t* out, int outlen){
    if (!out || outlen < 0) return -1;
    if (outlen == 0) return 0;
    int nblocks = (outlen + 31) / 32;
    if (nblocks > 255) return -1;
    uint8_t prev[32]; zero_mem(prev, 32);
    int done = 0;
    for (int i = 1; i <= nblocks; i++){
        hmac_sha256_ctx c;
        hmac_sha256_init(&c, prk, 32);
        if (i > 1) hmac_sha256_update(&c, prev, 32);
        if (info && infolen > 0) hmac_sha256_update(&c, info, infolen);
        uint8_t ctr = (uint8_t)i;
        hmac_sha256_update(&c, &ctr, 1);
        hmac_sha256_final(&c, prev);
        int take = outlen - done;
        if (take > 32) take = 32;
        copy_mem(out + done, prev, take);
        done += take;
    }
    zero_mem(prev, 32);
    return 0;
}


// ---------------------------------------------------------------------
//  ChaCha20 (RFC 8439 2.3) -- pure 32-bit arithmetic, no table lookups,
//  no secret-dependent branches (all ops are add/xor/rotate).
// ---------------------------------------------------------------------
#define CC_QR(a,b,c,d) \
    do { a += b; d ^= a; d = rotl32(d,16); \
         c += d; b ^= c; b = rotl32(b,12); \
         a += b; d ^= a; d = rotl32(d,8);  \
         c += d; b ^= c; b = rotl32(b,7);  } while (0)

void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]){
    uint32_t s[16];
    s[0] = 0x61707865; s[1] = 0x3320646e;   /* "expand 32-byte k" */
    s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = load_le32(key + 4 * i);
    s[12] = counter;
    s[13] = load_le32(nonce + 0);
    s[14] = load_le32(nonce + 4);
    s[15] = load_le32(nonce + 8);

    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = s[i];

    for (int i = 0; i < 10; i++){          /* 20 rounds = 10 double-rounds */
        CC_QR(w[0], w[4], w[8],  w[12]);
        CC_QR(w[1], w[5], w[9],  w[13]);
        CC_QR(w[2], w[6], w[10], w[14]);
        CC_QR(w[3], w[7], w[11], w[15]);
        CC_QR(w[0], w[5], w[10], w[15]);
        CC_QR(w[1], w[6], w[11], w[12]);
        CC_QR(w[2], w[7], w[8],  w[13]);
        CC_QR(w[3], w[4], w[9],  w[14]);
    }
    for (int i = 0; i < 16; i++) store_le32(out + 4 * i, w[i] + s[i]);
}
#undef CC_QR

void chacha20_xor(const uint8_t key[32], uint32_t counter,
                  const uint8_t nonce[12], const uint8_t* in, uint8_t* out,
                  int len){
    uint8_t ks[64];
    int done = 0;
    while (done < len){
        chacha20_block(key, counter++, nonce, ks);
        int take = len - done;
        if (take > 64) take = 64;
        for (int i = 0; i < take; i++)
            out[done + i] = (uint8_t)(in[done + i] ^ ks[i]);
        done += take;
    }
    zero_mem(ks, 64);
}

// ---------------------------------------------------------------------
//  Poly1305 (RFC 8439 2.5)
//  26-bit limb form (poly1305-donna-32 style).  Every multiply is
//  32x32 -> 64 (a single 'mul'), so -m32 needs no libgcc helper.
// ---------------------------------------------------------------------
typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
} poly1305_ctx;

static void poly1305_init(poly1305_ctx* st, const uint8_t key[32]){
    /* Clamp BEFORE splitting into limbs.  Doing it per-byte while building the
       limbs is easy to get wrong (the clamp clears bits 26,27 / 54,55 / ... of
       r, which straddle limb boundaries); the reference (poly1305-donna-32)
       masks the 32-bit words first and splits afterwards. */
    uint32_t t0 = load_le32(key + 0);
    uint32_t t1 = load_le32(key + 4);
    uint32_t t2 = load_le32(key + 8);
    uint32_t t3 = load_le32(key + 12);
    t0 &= 0x0fffffff;
    t1 &= 0x0ffffffc;
    t2 &= 0x0ffffffc;
    t3 &= 0x0ffffffc;

    st->r[0] = (t0)                            & 0x3ffffff;
    st->r[1] = ((t0 >> 26) | (t1 << 6))        & 0x3ffffff;
    st->r[2] = ((t1 >> 20) | (t2 << 12))       & 0x3ffffff;
    st->r[3] = ((t2 >> 14) | (t3 << 18))       & 0x3ffffff;
    st->r[4] = ((t3 >> 8))                     & 0x3ffffff;

    st->h[0] = st->h[1] = st->h[2] = st->h[3] = st->h[4] = 0;
    for (int i = 0; i < 4; i++) st->pad[i] = load_le32(key + 16 + 4 * i);
}

/* Absorb one 16-byte block.  'hibit' is 1<<24 for full blocks (the implicit
 * bit 128) and 0 for the padded final block (which carries its 1 explicitly). */
static void poly1305_block(poly1305_ctx* st, const uint8_t m[16], uint32_t hibit){
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;

    h0 += (uint32_t)(m[0]) | ((uint32_t)m[1] << 8) | ((uint32_t)m[2] << 16) | ((uint32_t)(m[3] & 0x03) << 24);
    h1 += ((uint32_t)(m[3] >> 2)) | ((uint32_t)m[4] << 6) | ((uint32_t)m[5] << 14) | ((uint32_t)(m[6] & 0x0f) << 22);
    h2 += ((uint32_t)(m[6] >> 4)) | ((uint32_t)m[7] << 4) | ((uint32_t)m[8] << 12) | ((uint32_t)(m[9] & 0x3f) << 20);
    h3 += ((uint32_t)(m[9] >> 6)) | ((uint32_t)m[10] << 2) | ((uint32_t)m[11] << 10) | ((uint32_t)m[12] << 18);
    h4 += ((uint32_t)m[13]) | ((uint32_t)m[14] << 8) | ((uint32_t)m[15] << 16) | hibit;

    d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly1305_finish(poly1305_ctx* st, uint8_t mac[16]){
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    uint32_t g0, g1, g2, g3, g4, c, mask;
    uint64_t f;

    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);

    mask = (g4 >> 31) - 1;             /* all ones when g4 >= 0 (h < p) */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    h0 = (h0      ) | (h1 << 26);
    h1 = (h1 >>  6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 <<  8);

    f = (uint64_t)h0 + st->pad[0];              h0 = (uint32_t)f; f >>= 32;
    f = (uint64_t)h1 + st->pad[1] + f;          h1 = (uint32_t)f; f >>= 32;
    f = (uint64_t)h2 + st->pad[2] + f;          h2 = (uint32_t)f; f >>= 32;
    f = (uint64_t)h3 + st->pad[3] + f;          h3 = (uint32_t)f;

    store_le32(mac + 0, h0);
    store_le32(mac + 4, h1);
    store_le32(mac + 8, h2);
    store_le32(mac + 12, h3);
}

void poly1305(const uint8_t key[32], const uint8_t* msg, int len, uint8_t out[16]){
    poly1305_ctx st;
    poly1305_init(&st, key);
    int i = 0;
    for (; i + 16 <= len; i += 16) poly1305_block(&st, msg + i, 1u << 24);
    int rem = len - i;
    if (rem > 0){
        uint8_t tail[16];
        zero_mem(tail,16);
        copy_mem(tail, msg + i, rem);
        tail[rem] = 0x01;                 /* explicit bit 128 */
        poly1305_block(&st, tail, 0);
    }
    poly1305_finish(&st, out);
}

// ---------------------------------------------------------------------
//  AEAD_CHACHA20_POLY1305 (RFC 8439 2.8)
//
//  MAC input (in order):
//    AAD || pad16(AAD) || ciphertext || pad16(ct) || len(AAD) || len(ct)
//  (both lengths as 64-bit little-endian)
// ---------------------------------------------------------------------
/* Poly1305 over the AEAD construction, computed incrementally via the
 * one-shot poly1305() by assembling the buffer.  Buffers are caller-provided
 * (tls_client owns them) so no large stack object is needed. */

/* Absorb 'len' bytes as a sequence of FULL 16-byte blocks, zero-padding the
 * final partial block.  This is the AEAD (RFC 8439 2.8) section rule: pad16()
 * turns a section into whole blocks, so every block carries the IMPLICIT
 * 2^128 bit (hibit = 1<<24 in the 26-bit limb form).
 *
 * NOTE the difference from poly1305() (bare MAC, RFC 8439 2.5.1): there the
 * FINAL partial block gets an EXPLICIT 1 bit at position 8*len.  Applying that
 * rule to each AEAD section is wrong -- it only coincides with the implicit
 * bit when len % 16 == 3, which is exactly the kind of bug that passes small
 * test cases and fails real traffic. */
static void poly1305_absorb_padded(poly1305_ctx* st, const uint8_t* data, int len){
    int i = 0;
    for (; i + 16 <= len; i += 16) poly1305_block(st, data + i, 1u << 24);
    int rem = len - i;
    if (rem > 0){
        uint8_t tail[16];
        zero_mem(tail, 16);
        copy_mem(tail, data + i, rem);      /* remaining bytes are zero */
        poly1305_block(st, tail, 1u << 24); /* full block -> implicit bit */
    }
}

int aead_chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, int aadlen,
                               const uint8_t* plain, int len, uint8_t* out){
    if (!key || !nonce || !out) return -1;
    if (len < 0 || aadlen < 0) return -1;

    /* 1. ciphertext = ChaCha20(key, counter=1, nonce, plain) */
    uint8_t poly_key[64];
    chacha20_block(key, 0, nonce, poly_key);      /* counter 0 -> Poly1305 key */
    if (len > 0) chacha20_xor(key, 1, nonce, plain, out, len);

    /* 2. tag = Poly1305(poly_key, aad||pad||ct||pad||len(aad)||len(ct)) */
    poly1305_ctx st;
    poly1305_init(&st, poly_key);

    poly1305_absorb_padded(&st, aad, aadlen);   /* pad16(AAD) */
    poly1305_absorb_padded(&st, out, len);      /* pad16(ciphertext) */
    {
        uint8_t lens[16];
        uint32_t l;
        l = (uint32_t)aadlen; store_le32(lens + 0, l); store_le32(lens + 4, 0);
        l = (uint32_t)len;    store_le32(lens + 8, l); store_le32(lens + 12, 0);
        poly1305_block(&st, lens, 1u << 24);
    }
    poly1305_finish(&st, out + len);

    zero_mem(poly_key, 64);
    return 0;
}

int aead_chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                               const uint8_t* aad, int aadlen,
                               const uint8_t* sealed, int len, uint8_t* out){
    if (!key || !nonce || !sealed || !out) return -1;
    if (len < 16) return -1;

    int ctlen = len - 16;
    uint8_t poly_key[64];
    chacha20_block(key, 0, nonce, poly_key);      /* counter 0 -> Poly1305 key */

    poly1305_ctx st;
    poly1305_init(&st, poly_key);

    poly1305_absorb_padded(&st, aad, aadlen);        /* pad16(AAD) */
    poly1305_absorb_padded(&st, sealed, ctlen);      /* pad16(ciphertext) */
    {
        uint8_t lens[16];
        store_le32(lens + 0, (uint32_t)aadlen); store_le32(lens + 4, 0);
        store_le32(lens + 8, (uint32_t)ctlen);  store_le32(lens + 12, 0);
        poly1305_block(&st, lens, 1u << 24);
    }
    uint8_t tag[16];
    poly1305_finish(&st, tag);

    int ok = ct_memcmp(tag, sealed + ctlen, 16);
    if (!ok){ zero_mem(poly_key, 64); return -1; }

    if (ctlen > 0) chacha20_xor(key, 1, nonce, sealed, out, ctlen);
    zero_mem(poly_key, 64);
    return 0;
}


// ---------------------------------------------------------------------
//  constant-time compare
// ---------------------------------------------------------------------
int ct_memcmp(const void* a, const void* b, int len){
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    uint8_t d = 0;
    for (int i = 0; i < len; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------
//  X25519 (RFC 7748)
//
//  Field arithmetic over GF(2^255-19) in radix 2^16: sixteen 16-bit limbs
//  held in int64_t so intermediate values can go negative (fe_sub) and so a
//  256-bit product accumulates without any overflow.
//
//  WHY SO PLAIN: an earlier version used the compact 10-limb 26/25-bit
//  "ref10" layout.  It was faster and completely wrong -- the pre-multipliers
//  (f1_2, g9_19, ...) encode radix-specific wraparound that only holds for
//  reduced limbs, and fe_add/fe_sub legitimately produce out-of-range and
//  negative limbs.  X25519 runs twice per TLS handshake, so correctness is
//  worth 2.5x the multiply cost.
//
//  Reduction: 2^256 == 38 (mod 2^255-19), so the high half of a 512-bit
//  product folds onto the low half with factor 38.  The top limb keeps 15
//  bits (bit 255) and folds with factor 19 (2^255 == 19).
// ---------------------------------------------------------------------
typedef struct { int64_t v[16]; } fe25519;

static void fe_zero(fe25519* h){ for (int i = 0; i < 16; i++) h->v[i] = 0; }
static void fe_one(fe25519* h){ fe_zero(h); h->v[0] = 1; }
static void fe_copy(fe25519* h, const fe25519* f){
    for (int i = 0; i < 16; i++) h->v[i] = f->v[i];
}
static void fe_add(fe25519* h, const fe25519* f, const fe25519* g){
    for (int i = 0; i < 16; i++) h->v[i] = f->v[i] + g->v[i];
}
static void fe_sub(fe25519* h, const fe25519* f, const fe25519* g){
    for (int i = 0; i < 16; i++) h->v[i] = f->v[i] - g->v[i];
}

/* Normalise to canonical limbs.  Runs a few passes because a borrow from a
 * negative limb can push the next one out of range again, and the top-limb
 * fold feeds back into limb 0. */
static void fe_carry(fe25519* h){
    for (int pass = 0; pass < 5; pass++){
        int64_t c;
        for (int i = 0; i < 15; i++){
            c = h->v[i] >> 16;              /* arithmetic shift: floor div */
            h->v[i] -= c << 16;             /* now in [0, 2^16)            */
            h->v[i + 1] += c;
        }
        c = h->v[15] >> 15;                 /* bit 255 and above           */
        h->v[15] -= c << 15;                /* keep bits 240..254          */
        h->v[0] += c * 19;                  /* 2^255 == 19 (mod p)         */
        c = h->v[0] >> 16;
        h->v[0] -= c << 16;
        h->v[1] += c;
        int stable = (c == 0);
        if (stable){
            for (int i = 0; i < 15; i++)
                if (h->v[i] < 0 || h->v[i] > 0xffff) { stable = 0; break; }
            if (h->v[15] < 0 || h->v[15] > 0x7fff) stable = 0;
        }
        if (stable) break;
    }
}

static void fe_mul(fe25519* h, const fe25519* f, const fe25519* g){
    fe25519 fa, ga;
    fe_copy(&fa, f); fe_carry(&fa);
    fe_copy(&ga, g); fe_carry(&ga);

    int64_t p[32];
    for (int i = 0; i < 32; i++) p[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            p[i + j] += fa.v[i] * ga.v[j];

    /* 2^256 == 38: fold the high 16 limbs down */
    for (int i = 0; i < 16; i++) p[i] += 38 * p[i + 16];

    fe25519 r;
    for (int i = 0; i < 16; i++) r.v[i] = p[i];
    fe_carry(&r);
    fe_copy(h, &r);
}

static void fe_sq(fe25519* h, const fe25519* f){ fe_mul(h, f, f); }

/* h = z^(p-2) mod p by square-and-multiply.  Same exponent as before; the
 * explicit loop keeps the exponent visible instead of hiding it in a
 * hand-derived addition chain. */
static void fe_invert(fe25519* out, const fe25519* z){
    uint8_t e[32];
    for (int i = 0; i < 32; i++) e[i] = 0xff;
    e[0] = 0xeb;        /* p-2 = 2^255 - 21 */
    e[31] = 0x7f;

    fe25519 r, b, t;
    fe_one(&r);
    fe_copy(&b, z);
    for (int bit = 0; bit < 255; bit++){
        if ((e[bit >> 3] >> (bit & 7)) & 1){
            fe_mul(&t, &r, &b);
            fe_copy(&r, &t);
        }
        fe_sq(&t, &b);
        fe_copy(&b, &t);
    }
    fe_copy(out, &r);
}

/* 32 little-endian bytes -> limbs.  Bit 255 is ignored (RFC 7748 section 5). */
static void fe_frombytes(fe25519* h, const uint8_t* s){
    for (int i = 0; i < 16; i++)
        h->v[i] = (int64_t)s[2 * i] | ((int64_t)s[2 * i + 1] << 8);
    h->v[15] &= 0x7fff;
}

/* limbs -> 32 little-endian bytes, fully reduced mod p. */
static void fe_tobytes(uint8_t* s, const fe25519* h){
    fe25519 t;
    fe_copy(&t, h);
    fe_carry(&t);

    /* Canonicalise: compute t + 19.  If bit 255 sets, then t >= p and the
     * low 255 bits of (t + 19) are the correct residue. */
    int64_t c = 19;
    for (int i = 0; i < 16; i++){
        int64_t x = t.v[i] + c;
        t.v[i] = x & 0xffff;
        c = x >> 16;
    }
    if (t.v[15] & 0x8000) t.v[15] &= 0x7fff;
    else {
        /* t + 19 did not overflow: the original t was already < p, so undo. */
        c = -19;
        for (int i = 0; i < 16; i++){
            int64_t x = t.v[i] + c;
            t.v[i] = x & 0xffff;
            c = x >> 16;
        }
    }
    for (int i = 0; i < 16; i++){
        s[2 * i]     = (uint8_t)(t.v[i] & 0xff);
        s[2 * i + 1] = (uint8_t)((t.v[i] >> 8) & 0xff);
    }
    s[31] &= 0x7f;
}

/* Constant-time conditional swap (b is 0 or 1). */
static void fe_cswap(fe25519* f, fe25519* g, int64_t b){
    int64_t mask = -b;
    for (int i = 0; i < 16; i++){
        int64_t x = (f->v[i] ^ g->v[i]) & mask;
        f->v[i] ^= x;
        g->v[i] ^= x;
    }
}

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]){
    fe25519 x1, x2, z2, x3, z3, A, AA, B, BB, E, C, D, DA, CB, t, a24, a24e;
    uint8_t k[32];
    for (int i = 0; i < 32; i++) k[i] = scalar[i];
    k[0]  &= 248;      /* clamp (RFC 7748 section 5) */
    k[31] &= 127;
    k[31] |= 64;

    fe_frombytes(&x1, point);
    fe_one(&x2);
    fe_zero(&z2);
    fe_copy(&x3, &x1);
    fe_one(&z3);

    fe_zero(&a24); a24.v[0] = 121665;   /* (A-2)/4 for curve25519 */

    int64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--){
        int64_t kt = (k[pos >> 3] >> (pos & 7)) & 1;
        swap ^= kt;
        fe_cswap(&x2, &x3, swap);
        fe_cswap(&z2, &z3, swap);
        swap = kt;

        fe_add(&A, &x2, &z2);        /* A  = x2 + z2 */
        fe_sq(&AA, &A);              /* AA = A^2     */
        fe_sub(&B, &x2, &z2);        /* B  = x2 - z2 */
        fe_sq(&BB, &B);              /* BB = B^2     */
        fe_sub(&E, &AA, &BB);        /* E  = AA - BB */
        fe_add(&C, &x3, &z3);        /* C  = x3 + z3 */
        fe_sub(&D, &x3, &z3);        /* D  = x3 - z3 */
        fe_mul(&DA, &D, &A);         /* DA = D * A   */
        fe_mul(&CB, &C, &B);         /* CB = C * B   */

        fe_add(&t, &DA, &CB);
        fe_sq(&x3, &t);              /* x3 = (DA + CB)^2 */
        fe_sub(&t, &DA, &CB);
        fe_sq(&z3, &t);
        fe_mul(&z3, &z3, &x1);       /* z3 = x1 * (DA - CB)^2 */

        fe_mul(&x2, &AA, &BB);       /* x2 = AA * BB */
        fe_mul(&a24e, &E, &a24);     /* a24 * E      */
        fe_add(&t, &AA, &a24e);
        fe_mul(&z2, &E, &t);         /* z2 = E * (AA + a24*E) */
    }
    fe_cswap(&x2, &x3, swap);
    fe_cswap(&z2, &z3, swap);

    fe_invert(&t, &z2);
    fe_mul(&x2, &x2, &t);
    fe_tobytes(out, &x2);

    uint8_t z = 0;
    for (int i = 0; i < 32; i++) z |= out[i];
    return z == 0 ? -1 : 0;
}

void x25519_public(uint8_t pub[32], const uint8_t scalar[32]){
    uint8_t base[32];
    zero_mem(base, 32);
    base[0] = 9;
    x25519(pub, scalar, base);
}

// ---------------------------------------------------------------------
//  CSPRNG shim -- the kernel provides real entropy via tls_crypto_seed().
//  Everything else here is a CSPRNG-quality DRBG once seeded; until seeded
//  it is deterministic, and TLS refuses to run unseeded (see tls_client).
// ---------------------------------------------------------------------
static uint8_t g_rng_state[32];
static uint8_t g_rng_counter[8];
static int     g_rng_seeded = 0;

extern "C" void tls_crypto_seed(const uint8_t* entropy, int len){
    uint8_t seed[64];
    zero_mem(seed, 64);
    if (entropy && len > 0) copy_mem(seed, entropy, len > 64 ? 64 : len);
    sha256(seed, 64, g_rng_state);
    g_rng_seeded = 1;
}

void tls_crypto_fill_random(uint8_t* out, int len){
    int done = 0;
    while (done < len){
        sha256_ctx c; sha256_init(&c);
        sha256_update(&c, g_rng_state, 32);
        sha256_update(&c, g_rng_counter, 8);
        uint8_t blk[32]; sha256_final(&c, blk);
        // counter++ (byte array, little-endian)
        for (int i = 0; i < 8; i++){ if (++g_rng_counter[i]) break; }
        copy_mem(g_rng_state, blk, 32);      /* forward-secure rekey */
        int take = len - done;
        if (take > 32) take = 32;
        copy_mem(out + done, blk, take);
        done += take;
    }
}