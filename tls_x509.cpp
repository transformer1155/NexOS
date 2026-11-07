/* tls_x509.cpp -- X.509 parsing + RSA signature verification.
 * See tls_x509.h for the trust model.
 */

#include "tls_x509.h"
#include "tls_crypto.h"

#define NW 64                       /* 64 x 32 bits = 2048-bit RSA */

static void mem_zero(void* p, int n){ uint8_t* q=(uint8_t*)p; for(int i=0;i<n;i++) q[i]=0; }
static void mem_copy(void* d, const void* s, int n){ uint8_t* a=(uint8_t*)d; const uint8_t* b=(const uint8_t*)s; for(int i=0;i<n;i++) a[i]=b[i]; }
static int mem_eq(const void* a, const void* b, int n){ const uint8_t* x=(const uint8_t*)a; const uint8_t* y=(const uint8_t*)b; for(int i=0;i<n;i++) if(x[i]!=y[i]) return 0; return 1; }

// ---------------------------------------------------------------------
//  DER cursor
// ---------------------------------------------------------------------
typedef struct { const uint8_t* p; int len; int off; } der;

static int der_next(der* d, uint8_t* tag, const uint8_t** val, int* vlen){
    if (d->off + 2 > d->len) return -1;
    const uint8_t* p = d->p + d->off;
    int i = 0;
    uint8_t t = p[i++];
    int l = p[i++];
    if (l & 0x80){
        int nb = l & 0x7f;
        if (nb == 0 || nb > 4 || i + nb > d->len - d->off) return -1;
        l = 0;
        for (int k = 0; k < nb; k++) l = (l << 8) | p[i++];
    }
    if (l < 0 || d->off + i + l > d->len) return -1;
    *tag = t; *val = p + i; *vlen = l;
    d->off += i + l;
    return 0;
}
static int der_enter(der* d, uint8_t want, der* out){
    uint8_t t; const uint8_t* v; int vl;
    if (der_next(d, &t, &v, &vl) != 0) return -1;
    if (t != want) return -1;
    out->p = v; out->len = vl; out->off = 0;
    return 0;
}
static int der_skip(der* d){
    uint8_t t; const uint8_t* v; int vl;
    return der_next(d, &t, &v, &vl);
}

// ---------------------------------------------------------------------
//  Montgomery arithmetic over N-limb moduli (N <= NW)
// ---------------------------------------------------------------------
typedef struct { uint32_t w[NW]; } bn;

/* 2048-bit modular arithmetic.
 *
 * This is the same algorithm net.cpp has used for years to run its SSH-2
 * Diffie-Hellman (group14) exchange, so it is the one piece of big-integer
 * code in the tree that has demonstrably operated on real traffic.  An earlier
 * attempt at a Montgomery ladder here was subtly wrong (it returned 0 for
 * 1^e), and for certificate verification "probably right" is worthless.
 */
static void bn_zero(bn* a){ for (int i = 0; i < NW; i++) a->w[i] = 0; }
static void bn_set_u32(bn* a, uint32_t v){ bn_zero(a); a->w[0] = v; }
static void bn_import(bn* a, const uint8_t* be, int len){
    bn_zero(a);
    for (int i = 0; i < len; i++){
        int bit = (len - 1 - i) * 8;
        a->w[bit >> 5] |= ((uint32_t)be[i]) << (bit & 31);
    }
}
static void bn_export(const bn* a, uint8_t* be, int len){
    for (int i = 0; i < len; i++){
        int bit = (len - 1 - i) * 8;
        be[i] = (uint8_t)(a->w[bit >> 5] >> (bit & 31));
    }
}
static int bn_bitlen(const bn* a){
    for (int i = NW - 1; i >= 0; i--)
        if (a->w[i]){ int b = 0; uint32_t v = a->w[i]; while (v){ b++; v >>= 1; } return i*32 + b; }
    return 0;
}
static int bn_cmp(const bn* a, const bn* b){
    for (int i = NW-1; i >= 0; i--) if (a->w[i] != b->w[i]) return a->w[i] > b->w[i] ? 1 : -1;
    return 0;
}
static void bn_add_mod(bn* r, const bn* a, const bn* b, const bn* m){
    uint64_t carry = 0;
    bn t; bn_zero(&t);
    for (int i = 0; i < NW; i++){
        uint64_t s = (uint64_t)a->w[i] + b->w[i] + carry;
        t.w[i] = (uint32_t)s; carry = s >> 32;
    }
    if (carry || bn_cmp(&t, m) >= 0){
        carry = 1;
        for (int i = 0; i < NW; i++){
            uint64_t s = (uint64_t)t.w[i] - m->w[i] - (1 - carry);
            t.w[i] = (uint32_t)s; carry = (s >> 32) & 1;
        }
    }
    *r = t;
}
static void bn_full_mul(const bn* a, const bn* b, uint32_t out[NW*2]){
    for (int i = 0; i < NW*2; i++) out[i] = 0;
    for (int i = 0; i < NW; i++){
        uint64_t carry = 0;
        for (int j = 0; j < NW; j++){
            uint64_t prod = (uint64_t)a->w[i] * b->w[j] + out[i+j] + carry;
            out[i+j] = (uint32_t)prod; carry = prod >> 32;
        }
        out[i + NW] += (uint32_t)carry;
    }
}
static inline uint32_t udiv64_32(uint64_t n, uint32_t d, uint32_t* rem){
    uint32_t hi = (uint32_t)(n >> 32);
    uint32_t lo = (uint32_t)n;
    uint32_t q, r;
    __asm__ __volatile__("divl %2" : "=a"(q), "=d"(r) : "rm"(d), "a"(lo), "d"(hi));
    if (rem) *rem = r;
    return q;
}
/* r = out (2*NW words) mod m -- Knuth algorithm D. */
static void bn_mod(uint32_t out[NW*2], const bn* m, bn* r){
    const int n = NW;
    int mhi = n - 1;
    while (mhi > 0 && m->w[mhi] == 0) mhi--;
    uint32_t mv = m->w[mhi];
    int norm = 0;
    while (!(mv & 0x80000000u)){ mv <<= 1; norm++; }
    bn mn; bn_zero(&mn);
    {
        uint32_t carry = 0;
        for (int i = 0; i < n; i++){
            uint64_t cur = ((uint64_t)m->w[i] << norm) | carry;
            mn.w[i] = (uint32_t)cur; carry = (uint32_t)(cur >> 32);
        }
    }
    uint32_t carry = 0;
    for (int i = 0; i < 2 * n; i++){
        uint64_t cur = ((uint64_t)out[i] << norm) | carry;
        out[i] = (uint32_t)cur; carry = (uint32_t)(cur >> 32);
    }
    for (int j = 2 * n - 1; j >= n; j--){
        uint64_t num = ((uint64_t)out[j] << 32) | out[j - 1];
        uint32_t rhat32;
        uint32_t qhat = udiv64_32(num, mn.w[n - 1], &rhat32);
        uint64_t rhat = rhat32;
        if (n > 1){
            while (qhat >= 0x100000000ULL ||
                   (uint64_t)qhat * (uint32_t)mn.w[n - 2] > (((rhat << 32) | out[j - 2]))){
                qhat--; rhat += (uint64_t)mn.w[n - 1];
                if (rhat >= 0x100000000ULL) break;
            }
        }
        int64_t borrow = 0; uint64_t mcarry = 0;
        for (int i = 0; i < n; i++){
            uint64_t sub = (uint64_t)qhat * (uint32_t)mn.w[i] + mcarry;
            mcarry = sub >> 32;
            uint64_t diff = (uint64_t)out[j - n + i] - (sub & 0xFFFFFFFFULL) - (borrow & 1);
            out[j - n + i] = (uint32_t)diff;
            borrow = (int64_t)(diff >> 32);
        }
        uint64_t diff = (uint64_t)out[j] - mcarry - (borrow & 1);
        out[j] = (uint32_t)diff;
        if ((int64_t)diff < 0){
            uint32_t c = 0;
            for (int i = 0; i < n; i++){
                uint64_t sum = (uint64_t)out[j - n + i] + (uint64_t)mn.w[i] + c;
                out[j - n + i] = (uint32_t)sum; c = (uint32_t)(sum >> 32);
            }
            out[j] += c;
        }
    }
    carry = 0;
    for (int i = n - 1; i >= 0; i--){
        uint64_t cur = ((uint64_t)carry << 32) | out[i];
        out[i] = (uint32_t)(cur >> norm); carry = (uint32_t)cur;
    }
    bn_zero(r);
    for (int i = 0; i < n; i++) r->w[i] = out[i];
}
static void bn_mul_mod(bn* r, const bn* a, const bn* b, const bn* m){
    uint32_t prod[NW*2];
    bn_full_mul(a, b, prod);
    bn_mod(prod, m, r);
}
static void bn_pow_mod(bn* r, const bn* a, const bn* e, const bn* m){
    bn result; bn_set_u32(&result, 1);
    bn base = *a;
    for (int i = 0; i < NW * 32; i++){
        if ((e->w[i >> 5] >> (i & 31)) & 1) bn_mul_mod(&result, &result, &base, m);
        bn_mul_mod(&base, &base, &base, m);
    }
    *r = result;
}


// ---------------------------------------------------------------------
//  RSA public key (from SubjectPublicKeyInfo)
// ---------------------------------------------------------------------
typedef struct { bn n; uint8_t e[8]; int elen; int k_bytes; } rsa_pub;

static int parse_rsa_spki(const uint8_t* spki, int len, rsa_pub* key){
    der top; top.p = spki; top.len = len; top.off = 0;
    der seq, alg, bits;
    if (der_enter(&top, 0x30, &seq) != 0) return -1;
    if (der_enter(&seq, 0x30, &alg) != 0) return -1;
    {
        static const uint8_t OID[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
        uint8_t t; const uint8_t* v; int vl;
        if (der_next(&alg, &t, &v, &vl) != 0 || t != 0x06) return -1;
        if (vl != (int)sizeof(OID) || !mem_eq(v, OID, vl)) return -1;
    }
    if (der_enter(&seq, 0x03, &bits) != 0) return -1;
    if (bits.len < 2) return -1;
    {
        der inner; inner.p = bits.p + 1; inner.len = bits.len - 1; inner.off = 0;  /* skip unused-bits byte */
        der rsakey;
        if (der_enter(&inner, 0x30, &rsakey) != 0) return -1;
        uint8_t t; const uint8_t* v; int vl;
        if (der_next(&rsakey, &t, &v, &vl) != 0 || t != 0x02) return -1;
        if (vl < 1) return -1;
        /* Strip DER's leading 0x00 sign byte BEFORE the size check: a
           2048-bit modulus is encoded as 257 bytes, and treating that as
           "> 256 bits, reject" made every RSA key parse fail. */
        while (vl > 1 && v[0] == 0x00){ v++; vl--; }
        if (vl > 256) return -1;
        bn_import(&key->n, v, vl);
        /* DER INTEGERs for a positive 2048-bit modulus carry a leading 0x00
           (the high bit would otherwise mean "negative").  Taking vl as the
           modulus length makes every length 257 and breaks PKCS#1 padding
           checks, so derive it from the actual bit length instead. */
        key->k_bytes = (bn_bitlen(&key->n) + 7) / 8;
        if (der_next(&rsakey, &t, &v, &vl) != 0 || t != 0x02) return -1;
        if (vl > 8 || vl < 1) return -1;
        key->elen = vl;
        mem_copy(key->e, v, vl);
    }
    return 0;
}

// ---------------------------------------------------------------------
//  RSA signature verification
// ---------------------------------------------------------------------
static int rsa_public_decrypt(const rsa_pub* key, const uint8_t* sig, int siglen,
                              uint8_t* em, int emlen){
    if (siglen > key->k_bytes) return -1;
    bn s; bn_import(&s, sig, siglen);
    if (bn_cmp(&s, &key->n) >= 0) return -1;      /* signature must be < n */

    bn e; bn_import(&e, key->e, key->elen);
    bn m;
    bn_pow_mod(&m, &s, &e, &key->n);
    bn_export(&m, em, emlen);
    return 0;
}

/* PKCS#1 v1.5 with SHA-256 */
static int rsa_v15_sha256(const rsa_pub* key, const uint8_t* sig, int siglen,
                          const uint8_t hash[32]){
    static const uint8_t DIGEST_INFO[] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,
        0x05,0x00,0x04,0x20
    };
    uint8_t em[300];
    if (key->k_bytes > (int)sizeof(em)) return -1;
    if (rsa_public_decrypt(key, sig, siglen, em, key->k_bytes) != 0) return -1;

    int tlen = (int)sizeof(DIGEST_INFO) + 32;
    int ps = key->k_bytes - tlen - 3;
    if (ps < 8) return -1;
    if (em[0] != 0x00 || em[1] != 0x01) return -1;
    for (int i = 0; i < ps; i++) if (em[2 + i] != 0xff) return -1;
    if (em[2 + ps] != 0x00) return -1;
    if (!mem_eq(em + 3 + ps, DIGEST_INFO, (int)sizeof(DIGEST_INFO))) return -1;
    if (!mem_eq(em + 3 + ps + sizeof(DIGEST_INFO), hash, 32)) return -1;
    return 0;
}

/* MGF1-SHA256 */
static void mgf1_sha256(const uint8_t* seed, int seedlen, uint8_t* out, int outlen){
    uint8_t block[32];
    uint32_t counter = 0;
    int done = 0;
    while (done < outlen){
        sha256_ctx c; sha256_init(&c);
        sha256_update(&c, seed, seedlen);
        uint8_t cb[4] = { (uint8_t)(counter>>24), (uint8_t)(counter>>16),
                          (uint8_t)(counter>>8), (uint8_t)counter };
        sha256_update(&c, cb, 4);
        sha256_final(&c, block);
        int take = outlen - done; if (take > 32) take = 32;
        for (int i = 0; i < take; i++) out[done + i] = block[i];
        done += take; counter++;
    }
}

/* RSASSA-PSS with SHA-256, salt length 32 */
static int rsa_pss_sha256(const rsa_pub* key, const uint8_t* sig, int siglen,
                          const uint8_t hash[32]){
    uint8_t em[300];
    if (key->k_bytes > (int)sizeof(em)) return -1;
    if (rsa_public_decrypt(key, sig, siglen, em, key->k_bytes) != 0) return -1;

    int emlen = key->k_bytes;
    int hlen = 32;
    if (emlen < hlen + 32 + 2) return -1;
    if (em[emlen - 1] != 0xbc) return -1;

    int dblen = emlen - hlen - 1;
    uint8_t db[300];
    for (int i = 0; i < dblen; i++) db[i] = em[i];
    uint8_t mask[300];
    mgf1_sha256(em + dblen, hlen, mask, dblen);
    for (int i = 0; i < dblen; i++) db[i] ^= mask[i];

    int topbits = 8 * emlen - (8 * emlen - 1);   /* bitlength modulus mod 8 */
    (void)topbits;
    db[0] &= (uint8_t)(0xff >> 1);

    /* DB = PS || 0x01 || salt, so PS occupies dblen - 1 - 32 bytes.
       Using -2 here shifted the 0x01 check by one and rejected every valid
       PSS signature (while PKCS#1 v1.5 kept passing, which is what made this
       look like a TLS problem). */
    int pslen = dblen - 32 - 1;
    for (int i = 0; i < pslen; i++) if (db[i] != 0x00) return -1;
    if (db[pslen] != 0x01) return -1;
    const uint8_t* salt = db + dblen - 32;

    sha256_ctx c; sha256_init(&c);
    uint8_t zeros[8]; mem_zero(zeros, 8);
    sha256_update(&c, zeros, 8);
    sha256_update(&c, hash, 32);
    sha256_update(&c, salt, 32);
    uint8_t h2[32]; sha256_final(&c, h2);
    return mem_eq(h2, em + dblen, 32) ? 0 : -1;
}

// ---------------------------------------------------------------------
//  Certificate parsing
// ---------------------------------------------------------------------
typedef struct {
    const uint8_t* tbs; int tbs_len;             /* raw, for signature */
    const uint8_t* sig; int sig_len;             /* BIT STRING content  */
    const uint8_t* sig_oid; int sig_oid_len;
    const uint8_t* spki; int spki_len;
    const uint8_t* san; int san_len;             /* extnValue content or NULL */
    int is_self_issued;
} x509_cert;

static int parse_cert(const uint8_t* der_bytes, int len, x509_cert* out){
    der top; top.p = der_bytes; top.len = len; top.off = 0;
    der cert;
    if (der_enter(&top, 0x30, &cert) != 0) return -1;
    mem_zero(out, sizeof(*out));

    /* tbsCertificate: remember the raw TLV */
    {
        int before = cert.off;
        der tbs;
        if (der_enter(&cert, 0x30, &tbs) != 0) return -1;
        /* Offsets inside the cursor are relative to cert.p (which already
           points past the outer SEQUENCE header), not to der_bytes.  Using
           der_bytes here shifted the signed bytes by the header length and
           every chain signature failed. */
        out->tbs = cert.p + before;
        out->tbs_len = cert.off - before;

        /* walk inside the tbs */
        der t = tbs;
        uint8_t tag; const uint8_t* v; int vl;
        /* optional [0] version */
        if (t.off < t.len && t.p[t.off] == 0xa0) { if (der_skip(&t) != 0) return -1; }
        if (der_skip(&t) != 0) return -1;        /* serialNumber   */
        if (der_skip(&t) != 0) return -1;        /* signature alg  */
        if (der_skip(&t) != 0) return -1;        /* issuer         */
        if (der_skip(&t) != 0) return -1;        /* validity       */
        if (der_skip(&t) != 0) return -1;        /* subject        */
        {
            int before2 = t.off;
            der spki;
            if (der_enter(&t, 0x30, &spki) != 0) return -1;
            out->spki = t.p + before2;
            out->spki_len = t.off - before2;
        }
        /* extensions [3] */
        if (t.off < t.len && t.p[t.off] == 0xa3){
            der exts, seq;
            if (der_enter(&t, 0xa3, &exts) != 0) return -1;
            if (der_enter(&exts, 0x30, &seq) != 0) return -1;
            while (seq.off < seq.len){
                der ext; der e = seq;
                if (der_enter(&e, 0x30, &ext) != 0) break;
                uint8_t et; const uint8_t* ev; int evl;
                if (der_next(&ext, &et, &ev, &evl) != 0 || et != 0x06) break;
                static const uint8_t SAN_OID[] = {0x55,0x1d,0x11};  /* 2.5.29.17 */
                int is_san = (evl == 3 && mem_eq(ev, SAN_OID, 3));
                if (ext.off < ext.len && ext.p[ext.off] == 0x01) der_skip(&ext);  /* critical */
                if (der_next(&ext, &et, &ev, &evl) == 0 && et == 0x04 && is_san){
                    out->san = ev; out->san_len = evl;
                }
                seq.off = e.off;
            }
        }
    }
    /* signatureAlgorithm */
    {
        der alg;
        if (der_enter(&cert, 0x30, &alg) != 0) return -1;
        uint8_t t2; const uint8_t* v; int vl;
        if (der_next(&alg, &t2, &v, &vl) != 0 || t2 != 0x06) return -1;
        out->sig_oid = v; out->sig_oid_len = vl;
    }
    /* signatureValue BIT STRING */
    {
        der bits;
        if (der_enter(&cert, 0x03, &bits) != 0) return -1;
        if (bits.len < 2 || bits.p[0] != 0) return -1;
        out->sig = bits.p + 1;
        out->sig_len = bits.len - 1;
    }
    return 0;
}

static int verify_signed_by(const x509_cert* child, const uint8_t* issuer_der,
                            int issuer_len){
    x509_cert issuer;
    if (parse_cert(issuer_der, issuer_len, &issuer) != 0) return -1;
    rsa_pub key;
    if (parse_rsa_spki(issuer.spki, issuer.spki_len, &key) != 0) return -1;

    uint8_t hash[32];
    sha256(child->tbs, child->tbs_len, hash);

    /* sha256WithRSAEncryption 1.2.840.113549.1.1.11 */
    static const uint8_t OID_V15[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b};
    /* RSASSA-PSS 1.2.840.113549.1.1.10 */
    static const uint8_t OID_PSS[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a};

    if (child->sig_oid_len == (int)sizeof(OID_V15) && mem_eq(child->sig_oid, OID_V15, sizeof(OID_V15)))
        return rsa_v15_sha256(&key, child->sig, child->sig_len, hash);
    if (child->sig_oid_len == (int)sizeof(OID_PSS) && mem_eq(child->sig_oid, OID_PSS, sizeof(OID_PSS)))
        return rsa_pss_sha256(&key, child->sig, child->sig_len, hash);
    return -1;
}

// ---------------------------------------------------------------------
//  hostname matching
// ---------------------------------------------------------------------
static int wildcard_match(const char* pattern, const char* host){
    /* only the leftmost label may be a wildcard, and it matches exactly one label */
    if (pattern[0] != '*') return 0;
    if (pattern[1] != '.') return 0;
    const char* dot = host;
    while (*dot && *dot != '.') dot++;
    if (*dot != '.') return 0;
    const char* p = pattern + 1, *h = dot;
    while (*p && *h){ if (*p != *h) return 0; p++; h++; }
    return (*p == 0 && *h == 0);
}

static int name_match(const char* name, int nlen, const char* host){
    int hlen = 0; while (host[hlen]) hlen++;
    if (nlen != hlen){
        /* wildcard pattern is shorter/longer; allow via match */
        if (nlen == 0 || name[0] != '*') return 0;
    }
    char tmp[256];
    if (nlen >= (int)sizeof(tmp)) return 0;
    mem_copy(tmp, name, nlen); tmp[nlen] = 0;
    if (nlen == hlen && mem_eq(tmp, host, nlen)) return 1;
    return wildcard_match(tmp, host);
}

static int hostname_ok(const x509_cert* cert, const char* hostname){
    if (cert->san && cert->san_len > 0){
        der seq; seq.p = cert->san; seq.len = cert->san_len; seq.off = 0;
        der list;
        if (der_enter(&seq, 0x30, &list) == 0){
            while (list.off < list.len){
                uint8_t t; const uint8_t* v; int vl;
                if (der_next(&list, &t, &v, &vl) != 0) break;
                if (t == 0x82 && name_match((const char*)v, vl, hostname)) return 1;  /* dNSName */
            }
        }
        return 0;   /* SAN present but no match: CN must not override */
    }
    return 0;       /* no SAN: reject (RFC 6125 says CN alone is deprecated) */
}

// ---------------------------------------------------------------------
//  public API
// ---------------------------------------------------------------------
int x509_verify_chain(const uint8_t* certs, int certs_len, int count,
                      const char* hostname, const uint8_t* anchors, int n_anchors,
                      const char** reason){
    if (reason) *reason = "x509:unknown";
    if (!certs || count <= 0){ if (reason) *reason = "x509:no_certs"; return -1; }
    if (!anchors || n_anchors <= 0){ if (reason) *reason = "x509:no_trust_anchors"; return -1; }

    /* split into individual DER blobs (3-byte length prefixes) */
    const uint8_t* derp[8]; int derl[8]; int n = 0;
    int off = 0;
    while (off + 3 <= certs_len && n < 8){
        int clen = ((int)certs[off] << 16) | ((int)certs[off+1] << 8) | certs[off+2];
        off += 3;
        if (clen <= 0 || off + clen > certs_len) break;
        derp[n] = certs + off; derl[n] = clen; n++;
        off += clen;
    }
    if (n == 0){ if (reason) *reason = "x509:bad_list"; return -1; }

    /* leaf hostname */
    x509_cert leaf;
    if (parse_cert(derp[0], derl[0], &leaf) != 0){ if (reason) *reason = "x509:leaf_parse"; return -1; }
    if (!hostname_ok(&leaf, hostname)){ if (reason) *reason = "x509:hostname_mismatch"; return -1; }

    /* chain: each cert must be signed by the next */
    for (int i = 0; i + 1 < n; i++){
        x509_cert child;
        if (parse_cert(derp[i], derl[i], &child) != 0){ if (reason) *reason = "x509:parse"; return -1; }
        if (verify_signed_by(&child, derp[i+1], derl[i+1]) != 0){
            if (reason) *reason = "x509:chain_signature"; return -1;
        }
    }

    /* the top of the chain must be one of the trusted anchors */
    {
        uint8_t fp[32];
        sha256(derp[n-1], derl[n-1], fp);
        for (int a = 0; a < n_anchors; a++)
            if (mem_eq(fp, anchors + a * 32, 32)){ if (reason) *reason = "ok"; return 0; }
    }
    if (reason) *reason = "x509:untrusted_root";
    return -1;
}

int x509_verify_certificate_verify(const uint8_t* leaf_der, int leaf_len,
                                   uint16_t scheme, const uint8_t transcript[32],
                                   const uint8_t* sig, int sig_len){
    x509_cert leaf;
    if (parse_cert(leaf_der, leaf_len, &leaf) != 0) return -11;
    rsa_pub key;
    if (parse_rsa_spki(leaf.spki, leaf.spki_len, &key) != 0) return -12;

    /* The signed content is the ASCII context string followed by the hash:
       TLS 1.3 4.4.3  "64 space bytes" || context || 0x00 || transcript_hash */
    uint8_t msg[64 + 33 + 32];
    int n = 0;
    for (int i = 0; i < 64; i++) msg[n++] = 0x20;
    const char* ctx = "TLS 1.3, server CertificateVerify";
    for (int i = 0; ctx[i]; i++) msg[n++] = (uint8_t)ctx[i];
    msg[n++] = 0x00;

    uint8_t hash[32];
    sha256_ctx c; sha256_init(&c);
    sha256_update(&c, msg, n);
    sha256_update(&c, transcript, 32);
    sha256_final(&c, hash);

    if (scheme == 0x0804) return rsa_pss_sha256(&key, sig, sig_len, hash) == 0 ? 0 : -14;
    if (scheme == 0x0401) return rsa_v15_sha256(&key, sig, sig_len, hash) == 0 ? 0 : -15;
    return -13;   /* unsupported scheme: fail closed rather than skip */
}