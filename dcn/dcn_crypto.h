#ifndef DCN_CRYPTO_H
#define DCN_CRYPTO_H

#include "dcn_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================= Hashes ================= */
DCN_API void dcn_sha1(const uint8_t* data, size_t len, uint8_t out[20]);
DCN_API void dcn_md5 (const uint8_t* data, size_t len, uint8_t out[16]);

/* ================= HMAC ================= */
DCN_API void dcn_hmac_sha1(const uint8_t* key, size_t klen,
                           const uint8_t* msg, size_t mlen, uint8_t out[20]);
DCN_API void dcn_hmac_md5 (const uint8_t* key, size_t klen,
                           const uint8_t* msg, size_t mlen, uint8_t out[16]);

/* ================= PBKDF2 (PMK from passphrase) ================= */
/* Derives outlen bytes (<= 64) using PBKDF2-HMAC-SHA1. */
DCN_API void dcn_pbkdf2_hmac_sha1(const uint8_t* pw,    size_t pwlen,
                                  const uint8_t* salt,  size_t saltlen,
                                  int iters,
                                  uint8_t* out, size_t outlen);

/* ================= WPA2 PRF ================= */
/* General PRF-X: outbits MUST be a multiple of 8.
   Concatenation is  label || 0x00 || data || counter(i as 1 byte). */
DCN_API void dcn_prf_x(const uint8_t* key, size_t klen,
                       const uint8_t* label, size_t labellen,
                       const uint8_t* data,  size_t datalen,
                       uint8_t* out, size_t outbits);

/* WPA2 Pairwise Transient Key (64 bytes) for CCMP.
   PTK layout: KCK[16] | KEK[16] | TK[16] | MIC_Tx[8] | MIC_Rx[8] */
DCN_API void dcn_wpa2_derive_ptk(const uint8_t pmk[DCN_PMK_LEN],
                                 const uint8_t aa[DCN_MAC_LEN],
                                 const uint8_t spa[DCN_MAC_LEN],
                                 const uint8_t anonce[DCN_NONCE_LEN],
                                 const uint8_t snonce[DCN_NONCE_LEN],
                                 uint8_t ptk[DCN_PTK_LEN]);

/* WPA2 Group Temporal Key (32 bytes). */
DCN_API void dcn_wpa2_derive_gtk(const uint8_t gmk[DCN_PMK_LEN],
                                 const uint8_t aa[DCN_MAC_LEN],
                                 const uint8_t gnonce[DCN_NONCE_LEN],
                                 uint8_t gtk[DCN_GTK_LEN]);

/* WPA2 EAPOL MIC (CCMP uses HMAC-SHA1, take first 16 bytes).
   `eapol` must have its 16-byte MIC field already zeroed. */
DCN_API void dcn_wpa2_mic(const uint8_t kck[DCN_KCK_LEN],
                          const uint8_t* eapol, size_t len,
                          uint8_t mic[DCN_MIC_LEN]);

/* ================= AES-128 ================= */
DCN_API void dcn_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
DCN_API void dcn_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* RFC 3394 AES Key Wrap (KEK is 16 bytes; plain multiple of 8, >= 16).
   cipher length = plainlen + 8. Returns 0 on success. */
DCN_API int dcn_aes_key_wrap (const uint8_t kek[16], const uint8_t* plain,   size_t plainlen, uint8_t* cipher);
DCN_API int dcn_aes_key_unwrap(const uint8_t kek[16], const uint8_t* cipher, size_t cipherlen, uint8_t* plain);

/* ================= Self test ================= */
/* Runs all RFC / FIPS-known-answer tests + the real WPA2 capture vectors.
   Returns 0 if every check passes, else the index (+1) of the first failure. */
DCN_API int dcn_crypto_selftest(void);

#ifdef __cplusplus
}
#endif
#endif /* DCN_CRYPTO_H */
