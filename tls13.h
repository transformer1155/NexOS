#ifndef NEXOS_TLS13_H
#define NEXOS_TLS13_H
/* tls13.h -- minimal TLS 1.3 client for NexOS (in-kernel).
 *
 * SCOPE
 *   * TLS 1.3 only (no 1.2 downgrade; downgrade is a security trap anyway).
 *   * One cipher suite: TLS_CHACHA20_POLY1305_SHA256 (0x1303).  Pure 32-bit,
 *     constant-time-friendly, and offered by every modern TLS 1.3 endpoint.
 *   * Key exchange: X25519.
 *   * Certificate verification: pluggable.  tls13_connect() REQUIRES a
 *     verifier; there is deliberately no "skip verification" switch in the
 *     production entry point.  A host-only test hook exists separately.
 *
 * TRANSPORT
 *   The caller supplies blocking send/recv callbacks, so the same client runs
 *   over the kernel's tcp_client (net.cpp) and over a POSIX socket in the host
 *   test harness.
 */

#include <stdint.h>
#include "tls_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record payload cap (RFC 8446 5.1 allows 2^14) and the handshake-message
 * buffer cap.  The latter is pure .bss in the kernel: 32 KB here meant 64 KB
 * of static data for the handshake + certificate buffers, which pushed the
 * 32-bit image through the EBDA / HEAP_START limits.  Real chains are small
 * (api.deepseek.com sends 3 certificates, ~3.5 KB total); 8 KB covers that
 * with room for a 4-5 certificate chain.  A server that sends more fails the
 * handshake cleanly instead of corrupting memory. */
#define TLS13_MAX_RECORD      16384
#define TLS13_MAX_HANDSHAKE   8192

typedef struct {
    /* Must send exactly len bytes; return len on success, <0 on error. */
    int (*send)(void* ctx, const uint8_t* buf, int len);
    /* Must return >0 bytes, 0 on clean EOF, <0 on error. May block. */
    int (*recv)(void* ctx, uint8_t* buf, int maxlen);
    void* ctx;
} tls13_io;

/* Verifies the server's certificate chain for hostname.
 *   certs    -- concatenated DER certs, each prefixed by a 3-byte length
 *               exactly as they appear in the TLS Certificate message.
 *   count    -- number of certificates.
 *   out_reason -- optional, receives a short static reason on failure.
 * Returns 0 to accept, <0 to reject.  MUST reject on any parse error. */
typedef int (*tls13_verify_fn)(const uint8_t* certs, int certs_len, int count,
                               const char* hostname, const char** out_reason);

typedef struct {
    tls13_io        io;
    tls13_verify_fn verify;

    /* traffic keys: handshake and application, one direction each */
    uint8_t  client_hs_key[32], client_hs_iv[12];
    uint8_t  server_hs_key[32], server_hs_iv[12];
    /* traffic SECRETS (not keys): Finished is HMAC(finished_key, transcript)
       where finished_key = HKDF-Expand-Label(traffic_secret, "finished", ...) */
    uint8_t  client_hs_secret[32], server_hs_secret[32];
    uint8_t  client_ap_key[32], client_ap_iv[12];
    uint8_t  server_ap_key[32], server_ap_iv[12];

    uint64_t client_hs_seq, server_hs_seq;
    uint64_t client_ap_seq, server_ap_seq;

    uint8_t  read_key[32], read_iv[12];    /* current inbound  */
    uint8_t  write_key[32], write_iv[12];  /* current outbound */
    uint64_t read_seq, write_seq;
    int      encrypted;                    /* records are AEAD-protected */

    /* transcript hash of all handshake messages (RFC 8446 4.4.1), kept as a
       running SHA-256 so no 32 KB handshake buffer is needed in kernel .bss */
    sha256_ctx transcript_ctx;
    uint8_t  transcript[32];
    int      transcript_len;               /* bytes absorbed (for debugging) */

    uint8_t  client_priv[32], client_pub[32];
    uint8_t  master_secret[32];

    char     error[96];
    int      established;
} tls13_conn;

/* Handshake over c->io with the given server name.  verify is mandatory.
 * Returns 0 on success; on failure c->error says why and a negative code is
 * returned (see TLS13_E_*). */
#define TLS13_E_IO      (-1)
#define TLS13_E_PROTO   (-2)
#define TLS13_E_VERIFY  (-3)
#define TLS13_E_CRYPTO  (-4)
#define TLS13_E_NOMEM   (-5)

/* Handshake + full certificate verification.
 *   anchors/n_anchors -- SHA-256 fingerprints of accepted roots (>=1 required;
 *                        passing 0 anchors is an error, never "trust anything").
 * CertificateVerify signatures are checked too, so a server cannot present a
 * valid chain for a different key. */
int  tls13_connect(tls13_conn* c, const char* hostname,
                   const uint8_t* anchors, int n_anchors);

/* Application data.  Returns bytes written, or <0. */
int  tls13_write(tls13_conn* c, const void* data, int len);
/* Reads up to maxlen plaintext bytes.  Returns >0, 0 on close_notify/EOF, <0. */
int  tls13_read(tls13_conn* c, void* buf, int maxlen);

/* Send close_notify. */
void tls13_close(tls13_conn* c);

/* Convenience: one HTTP/1.1 request over an established connection.
 * Writes the raw response (headers + body) into out and returns its length. */
int tls13_http_request(tls13_conn* c, const char* host,
                       const char* method, const char* path,
                       const char* extra_headers,
                       const uint8_t* body, int body_len,
                       char* out, int out_cap);

/* ---- host-test only ------------------------------------------------
 * A verifier that accepts any certificate chain.  Compiled only when
 * TLS13_HOST_TEST is defined; the kernel build has no such symbol, so
 * shipping code cannot accidentally disable verification. */
#ifdef TLS13_HOST_TEST
int tls13_verify_accept_all(const uint8_t* certs, int certs_len, int count,
                            const char* hostname, const char** out_reason);
#endif

#ifdef __cplusplus
}
#endif
#endif
