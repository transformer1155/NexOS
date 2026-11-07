/* net_tls.cpp -- kernel TLS transport glue (see net_tls.h). */

#include "net_tls.h"
#include "tls13.h"
#include "tls_x509.h"
#include "tls_crypto.h"

extern "C" int net_tcp_open(const char* host, int port);
extern "C" int net_tcp_write(const uint8_t* data, int len);
extern "C" int net_tcp_read(uint8_t* buf, int maxlen);

static int io_send(void* ctx, const uint8_t* b, int n){
    (void)ctx; return net_tcp_write(b, n);
}
static int io_recv(void* ctx, uint8_t* b, int n){
    (void)ctx; return net_tcp_read(b, n);
}

/* ---- trust anchors ------------------------------------------------- */
#define MAX_ANCHORS 8
static uint8_t s_anchors[MAX_ANCHORS][32];
static int     s_nanchors = 0;

/* Factory preset: DigiCert Global Root G2
 * (SHA-256 of its DER; this is the root that terminates api.deepseek.com's
 * chain: leaf -> TrustAsia DV TLS RSA CA 2025 -> DigiCert Global Root G2).
 * Replaced/updated through net_tls_add_anchor() at runtime; keeping it here
 * means a fresh install can talk to a real CA without any setup. */
static const uint8_t DIGICERT_GLOBAL_ROOT_G2[32] = {
    0xcb,0x3c,0xcb,0xb7,0x60,0x31,0xe5,0xe0,
    0x13,0x8f,0x8d,0xd3,0x9a,0x23,0xf9,0xde,
    0x47,0xff,0xc3,0x5e,0x43,0xc1,0x14,0x4c,
    0xea,0x27,0xd4,0x6a,0x5a,0xb1,0xcb,0x5f
};

static int anchors_ready = 0;
static void anchors_init(void){
    if (anchors_ready) return;
    anchors_ready = 1;
    for (int i = 0; i < 32; i++) s_anchors[0][i] = DIGICERT_GLOBAL_ROOT_G2[i];
    s_nanchors = 1;
}

int net_tls_add_anchor(const uint8_t fp[32]){
    anchors_init();
    if (!fp) return -1;
    /* de-duplicate so repeated setup is harmless */
    for (int i = 0; i < s_nanchors; i++){
        int same = 1;
        for (int k = 0; k < 32; k++) if (s_anchors[i][k] != fp[k]){ same = 0; break; }
        if (same) return 0;
    }
    if (s_nanchors >= MAX_ANCHORS) return -1;
    for (int k = 0; k < 32; k++) s_anchors[s_nanchors][k] = fp[k];
    s_nanchors++;
    return 0;
}
int net_tls_anchor_count(void){ anchors_init(); return s_nanchors; }

/* ---- error reporting ------------------------------------------------ */
static char s_err[96];
const char* net_tls_last_error(void){ return s_err; }
static void set_err(const char* m){
    int i = 0;
    if (m) while (m[i] && i < (int)sizeof(s_err) - 1){ s_err[i] = m[i]; i++; }
    s_err[i] = 0;
}

/* ---- entropy -------------------------------------------------------- */
/* The kernel has no /dev/urandom.  Mix the TSC (which varies per handshake on
 * real hardware and across boots under emulation) with a counter and a stack
 * address so two sessions never share a client random.  Documented, and good
 * enough for TLS client randoms -- not for long-term key generation. */
static uint64_t s_mix = 0x9E3779B97F4A7C15ULL;

extern "C" uint64_t tls_rdtsc64(void){
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void seed_entropy_once(void){
    static int done = 0;
    if (done) return;
    done = 1;
    uint8_t seed[32];
    uint64_t t = tls_rdtsc64();
    uint32_t sp = (uint32_t)(uintptr_t)&seed;
    for (int i = 0; i < 4; i++){
        s_mix ^= t + (uint64_t)sp * (uint64_t)(i + 1);
        s_mix *= 0x100000001B3ULL;
        uint32_t v = (uint32_t)(s_mix ^ (s_mix >> 32));
        seed[4*i+0] = (uint8_t)v; seed[4*i+1] = (uint8_t)(v >> 8);
        seed[4*i+2] = (uint8_t)(v >> 16); seed[4*i+3] = (uint8_t)(v >> 24);
    }
    tls_crypto_seed(seed, 32);
}

/* ---- public API ----------------------------------------------------- */
int net_https_post(const char* host, const char* path,
                   const char* extra_headers,
                   const char* body, char* out, int out_cap){
    if (!host || !path || !out || out_cap <= 0) return -1;
    anchors_init();
    seed_entropy_once();

    if (s_nanchors <= 0){ set_err("no trust anchors"); return -1; }
    if (net_tcp_open(host, 443) != 0){ set_err("tcp connect failed"); return -1; }

    static tls13_conn conn;
    conn.io.send = io_send;
    conn.io.recv = io_recv;
    conn.io.ctx  = 0;

    int rc = tls13_connect(&conn, host, (const uint8_t*)s_anchors, s_nanchors);
    if (rc != 0){ set_err(conn.error); return -1; }

    int blen = 0;
    if (body) while (body[blen]) blen++;

    char hdr[256];
    int hn = 0;
    if (blen > 0){
        const char* c0 = "Content-Type: application/json\r\n";
        for (int i = 0; c0[i] && hn < (int)sizeof(hdr) - 1; i++) hdr[hn++] = c0[i];
        char num[12]; int nl = 0; int v = blen;
        if (v == 0) num[nl++] = '0';
        else { char t[12]; int k = 0; while (v){ t[k++] = (char)('0' + v % 10); v /= 10; }
               while (k) num[nl++] = t[--k]; }
        const char* c1 = "Content-Length: ";
        for (int i = 0; c1[i] && hn < (int)sizeof(hdr) - 1; i++) hdr[hn++] = c1[i];
        for (int i = 0; i < nl; i++) hdr[hn++] = num[i];
        hdr[hn++] = '\r'; hdr[hn++] = '\n';
    }
    if (extra_headers){
        for (int i = 0; extra_headers[i] && hn < (int)sizeof(hdr) - 1; i++)
            hdr[hn++] = extra_headers[i];
    }
    hdr[hn] = 0;

    int n = tls13_http_request(&conn, host, "POST", path, hdr,
                               (const uint8_t*)body, blen, out, out_cap);
    tls13_close(&conn);
    if (n <= 0){ set_err("http request failed"); return -1; }
    set_err("ok");
    return n;
}
