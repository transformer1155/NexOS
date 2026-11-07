#ifndef NEXOS_TLS_CRYPTO_H
#define NEXOS_TLS_CRYPTO_H
/* tls_crypto.h -- self-contained crypto primitives for the in-kernel TLS 1.3
 * client.  Freestanding: no libc, no libm, no external library.
 *
 * SCOPE (deliberately minimal, matching what TLS 1.3 cipher suites need)
 *   SHA-256                      RFC 6234
 *   HMAC-SHA256                  RFC 2104
 *   HKDF-Extract / HKDF-Expand   RFC 5869
 *   ChaCha20                     RFC 8439 3.2
 *   Poly1305                     RFC 8439 2.5
 *   AEAD_CHACHA20_POLY1305       RFC 8439 2.8
 *   AES-128 / AES-256 + GCM      FIPS-197 / SP 800-38D
 *   X25519                       RFC 7748
 *
 * DESIGN NOTES
 *   * Only the TLS 1.3 suites are supported: TLS_CHACHA20_POLY1305_SHA256
 *     (preferred -- pure 32-bit, no table lookups) and
 *     TLS_AES_128_GCM_SHA256 / TLS_AES_256_GCM_SHA384(LOW).  AES is implemented
 *     in constant-time-friendly bitsliced form to avoid cache-timing leaks
 *     from S-box lookups.
 *   * Everything is 32-bit clean so it builds under -m32 -ffreestanding;
 *     64-bit products are only ever 32x32 -> 64 (a single 'mul'), never
 *     64x64, so no libgcc helpers are pulled in.
 *   * Every primitive is verified against its RFC test vectors by
 *     tools/test_tls_crypto.cpp (host build), which the Makefile runs.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- SHA-256 ---------------- */
typedef struct {
    uint32_t h[8];
    uint64_t len;          /* total bytes absorbed */
    uint8_t  buf[64];
    int      buflen;
} sha256_ctx;

void sha256_init(sha256_ctx* c);
void sha256_update(sha256_ctx* c, const void* data, int len);
void sha256_final(sha256_ctx* c, uint8_t out[32]);
/* One-shot convenience. */
void sha256(const void* data, int len, uint8_t out[32]);

/* ---------------- HMAC-SHA256 ---------------- */
typedef struct {
    sha256_ctx inner;
    sha256_ctx outer;
} hmac_sha256_ctx;

void hmac_sha256_init(hmac_sha256_ctx* c, const uint8_t* key, int keylen);
void hmac_sha256_update(hmac_sha256_ctx* c, const void* data, int len);
void hmac_sha256_final(hmac_sha256_ctx* c, uint8_t out[32]);
void hmac_sha256(const uint8_t* key, int keylen, const void* data, int len,
                 uint8_t out[32]);

/* ---------------- HKDF (RFC 5869) ---------------- */
/* HKDF-Extract(salt, ikm) -> prk (32 bytes).  salt may be NULL (treated as 0s). */
void hkdf_extract(const uint8_t* salt, int saltlen,
                  const uint8_t* ikm, int ikmlen, uint8_t prk[32]);
/* HKDF-Expand(prk, info, outlen).  outlen <= 255*32.  Returns 0 on success. */
int  hkdf_expand(const uint8_t prk[32], const uint8_t* info, int infolen,
                 uint8_t* out, int outlen);

/* ---------------- ChaCha20 / Poly1305 (RFC 8439) ---------------- */
/* ChaCha20 block function: 64-byte keystream block into out. */
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);
/* XOR 'len' bytes of keystream starting at block 'counter'. */
void chacha20_xor(const uint8_t key[32], uint32_t counter,
                  const uint8_t nonce[12], const uint8_t* in, uint8_t* out,
                  int len);

/* Poly1305: key is 32 bytes (r || s).  One-shot over msg -> 16-byte tag. */
void poly1305(const uint8_t key[32], const uint8_t* msg, int len,
              uint8_t out[16]);

/* AEAD_CHACHA20_POLY1305 (RFC 8439 2.8).
 * Seals 'plain' into 'out' (out must hold len+16), returns 0.
 * Opens 'sealed' (len+16) into 'out', returns 0 on success, -1 on tag mismatch. */
int  aead_chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t* aad, int aadlen,
                                const uint8_t* plain, int len, uint8_t* out);
int  aead_chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t* aad, int aadlen,
                                const uint8_t* sealed, int len, uint8_t* out);

/* ---------------- AES-128/256 + GCM (for TLS_AES_128_GCM_SHA256) ---------- */
typedef struct {
    uint32_t rk[60];       /* round keys, 4 words per round */
    int      rounds;       /* 10 (128-bit) or 14 (256-bit) */
} aes_ctx;

void aes_init(aes_ctx* c, const uint8_t* key, int keylen);   /* 16 or 32 */
void aes_encrypt_block(const aes_ctx* c, const uint8_t in[16], uint8_t out[16]);

/* AES-GCM.  out holds len+16 (ciphertext || tag). */
int  aead_aes_gcm_seal(const uint8_t* key, int keylen, const uint8_t nonce[12],
                       const uint8_t* aad, int aadlen,
                       const uint8_t* plain, int len, uint8_t* out);
int  aead_aes_gcm_open(const uint8_t* key, int keylen, const uint8_t nonce[12],
                       const uint8_t* aad, int aadlen,
                       const uint8_t* sealed, int len, uint8_t* out);

/* ---------------- X25519 (RFC 7748) ---------------- */
/* Computes out = scalar * point (X25519).  Returns 0 on success, -1 if the
 * result is the all-zero point (invalid public key). */
int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
/* Derives a public key from a private scalar. */
void x25519_public(uint8_t pub[32], const uint8_t scalar[32]);

/* ---------------- helpers ---------------- */
/* Constant-time comparison: returns 1 if equal, 0 otherwise. */
int ct_memcmp(const void* a, const void* b, int len);

/* Seed the DRBG.  Must be called once before any TLS handshake; the kernel
 * feeds it TSC+counter material (see net_tls.cpp), a host build feeds it
 * os.urandom.  Unseeded output is deterministic, and tls13 refuses nothing --
 * so callers MUST seed. */
void tls_crypto_seed(const uint8_t* entropy, int len);
/* Best-effort CSPRNG seed material; the kernel supplies real entropy. */
void tls_crypto_fill_random(uint8_t* out, int len);

#ifdef __cplusplus
}
#endif
#endif
