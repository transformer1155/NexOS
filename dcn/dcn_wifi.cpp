#include "dcn_wifi.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* =========================================================================
 * EAPOL-Key frame builder / parser (fixed layout, MIC at offset 81)
 * ========================================================================= */
#define KI_M1 0x0026
#define KI_M2 0x0046
#define KI_M3 0x04F6
#define KI_M4 0x00C6

static size_t eapol_build(uint8_t* buf, uint16_t keyinfo, const uint8_t* nonce,
                           const uint8_t* keydata, uint16_t kdlen, uint64_t replay,
                           const uint8_t* mic){
    buf[0] = 1; buf[1] = 3;                      /* version, type=EAPOL-Key */
    uint16_t bodylen = (uint16_t)(95 + kdlen);
    dcn_wr16be(buf + 2, bodylen);
    buf[4] = 2;                                 /* descriptor type = RSN */
    dcn_wr16be(buf + 5, keyinfo);
    dcn_wr16be(buf + 7, 0);                     /* key length */
    dcn_wr32be(buf + 9,  (uint32_t)(replay >> 32));
    dcn_wr32be(buf + 13, (uint32_t) replay);
    if (nonce) memcpy(buf + 17, nonce, 32); else memset(buf + 17, 0, 32);
    memset(buf + 49, 0, 16);                    /* IV */
    memset(buf + 65, 0, 8);                     /* RSC */
    memset(buf + 73, 0, 8);                     /* Key ID */
    memset(buf + 81, 0, 16);                    /* MIC (zeroed until computed) */
    dcn_wr16be(buf + 97, kdlen);
    if (kdlen && keydata) memcpy(buf + 99, keydata, kdlen);
    size_t total = 99 + kdlen;
    if (mic) memcpy(buf + 81, mic, 16);
    return total;
}
static void eapol_parse(const uint8_t* buf, size_t len, uint16_t* keyinfo,
                        uint8_t* nonce, uint16_t* kdlen, const uint8_t** keydata,
                        uint8_t* mic, uint64_t* replay){
    if (keyinfo) *keyinfo = dcn_rd16be(buf + 5);
    if (nonce)   memcpy(nonce, buf + 17, 32);
    if (kdlen)   *kdlen = dcn_rd16be(buf + 97);
    if (keydata) *keydata = (len >= 99) ? buf + 99 : NULL;
    if (mic)     memcpy(mic, buf + 81, 16);
    if (replay)  *replay = ((uint64_t)dcn_rd32be(buf + 9) << 32) | dcn_rd32be(buf + 13);
}
/* verify the MIC of a received frame against KCK; returns 1 if valid */
static int verify_mic(const uint8_t* frame, size_t len, const uint8_t kck[16]){
    uint8_t tmp[512]; if (len > sizeof(tmp)) return 0;
    memcpy(tmp, frame, len);
    memset(tmp + 81, 0, 16);
    uint8_t calc[16];
    dcn_wpa2_mic(kck, tmp, len, calc);
    uint8_t embedded[16]; memcpy(embedded, frame + 81, 16);
    return dcn_memeq(calc, embedded, 16);
}

static void gen_nonce(const uint8_t* seed, size_t slen, uint8_t out[32]){
    uint8_t h[20];
    dcn_hmac_sha1(seed, slen, (const uint8_t*)"A-NONCE", 7, h);
    memcpy(out, h, 20);
    dcn_hmac_sha1(seed, slen, (const uint8_t*)"A-NONCE-2", 9, h);
    memcpy(out + 20, h, 12);
}
static void gen_snonce(const uint8_t* seed, size_t slen, uint8_t out[32]){
    uint8_t h[20];
    dcn_hmac_sha1(seed, slen, (const uint8_t*)"S-NONCE", 7, h);
    memcpy(out, h, 20);
    dcn_hmac_sha1(seed, slen, (const uint8_t*)"S-NONCE-2", 9, h);
    memcpy(out + 20, h, 12);
}

static const uint8_t RSN_IE[22] = {
    0x30,0x14,0x01,0x00,0x00,0x0f,0xac,0x04,0x01,0x00,0x00,0x0f,0xac,0x04,
    0x01,0x00,0x00,0x0f,0xac,0x02,0x00,0x00 };

/* =========================================================================
 * Supplicant
 * ========================================================================= */
void dcn_wpa2_supplicant_init(dcn_wpa2_supplicant_t* s, const uint8_t spa[DCN_MAC_LEN],
                              const uint8_t aa[DCN_MAC_LEN], const char* ssid, const char* pass){
    memset(s, 0, sizeof(*s));
    memcpy(s->spa, spa, 6); memcpy(s->aa, aa, 6);
    size_t sl = ssid ? strlen(ssid) : 0; if (sl > DCN_SSID_MAX) sl = DCN_SSID_MAX;
    dcn_pbkdf2_hmac_sha1((const uint8_t*)pass, pass ? strlen(pass) : 0,
                         (const uint8_t*)ssid, sl, 4096, s->pmk, 32);
    gen_snonce(s->pmk, 32, s->snonce);
}
int dcn_wpa2_on_m1(dcn_wpa2_supplicant_t* s, const uint8_t* anonce, uint8_t* out){
    memcpy(s->anonce, anonce, 32);
    dcn_wpa2_derive_ptk(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->ptk);
    s->have_ptk = 1;
    uint8_t mic[16];
    uint8_t frame[256];
    size_t n = eapol_build(frame, KI_M2, s->snonce, RSN_IE, sizeof(RSN_IE), 1, NULL);
    dcn_wpa2_mic(s->ptk, frame, n, mic);                 /* KCK = ptk[0:16] */
    n = eapol_build(frame, KI_M2, s->snonce, RSN_IE, sizeof(RSN_IE), 1, mic);
    memcpy(out, frame, n);
    return (int)n;
}
int dcn_wpa2_on_m3(dcn_wpa2_supplicant_t* s, const uint8_t* m3, size_t m3len, uint8_t* out){
    uint8_t mic[16], kck[16];
    memcpy(kck, s->ptk, 16);
    if (!verify_mic(m3, m3len, kck)) return 0;           /* AP does not know PMK */
    /* re-derive from M3's ANonce (already known) */
    uint16_t kdlen; const uint8_t* kd;
    eapol_parse(m3, m3len, NULL, s->anonce, &kdlen, &kd, mic, NULL);
    dcn_wpa2_derive_ptk(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->ptk);
    /* unwrap GTK with KEK = ptk[16:32] */
    if (kd && kdlen == 40){
        if (dcn_aes_key_unwrap(s->ptk + 16, kd, 40, s->gtk) != 0) return 0;
    }
    uint8_t fmic[16];
    uint8_t frame[256];
    size_t n = eapol_build(frame, KI_M4, s->snonce, NULL, 0, 1, NULL);
    dcn_wpa2_mic(s->ptk, frame, n, fmic);
    n = eapol_build(frame, KI_M4, s->snonce, NULL, 0, 1, fmic);
    memcpy(out, frame, n);
    return (int)n;
}

/* =========================================================================
 * Authenticator
 * ========================================================================= */
void dcn_wpa2_authenticator_init(dcn_wpa2_authenticator_t* a, const uint8_t aa[DCN_MAC_LEN],
                                 const uint8_t spa[DCN_MAC_LEN], const uint8_t pmk[DCN_PMK_LEN],
                                 const uint8_t gtk[DCN_GTK_LEN]){
    memset(a, 0, sizeof(*a));
    memcpy(a->aa, aa, 6); memcpy(a->spa, spa, 6);
    memcpy(a->pmk, pmk, 32); memcpy(a->gtk, gtk, 32);
    gen_nonce(a->pmk, 32, a->anonce);
    a->replay = 0;
}
int dcn_wpa2_auth_m1(dcn_wpa2_authenticator_t* a, uint8_t* out){
    a->replay++;
    uint8_t frame[256];
    size_t n = eapol_build(frame, KI_M1, a->anonce, NULL, 0, a->replay, NULL);
    memcpy(out, frame, n);
    return (int)n;
}
int dcn_wpa2_auth_on_m2(dcn_wpa2_authenticator_t* a, const uint8_t* m2, size_t m2len, uint8_t* out){
    uint8_t snonce[32], mic[16], kck[16];
    eapol_parse(m2, m2len, NULL, snonce, NULL, NULL, mic, NULL);
    memcpy(a->snonce, snonce, 32);
    dcn_wpa2_derive_ptk(a->pmk, a->aa, a->spa, a->anonce, a->snonce, a->ptk);
    memcpy(kck, a->ptk, 16);
    if (!verify_mic(m2, m2len, kck)) return 0;           /* wrong password */
    /* wrap GTK with KEK = ptk[16:32] */
    uint8_t wrapped[40];
    if (dcn_aes_key_wrap(a->ptk + 16, a->gtk, 32, wrapped) != 0) return 0;
    a->replay++;
    uint8_t fmic[16];
    uint8_t frame[256];
    size_t n = eapol_build(frame, KI_M3, a->anonce, wrapped, 40, a->replay, NULL);
    dcn_wpa2_mic(a->ptk, frame, n, fmic);
    n = eapol_build(frame, KI_M3, a->anonce, wrapped, 40, a->replay, fmic);
    memcpy(out, frame, n);
    return (int)n;
}
int dcn_wpa2_auth_on_m4(dcn_wpa2_authenticator_t* a, const uint8_t* m4, size_t m4len){
    uint8_t kck[16]; memcpy(kck, a->ptk, 16);
    return verify_mic(m4, m4len, kck) ? 1 : 0;
}

/* =========================================================================
 * WiFiManager FSM + mock AP driver
 * ========================================================================= */
struct dcn_wifi {
    dcn_wifi_state state;
    uint8_t self_mac[DCN_MAC_LEN];
    uint8_t ap_mac[DCN_MAC_LEN];
    char    ssid[DCN_SSID_MAX + 1];
    char    pass[DCN_PASS_MAX + 1];
    dcn_wpa2_supplicant_t sup;
};

dcn_wifi_t* dcn_wifi_create(const uint8_t self_mac[DCN_MAC_LEN]){
    dcn_wifi_t* w = (dcn_wifi_t*)malloc(sizeof(*w));
    if (!w) return NULL;
    memset(w, 0, sizeof(*w));
    memcpy(w->self_mac, self_mac, 6);
    w->state = DCN_WIFI_IDLE;
    return w;
}
void dcn_wifi_destroy(dcn_wifi_t* w){ free(w); }
void dcn_wifi_set_ap_mac(dcn_wifi_t* w, const uint8_t aa[DCN_MAC_LEN]){ memcpy(w->ap_mac, aa, 6); }
void dcn_wifi_set_credential(dcn_wifi_t* w, const char* ssid, const char* pass){
    strncpy(w->ssid, ssid ? ssid : "", DCN_SSID_MAX); w->ssid[DCN_SSID_MAX] = 0;
    strncpy(w->pass, pass ? pass : "", DCN_PASS_MAX); w->pass[DCN_PASS_MAX] = 0;
}
dcn_wifi_state dcn_wifi_stateof(dcn_wifi_t* w){ return w->state; }

int dcn_wifi_scan(dcn_wifi_t* w, dcn_wifi_driver_t* drv, dcn_wifi_net_t* out, int max){
    (void)w;
    if (!drv || !drv->scan) return 0;
    return drv->scan(drv->ctx, out, max);
}
int dcn_wifi_connect_sync(dcn_wifi_t* w, dcn_wifi_driver_t* drv){
    w->state = DCN_WIFI_CONNECTING;
    dcn_wpa2_supplicant_init(&w->sup, w->self_mac, w->ap_mac, w->ssid, w->pass);
    w->state = DCN_WIFI_AUTHING;

    uint8_t m1[256]; int n = drv->recv(drv->ctx, m1, sizeof(m1));
    if (n <= 0){ w->state = DCN_WIFI_FAILED; return DCN_ERR; }
    uint8_t anonce[32]; eapol_parse(m1, (size_t)n, NULL, anonce, NULL, NULL, NULL, NULL);

    uint8_t m2[256]; int m2len = dcn_wpa2_on_m1(&w->sup, anonce, m2);
    if (m2len <= 0){ w->state = DCN_WIFI_FAILED; return DCN_ERR; }
    int s2 = drv->send(drv->ctx, m2, (size_t)m2len);
    if (s2 != DCN_OK){ w->state = DCN_WIFI_FAILED; return DCN_ERR; }

    uint8_t m3[256]; int m3len = drv->recv(drv->ctx, m3, sizeof(m3));
    if (m3len <= 0){ w->state = DCN_WIFI_FAILED; return DCN_ERR; }
    uint8_t m4[256]; int m4len = dcn_wpa2_on_m3(&w->sup, m3, (size_t)m3len, m4);
    if (m4len <= 0){ w->state = DCN_WIFI_FAILED; return DCN_ERR_AUTH; }
    if (drv->send(drv->ctx, m4, (size_t)m4len) != DCN_OK){ w->state = DCN_WIFI_FAILED; return DCN_ERR; }

    w->state = DCN_WIFI_ONLINE;
    return DCN_OK;
}
void dcn_wifi_ptk(dcn_wifi_t* w, uint8_t out[DCN_PTK_LEN]){ memcpy(out, w->sup.ptk, DCN_PTK_LEN); }
void dcn_wifi_gtk(dcn_wifi_t* w, uint8_t out[DCN_GTK_LEN]){ memcpy(out, w->sup.gtk, DCN_GTK_LEN); }

/* ---- Mock AP (test helper) ---- */
struct mock_ap {
    dcn_wpa2_authenticator_t auth;
    int     phase;          /* 0:send M1, 1:got M2->M3 queued, 2:done */
    uint8_t pending[512];
    size_t  pending_len;
};
static int mock_scan(void* ctx, dcn_wifi_net_t* out, int max){
    (void)ctx;
    if (max <= 0) return 0;
    out[0].rssi = -40; out[0].secured = 1;
    strncpy(out[0].ssid, "NexOS-AP", DCN_SSID_MAX);
    return 1;
}
static int mock_send(void* ctx, const uint8_t* eapol, size_t len){
    struct mock_ap* m = (struct mock_ap*)ctx;
    if (m->phase == 1){
        m->pending_len = (size_t)dcn_wpa2_auth_on_m2(&m->auth, eapol, len, m->pending);
        if (m->pending_len == 0) return DCN_ERR_AUTH;   /* wrong password */
        m->phase = 2;
        return DCN_OK;
    } else if (m->phase == 2){
        return dcn_wpa2_auth_on_m4(&m->auth, eapol, len) ? DCN_OK : DCN_ERR_AUTH;
    }
    return DCN_ERR;
}
static int mock_recv(void* ctx, uint8_t* buf, size_t cap){
    struct mock_ap* m = (struct mock_ap*)ctx;
    if (m->phase == 0){
        m->pending_len = (size_t)dcn_wpa2_auth_m1(&m->auth, m->pending);
        m->phase = 1;
    }
    if (m->pending_len > 0 && m->pending_len <= cap){
        memcpy(buf, m->pending, m->pending_len);
        size_t r = m->pending_len; m->pending_len = 0;
        return (int)r;
    }
    return 0;
}
dcn_wifi_driver_t* dcn_wifi_mock_ap_create(const uint8_t aa[DCN_MAC_LEN],
                                           const uint8_t spa[DCN_MAC_LEN],
                                           const uint8_t pmk[DCN_PMK_LEN],
                                           const uint8_t gtk[DCN_GTK_LEN]){
    struct mock_ap* m = (struct mock_ap*)malloc(sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    dcn_wpa2_authenticator_init(&m->auth, aa, spa, pmk, gtk);
    dcn_wifi_driver_t* drv = (dcn_wifi_driver_t*)malloc(sizeof(*drv));
    if (!drv){ free(m); return NULL; }
    drv->ctx = m; drv->scan = mock_scan; drv->send = mock_send; drv->recv = mock_recv;
    return drv;
}
void dcn_wifi_mock_ap_destroy(dcn_wifi_driver_t* drv){
    if (!drv) return;
    free(drv->ctx); free(drv);
}
