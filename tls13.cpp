/* tls13.cpp -- minimal TLS 1.3 client.  See tls13.h for scope.
 *
 * Built strictly to RFC 8446:
 *   4.1.2  ClientHello / cipher suites
 *   4.2    extensions (supported_versions, key_share, signature_algorithms)
 *   4.4.1  transcript hash
 *   5.2    record layer (AEAD, 5-byte AAD, inner content type)
 *   7.1    key schedule (HKDF-Expand-Label / Derive-Secret)
 *   4.4.4  Finished (HMAC over transcript)
 */

#include "tls13.h"
#include "tls_x509.h"

/* Deliberately NOT <string.h>.  Under the kernel's -O2 build glibc's fortify
 * headers rewrite memcpy() into __memcpy_chk(), which the freestanding 64-bit
 * kernel does not link: the linker binds it to address 0 and the first TLS
 * memcpy triple-faults.  Local helpers keep this unit libc-free. */
static void tls_memcpy(void* d, const void* s, int n){
    uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s;
    for (int i = 0; i < n; i++) a[i] = b[i];
}
static void tls_memset(void* d, int v, int n){
    uint8_t* p = (uint8_t*)d;
    for (int i = 0; i < n; i++) p[i] = (uint8_t)v;
}
#define memcpy tls_memcpy
#define memset tls_memset

#ifdef TLS13_HOST_TEST
/* Debug taps for the host harness only (never compiled into the kernel):
   lets the test hand the exact signed bytes to openssl for an independent
   second opinion when a CertificateVerify signature is rejected. */
uint16_t tls13_dbg_scheme;
uint8_t  tls13_dbg_th[32];
int      tls13_dbg_siglen;
uint8_t  tls13_dbg_sig[512];
#endif

#define HS_CLIENT_HELLO        1
#define HS_SERVER_HELLO        2
#define HS_ENCRYPTED_EXTS      8
#define HS_CERTIFICATE         11
#define HS_CERT_VERIFY         15
#define HS_FINISHED            20

#define CT_CHANGE_CIPHER_SPEC  20
#define CT_ALERT               21
#define CT_HANDSHAKE           22
#define CT_APPLICATION_DATA    23

#define EXT_SERVER_NAME        0x0000
#define EXT_SUPPORTED_GROUPS   0x000a
#define EXT_SIG_ALGS           0x000d
#define EXT_SUPPORTED_VERSIONS 0x002b
#define EXT_KEY_SHARE          0x0033

#define GROUP_X25519           0x001d
#define SUITE_CHACHA20_SHA256  0x1303

// ---------------------------------------------------------------------
//  byte helpers
// ---------------------------------------------------------------------
static void w8(uint8_t* p, uint8_t v){ p[0] = v; }
static void w16(uint8_t* p, uint16_t v){ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w24(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}
static uint16_t r16(const uint8_t* p){ return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t r24(const uint8_t* p){
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void set_error(tls13_conn* c, const char* msg){
    int i = 0;
    if (msg) while (msg[i] && i < (int)sizeof(c->error) - 1){ c->error[i] = msg[i]; i++; }
    c->error[i] = 0;
}

// ---------------------------------------------------------------------
//  HKDF-Expand-Label / Derive-Secret (RFC 8446 7.1)
// ---------------------------------------------------------------------
static int hkdf_expand_label(const uint8_t secret[32], const char* label,
                             const uint8_t* context, int ctxlen,
                             uint8_t* out, int outlen){
    uint8_t info[2 + 1 + 255 + 1 + 255];
    int n = 0;
    w16(info + n, (uint16_t)outlen); n += 2;
    const char* prefix = "tls13 ";
    int plen = 6;
    int llen = 0; while (label[llen]) llen++;
    info[n++] = (uint8_t)(plen + llen);
    for (int i = 0; i < plen; i++) info[n++] = (uint8_t)prefix[i];
    for (int i = 0; i < llen; i++) info[n++] = (uint8_t)label[i];
    info[n++] = (uint8_t)(ctxlen & 0xff);
    for (int i = 0; i < ctxlen; i++) info[n++] = context[i];
    return hkdf_expand(secret, info, n, out, outlen);
}

static void derive_secret(const uint8_t secret[32], const char* label,
                          const uint8_t transcript_hash[32], uint8_t out[32]){
    hkdf_expand_label(secret, label, transcript_hash, 32, out, 32);
}

/* Transcript-Hash("") -- SHA-256 of the empty string.  Used by the two
 * "derived" steps in the key schedule, whose context is the empty transcript,
 * NOT the current handshake transcript.  Mixing those up yields handshake
 * keys that decrypt nothing (first EncryptedExtensions record fails its tag). */
static void empty_hash(uint8_t out[32]){ sha256("", 0, out); }

static void transcript_hash(const tls13_conn* c, uint8_t out[32]){
    sha256_ctx t = c->transcript_ctx;      /* copy: final must not disturb */
    sha256_final(&t, out);
}

static void transcript_add(tls13_conn* c, const uint8_t* data, int len){
    sha256_update(&c->transcript_ctx, data, len);
    c->transcript_len += len;
}

// ---------------------------------------------------------------------
//  record layer
// ---------------------------------------------------------------------
static int send_all(tls13_conn* c, const uint8_t* buf, int len){
    int off = 0;
    while (off < len){
        int n = c->io.send(c->io.ctx, buf + off, len - off);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

static int recv_exact(tls13_conn* c, uint8_t* buf, int len){
    int off = 0;
    while (off < len){
        int n = c->io.recv(c->io.ctx, buf + off, len - off);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

/* Send one record.  When c->encrypted, wraps in AEAD with inner type. */
static int send_record(tls13_conn* c, uint8_t type, const uint8_t* payload, int len){
    static uint8_t rec[TLS13_MAX_RECORD + 64];
    if (len > TLS13_MAX_RECORD) return -1;

    if (!c->encrypted){
        rec[0] = type; rec[1] = 0x03; rec[2] = 0x03;
        w16(rec + 3, (uint16_t)len);
        if (len) memcpy(rec + 5, payload, len);
        return send_all(c, rec, 5 + len);
    }

    int inner = len + 1;
    int total = inner + 16;
    rec[0] = CT_APPLICATION_DATA; rec[1] = 0x03; rec[2] = 0x03;
    w16(rec + 3, (uint16_t)total);
    uint8_t* body = rec + 5;
    if (len) memcpy(body, payload, len);
    body[len] = type;

    uint8_t nonce[12];
    memcpy(nonce, c->write_iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(c->write_seq >> (8 * i));

    if (aead_chacha20poly1305_seal(c->write_key, nonce, rec, 5, body, inner, body) != 0)
        return -1;
    c->write_seq++;
    return send_all(c, rec, 5 + total);
}

/* Receive one record; returns its plaintext in payload and type in *type. */
static int recv_record(tls13_conn* c, uint8_t* type, uint8_t* payload, int maxlen){
    uint8_t hdr[5];
    if (recv_exact(c, hdr, 5) != 0) return -1;
    int len = r16(hdr + 3);
    if (len <= 0 || len > TLS13_MAX_RECORD + 256) return -1;

    static uint8_t rec[TLS13_MAX_RECORD + 256];
    if (recv_exact(c, rec, len) != 0) return -1;

    uint8_t outer = hdr[0];
    if (outer == CT_CHANGE_CIPHER_SPEC){
        *type = CT_CHANGE_CIPHER_SPEC;
        return 0;                              /* ignore (compat only) */
    }
    if (!c->encrypted){
        if (len > maxlen) return -1;
        *type = outer;
        memcpy(payload, rec, len);
        return len;
    }
    if (outer != CT_APPLICATION_DATA) return -1;

    uint8_t nonce[12];
    memcpy(nonce, c->read_iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(c->read_seq >> (8 * i));

    static uint8_t plain[TLS13_MAX_RECORD + 256];
    if (aead_chacha20poly1305_open(c->read_key, nonce, hdr, 5, rec, len, plain) != 0)
        return -1;                             /* bad tag: abort, never continue */
    c->read_seq++;
    int plen = len - 16;
    if (plen < 1) return -1;
    *type = plain[plen - 1];
    plen -= 1;
    if (plen > maxlen) return -1;
    memcpy(payload, plain, plen);
    return plen;
}

// ---------------------------------------------------------------------
//  handshake message assembly
// ---------------------------------------------------------------------
typedef struct {
    uint8_t* buf;
    int      cap;
    int      len;
    int      ok;
} wbuf;

static void wb_bytes(wbuf* w, const void* p, int n){
    if (!w->ok || w->len + n > w->cap){ w->ok = 0; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}
static void wb_u8(wbuf* w, uint8_t v){ wb_bytes(w, &v, 1); }
static void wb_u16(wbuf* w, uint16_t v){ uint8_t b[2]; w16(b, v); wb_bytes(w, b, 2); }
static void wb_u24(wbuf* w, uint32_t v){ uint8_t b[3]; w24(b, v); wb_bytes(w, b, 3); }

// ---------------------------------------------------------------------
//  handshake I/O with buffering (one record can hold several messages)
// ---------------------------------------------------------------------
static uint8_t g_hs_buf[TLS13_MAX_HANDSHAKE];
static int     g_hs_len, g_hs_off;

static int hs_next(tls13_conn* c, uint8_t* type, const uint8_t** body, int* blen){
    for (;;){
        if (g_hs_off + 4 <= g_hs_len){
            uint32_t l = r24(g_hs_buf + g_hs_off + 1);
            if (g_hs_off + 4 + (int)l <= g_hs_len){
                *type = g_hs_buf[g_hs_off];
                *body = g_hs_buf + g_hs_off + 4;
                *blen = (int)l;
                g_hs_off += 4 + (int)l;
                return 0;
            }
        }
        /* need another record */
        uint8_t rt; int n;
        if (g_hs_off > 0 && g_hs_off == g_hs_len){ g_hs_off = g_hs_len = 0; }
        n = recv_record(c, &rt, g_hs_buf + g_hs_len, (int)sizeof(g_hs_buf) - g_hs_len);
        if (n < 0) return -1;
        if (rt == CT_CHANGE_CIPHER_SPEC) continue;
        if (rt != CT_HANDSHAKE) return -1;
        g_hs_len += n;
    }
}

// ---------------------------------------------------------------------
//  ClientHello
// ---------------------------------------------------------------------
static int build_client_hello(tls13_conn* c, const char* hostname,
                              uint8_t* out, int cap){
    wbuf w; w.buf = out; w.cap = cap; w.len = 0; w.ok = 1;

    wb_u8(&w, HS_CLIENT_HELLO);
    int lenpos = w.len; wb_u24(&w, 0);          /* patched later */

    uint8_t rand32[32];
    tls_crypto_fill_random(rand32, 32);
    wb_u16(&w, 0x0303);                          /* legacy_version */
    wb_bytes(&w, rand32, 32);

    uint8_t sid[32];
    tls_crypto_fill_random(sid, 32);
    wb_u8(&w, 32); wb_bytes(&w, sid, 32);        /* legacy_session_id */

    wb_u16(&w, 2); wb_u16(&w, SUITE_CHACHA20_SHA256);
    wb_u8(&w, 1); wb_u8(&w, 0);                  /* null compression */

    int ext_len_pos = w.len; wb_u16(&w, 0);      /* extensions length */

    /* server_name */
    {
        int hn = 0; while (hostname[hn]) hn++;
        wb_u16(&w, EXT_SERVER_NAME);
        wb_u16(&w, (uint16_t)(hn + 5));
        wb_u16(&w, (uint16_t)(hn + 3));
        wb_u8(&w, 0);                            /* host_name */
        wb_u16(&w, (uint16_t)hn);
        wb_bytes(&w, hostname, hn);
    }
    /* supported_groups: x25519 */
    wb_u16(&w, EXT_SUPPORTED_GROUPS); wb_u16(&w, 4);
    wb_u16(&w, 2); wb_u16(&w, GROUP_X25519);
    /* signature_algorithms: rsa_pss_rsae_sha256, ecdsa_secp256r1_sha256,
       rsa_pkcs1_sha256 (for older chains) */
    wb_u16(&w, EXT_SIG_ALGS); wb_u16(&w, 8);
    wb_u16(&w, 6);
    wb_u16(&w, 0x0804); wb_u16(&w, 0x0403); wb_u16(&w, 0x0401);
    /* supported_versions: TLS 1.3 only */
    wb_u16(&w, EXT_SUPPORTED_VERSIONS); wb_u16(&w, 3);
    wb_u8(&w, 2); wb_u16(&w, 0x0304);
    /* key_share: one x25519 share.
       Layout (RFC 8446 4.2.8):
         extension_data = client_shares<0..2^16-1>
         KeyShareEntry   = group(2) + key_exchange<1..2^16-1>
         key_exchange    = <2-byte length> + 32 bytes
       Forgetting the inner 2-byte length is what OpenSSL reports as
       "extract_keyshares: length mismatch". */
    wb_u16(&w, EXT_KEY_SHARE);
    wb_u16(&w, 2 + 2 + 2 + 32);      /* extension_data length */
    wb_u16(&w, 2 + 2 + 32);          /* client_shares length  */
    wb_u16(&w, GROUP_X25519);
    wb_u16(&w, 32);                  /* key_exchange length   */
    wb_bytes(&w, c->client_pub, 32);

    if (!w.ok) return -1;
    int ext_total = w.len - ext_len_pos - 2;
    w16(out + ext_len_pos, (uint16_t)ext_total);
    int body_len = w.len - lenpos - 3;
    w24(out + lenpos, (uint32_t)body_len);
    return w.len;
}

// ---------------------------------------------------------------------
//  ServerHello
// ---------------------------------------------------------------------
static int parse_server_hello(tls13_conn* c, const uint8_t* b, int n,
                              uint8_t server_pub[32]){
    if (n < 2 + 32 + 1) return -1;
    int p = 0;
    if (r16(b + p) != 0x0303) return -1;   p += 2;
    p += 32;                                /* random */
    int sidlen = b[p++];
    if (p + sidlen > n) return -1;
    p += sidlen;
    if (p + 3 > n) return -1;
    if (r16(b + p) != SUITE_CHACHA20_SHA256) return -1;
    p += 2;
    if (b[p++] != 0) return -1;
    if (p + 2 > n) return -1;
    int extlen = r16(b + p); p += 2;
    if (p + extlen > n) return -1;
    int end = p + extlen;

    int got_ver = 0, got_share = 0;
    while (p + 4 <= end){
        int t = r16(b + p);
        int l = r16(b + p + 2);
        p += 4;
        if (p + l > end) return -1;
        if (t == EXT_SUPPORTED_VERSIONS){
            if (l != 2 || r16(b + p) != 0x0304) return -1;
            got_ver = 1;
        } else if (t == EXT_KEY_SHARE){
            if (l < 4) return -1;
            if (r16(b + p) != GROUP_X25519) return -1;
            int kl = r16(b + p + 2);
            if (kl != 32 || l != 4 + 32) return -1;
            memcpy(server_pub, b + p + 4, 32);
            got_share = 1;
        }
        p += l;
    }
    return (got_ver && got_share) ? 0 : -1;
}

// ---------------------------------------------------------------------
//  key schedule (RFC 8446 7.1)
// ---------------------------------------------------------------------
static void setup_traffic_keys(tls13_conn* c, const uint8_t secret[32],
                               const char* label,
                               uint8_t out_secret[32],
                               uint8_t key[32], uint8_t iv[12], uint64_t* seq){
    uint8_t th[32];
    transcript_hash(c, th);
    uint8_t ts[32];
    derive_secret(secret, label, th, ts);
    if (out_secret) memcpy(out_secret, ts, 32);
    hkdf_expand_label(ts, "key", 0, 0, key, 32);
    hkdf_expand_label(ts, "iv", 0, 0, iv, 12);
    *seq = 0;
}

// ---------------------------------------------------------------------
//  public API
// ---------------------------------------------------------------------
int tls13_connect(tls13_conn* c, const char* hostname,
                  const uint8_t* anchors, int n_anchors){
    if (!c || !hostname || !hostname[0]) return TLS13_E_PROTO;
    if (!anchors || n_anchors <= 0){ return TLS13_E_VERIFY; }   /* nothing trusted */
    /* Preserve the caller-supplied transport across the reset: memset() of the
       whole struct would otherwise null out io.send/io.recv and the first
       write would jump through a NULL pointer. */
    tls13_io io = c->io;
    memset(c, 0, sizeof(*c));
    c->io = io;

    /* Ephemeral X25519 key pair for this connection. */
    tls_crypto_fill_random(c->client_priv, 32);
    x25519_public(c->client_pub, c->client_priv);

    uint8_t ch[1024];
    int chlen = build_client_hello(c, hostname, ch, sizeof(ch));
    if (chlen < 0){ set_error(c, "client_hello_build"); return TLS13_E_PROTO; }

    sha256_init(&c->transcript_ctx);
    transcript_add(c, ch, chlen);
    if (send_record(c, CT_HANDSHAKE, ch, chlen) != 0){
        set_error(c, "send_client_hello"); return TLS13_E_IO;
    }

    /* --- ServerHello --- */
    g_hs_len = g_hs_off = 0;
    uint8_t mt; const uint8_t* body; int blen;
    if (hs_next(c, &mt, &body, &blen) != 0){
        set_error(c, "read_server_hello"); return TLS13_E_IO;
    }
    if (mt != HS_SERVER_HELLO){ set_error(c, "expected_server_hello"); return TLS13_E_PROTO; }

    uint8_t sh_copy[TLS13_MAX_HANDSHAKE];
    int sh_total = blen + 4;
    sh_copy[0] = mt; w24(sh_copy + 1, (uint32_t)blen);
    memcpy(sh_copy + 4, body, blen);
    transcript_add(c, sh_copy, sh_total);

    uint8_t server_pub[32];
    if (parse_server_hello(c, body, blen, server_pub) != 0){
        set_error(c, "bad_server_hello"); return TLS13_E_PROTO;
    }

    /* --- ECDHE + handshake secrets --- */
    uint8_t shared[32];
    if (x25519(shared, c->client_priv, server_pub) != 0){
        set_error(c, "x25519_failed"); return TLS13_E_CRYPTO;
    }

    uint8_t zero[32]; memset(zero, 0, 32);
    uint8_t early[32], derived[32], hs_secret[32], th_empty[32];
    hkdf_extract(0, 0, zero, 32, early);            /* salt=0, ikm=0 */
    empty_hash(th_empty);
    derive_secret(early, "derived", th_empty, derived);
    hkdf_extract(derived, 32, shared, 32, hs_secret);

    setup_traffic_keys(c, hs_secret, "c hs traffic", c->client_hs_secret, c->client_hs_key, c->client_hs_iv, &c->client_hs_seq);
    setup_traffic_keys(c, hs_secret, "s hs traffic", c->server_hs_secret, c->server_hs_key, c->server_hs_iv, &c->server_hs_seq);

    /* from here inbound records are encrypted with the server hs key */
    memcpy(c->read_key, c->server_hs_key, 32);
    memcpy(c->read_iv,  c->server_hs_iv, 12);
    c->read_seq = 0;
    c->encrypted = 1;
    memcpy(c->write_key, c->client_hs_key, 32);
    memcpy(c->write_iv,  c->client_hs_iv, 12);
    c->write_seq = 0;

    /* --- EncryptedExtensions --- */
    if (hs_next(c, &mt, &body, &blen) != 0){
        set_error(c, "read_encrypted_extensions"); return TLS13_E_IO;
    }
    if (mt != HS_ENCRYPTED_EXTS){ set_error(c, "expected_encrypted_extensions"); return TLS13_E_PROTO; }
    {
        uint8_t tmp[4 + TLS13_MAX_HANDSHAKE];
        tmp[0] = mt; w24(tmp + 1, (uint32_t)blen);
        memcpy(tmp + 4, body, blen);
        transcript_add(c, tmp, blen + 4);
    }

    /* --- Certificate --- */
    static uint8_t certs[TLS13_MAX_HANDSHAKE];
    int certs_len = 0, cert_count = 0;
    const uint8_t* leaf_der = 0;
    int leaf_len = 0;
    int leaf_off = -1;
    if (hs_next(c, &mt, &body, &blen) != 0){
        set_error(c, "read_certificate"); return TLS13_E_IO;
    }
    if (mt != HS_CERTIFICATE){ set_error(c, "expected_certificate"); return TLS13_E_PROTO; }
    {
        uint8_t tmp[4 + TLS13_MAX_HANDSHAKE];
        tmp[0] = mt; w24(tmp + 1, (uint32_t)blen);
        memcpy(tmp + 4, body, blen);
        transcript_add(c, tmp, blen + 4);
    }
    {
        int p = 0;
        if (blen < 4) { set_error(c, "bad_certificate"); return TLS13_E_PROTO; }
        p += 1;                                  /* certificate_request_context len */
        int ctxlen = body[0];
        if (1 + ctxlen + 3 > blen) { set_error(c, "bad_certificate"); return TLS13_E_PROTO; }
        p = 1 + ctxlen;
        int listlen = r24(body + p); p += 3;
        if (p + listlen > blen){ set_error(c, "bad_certificate_list"); return TLS13_E_PROTO; }
        while (p + 3 <= blen){
            int clen = r24(body + p);
            if (p + 3 + clen > blen) break;
            if (cert_count == 0) leaf_off = certs_len;   /* remember where the copy lands */
            if (certs_len + 3 + clen <= (int)sizeof(certs)){
                certs[certs_len + 0] = body[p + 0];
                certs[certs_len + 1] = body[p + 1];
                certs[certs_len + 2] = body[p + 2];
                memcpy(certs + certs_len + 3, body + p + 3, clen);
                certs_len += 3 + clen;
                cert_count++;
            }
            p += 3 + clen;
        }
        /* Point at OUR copy, not into g_hs_buf: reading the next handshake
           message reuses that buffer and silently invalidated the pointer,
           which surfaced as cv_leaf_parse. */
        if (leaf_off >= 0 && leaf_off + 3 <= certs_len){
            leaf_der = certs + leaf_off + 3;
            leaf_len = ((int)certs[leaf_off] << 16) | ((int)certs[leaf_off+1] << 8) | certs[leaf_off+2];
        }
    }

    /* --- CertificateVerify (signature is checked by the verify hook's chain
           work in phase C; here we only require the message to be present and
           well-formed so a server cannot skip it) --- */
    if (hs_next(c, &mt, &body, &blen) != 0){
        set_error(c, "read_certificate_verify"); return TLS13_E_IO;
    }
    if (mt != HS_CERT_VERIFY){ set_error(c, "expected_certificate_verify"); return TLS13_E_PROTO; }
    if (blen < 4){ set_error(c, "bad_certificate_verify"); return TLS13_E_PROTO; }
    {
        /* Scheme and signature.  The signed transcript is everything up to and
           INCLUDING Certificate, i.e. exactly the hash at this point. */
        uint16_t scheme = (uint16_t)((body[0] << 8) | body[1]);
        int slen = (body[2] << 8) | body[3];
        if (4 + slen > blen){ set_error(c, "bad_certificate_verify"); return TLS13_E_PROTO; }
        uint8_t th_cv[32];
        transcript_hash(c, th_cv);
#ifdef TLS13_HOST_TEST
        tls13_dbg_scheme = scheme;
        memcpy(tls13_dbg_th, th_cv, 32);
        tls13_dbg_siglen = slen > 512 ? 512 : slen;
        memcpy(tls13_dbg_sig, body + 4, tls13_dbg_siglen);
#endif
        int cvr = leaf_der ? x509_verify_certificate_verify(leaf_der, leaf_len, scheme,
                                                            th_cv, body + 4, slen) : -10;
        if (cvr != 0){
            switch (cvr){
                case -10: set_error(c, "cv_no_leaf"); break;
                case -11: set_error(c, "cv_leaf_parse"); break;
                case -12: set_error(c, "cv_spki_parse"); break;
                case -13: set_error(c, "cv_scheme_unsupported"); break;
                case -14: set_error(c, "cv_pss"); break;
                case -15: set_error(c, "cv_v15"); break;
                default:  set_error(c, "cv_unknown"); break;
            }
            return TLS13_E_VERIFY;
        }
        uint8_t tmp[4 + TLS13_MAX_HANDSHAKE];
        tmp[0] = mt; w24(tmp + 1, (uint32_t)blen);
        memcpy(tmp + 4, body, blen);
        transcript_add(c, tmp, blen + 4);
    }

    /* --- verify identity (chain + hostname + anchor) --- */
    {
        const char* reason = "unspecified";
        if (x509_verify_chain(certs, certs_len, cert_count, hostname,
                              anchors, n_anchors, &reason) != 0){
            set_error(c, reason ? reason : "cert_verify_failed");
            return TLS13_E_VERIFY;
        }
    }

    /* --- server Finished --- */
    if (hs_next(c, &mt, &body, &blen) != 0){
        set_error(c, "read_finished"); return TLS13_E_IO;
    }
    if (mt != HS_FINISHED){ set_error(c, "expected_finished"); return TLS13_E_PROTO; }
    {
        uint8_t th[32];
        transcript_hash(c, th);
        uint8_t fk[32];
        hkdf_expand_label(c->server_hs_secret, "finished", 0, 0, fk, 32);
        uint8_t want[32];
        hmac_sha256(fk, 32, th, 32, want);
        if (blen != 32 || !ct_memcmp(want, body, 32)){
            set_error(c, "server_finished_mismatch");
            return TLS13_E_VERIFY;
        }
        /* transcript includes server Finished for the app-secret derivation */
        uint8_t tmp[4 + 64];
        tmp[0] = mt; w24(tmp + 1, (uint32_t)blen);
        memcpy(tmp + 4, body, blen);
        transcript_add(c, tmp, blen + 4);
    }

    /* --- application secrets --- */
    {
        uint8_t th[32], master[32];
        empty_hash(th);                    /* context for "derived" is "" */
        uint8_t cur[32];
        derive_secret(hs_secret, "derived", th, cur);
        uint8_t zeros[32]; memset(zeros, 0, 32);
        hkdf_extract(cur, 32, zeros, 32, master);
        memcpy(c->master_secret, master, 32);

        setup_traffic_keys(c, master, "c ap traffic", 0, c->client_ap_key, c->client_ap_iv, &c->client_ap_seq);
        setup_traffic_keys(c, master, "s ap traffic", 0, c->server_ap_key, c->server_ap_iv, &c->server_ap_seq);
    }

    /* --- client Finished, still under the handshake write key --- */
    {
        uint8_t th[32];
        transcript_hash(c, th);
        uint8_t fk[32], verify_data[32];
        hkdf_expand_label(c->client_hs_secret, "finished", 0, 0, fk, 32);
        hmac_sha256(fk, 32, th, 32, verify_data);

        uint8_t msg[4 + 32];
        msg[0] = HS_FINISHED; w24(msg + 1, 32);
        memcpy(msg + 4, verify_data, 32);
        transcript_add(c, msg, 4 + 32);
        if (send_record(c, CT_HANDSHAKE, msg, 4 + 32) != 0){
            set_error(c, "send_client_finished"); return TLS13_E_IO;
        }
    }

    /* switch to application keys */
    memcpy(c->read_key,  c->server_ap_key, 32);
    memcpy(c->read_iv,   c->server_ap_iv, 12);
    c->read_seq = 0;
    memcpy(c->write_key, c->client_ap_key, 32);
    memcpy(c->write_iv,  c->client_ap_iv, 12);
    c->write_seq = 0;

    transcript_hash(c, c->transcript);
    c->established = 1;
    c->error[0] = 0;
    return 0;
}

int tls13_write(tls13_conn* c, const void* data, int len){
    if (!c || !c->established) return -1;
    return send_record(c, CT_APPLICATION_DATA, (const uint8_t*)data, len) == 0 ? len : -1;
}

int tls13_read(tls13_conn* c, void* buf, int maxlen){
    if (!c || !c->established) return -1;
    /* A TLS 1.3 server sends NewSessionTicket right after the handshake, and
       those arrive as ENCRYPTED HANDSHAKE records.  They are not application
       data: returning them to the caller corrupts the HTTP stream (the read
       loop below skips them instead). */
    for (;;){
        uint8_t type;
        int n = recv_record(c, &type, (uint8_t*)buf, maxlen);
        if (n < 0) return -1;
        if (type == CT_ALERT) return 0;        /* close_notify or fatal */
        if (type == CT_HANDSHAKE) continue;    /* NewSessionTicket / KeyUpdate */
        if (type != CT_APPLICATION_DATA) return 0;
        return n;
    }
}

void tls13_close(tls13_conn* c){
    if (!c || !c->established) return;
    uint8_t alert[2];
    alert[0] = 0x01;   /* warning */
    alert[1] = 0x00;   /* close_notify */
    send_record(c, CT_ALERT, alert, 2);
    c->established = 0;
}

int tls13_http_request(tls13_conn* c, const char* host,
                       const char* method, const char* path,
                       const char* extra_headers,
                       const uint8_t* body, int body_len,
                       char* out, int out_cap){
    char req[2048];
    int n = 0;
    const char* parts[8]; int count = 0;
    parts[count++] = method; parts[count++] = " "; parts[count++] = path;
    parts[count++] = " HTTP/1.1\r\nHost: "; parts[count++] = host;
    parts[count++] = "\r\nConnection: close\r\n";
    for (int i = 0; i < count; i++){
        const char* s = parts[i];
        while (*s && n < (int)sizeof(req) - 1) req[n++] = *s++;
    }
    if (extra_headers){
        const char* s = extra_headers;
        while (*s && n < (int)sizeof(req) - 1) req[n++] = *s++;
    }
    {
        const char* tail = "\r\n";
        while (*tail && n < (int)sizeof(req) - 1) req[n++] = *tail++;
    }
    req[n] = 0;

    if (tls13_write(c, req, n) < 0) return -1;
    if (body && body_len > 0){
        if (tls13_write(c, body, body_len) < 0) return -1;
    }

    int total = 0;
    for (;;){
        int got = tls13_read(c, out + total, out_cap - total - 1);
        if (got <= 0) break;
        total += got;
        if (total >= out_cap - 1) break;
    }
    out[total] = 0;
    return total;
}

#ifdef TLS13_HOST_TEST
int tls13_verify_accept_all(const uint8_t* certs, int certs_len, int count,
                            const char* hostname, const char** out_reason){
    (void)certs; (void)certs_len; (void)count; (void)hostname;
    if (out_reason) *out_reason = "accept_all(test only)";
    return 0;
}
#endif
