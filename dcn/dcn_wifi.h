#ifndef DCN_WIFI_H
#define DCN_WIFI_H

#include "dcn_common.h"
#include "dcn_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DCN_WIFI_IDLE = 0,
    DCN_WIFI_SCANNING,
    DCN_WIFI_CONNECTING,
    DCN_WIFI_AUTHING,
    DCN_WIFI_ONLINE,
    DCN_WIFI_FAILED
} dcn_wifi_state;

typedef struct dcn_wifi_net {
    char ssid[DCN_SSID_MAX + 1];
    int  rssi;
    int  secured;   /* 1 = WPA2-PSK */
} dcn_wifi_net_t;

/* ---- WPA2 handshake primitives (real crypto) ---- */
struct dcn_wpa2_supplicant {
    uint8_t spa[DCN_MAC_LEN];
    uint8_t aa[DCN_MAC_LEN];
    uint8_t pmk[DCN_PMK_LEN];
    uint8_t snonce[DCN_NONCE_LEN];
    uint8_t anonce[DCN_NONCE_LEN];
    uint8_t ptk[DCN_PTK_LEN];
    uint8_t gtk[DCN_GTK_LEN];
    int     have_ptk;
};
typedef struct dcn_wpa2_supplicant dcn_wpa2_supplicant_t;

struct dcn_wpa2_authenticator {
    uint8_t aa[DCN_MAC_LEN];
    uint8_t spa[DCN_MAC_LEN];
    uint8_t pmk[DCN_PMK_LEN];
    uint8_t anonce[DCN_NONCE_LEN];
    uint8_t snonce[DCN_NONCE_LEN];
    uint8_t ptk[DCN_PTK_LEN];
    uint8_t gtk[DCN_GTK_LEN];
    uint64_t replay;
};
typedef struct dcn_wpa2_authenticator dcn_wpa2_authenticator_t;

DCN_API void dcn_wpa2_supplicant_init(dcn_wpa2_supplicant_t* s,
                                      const uint8_t spa[DCN_MAC_LEN],
                                      const uint8_t aa[DCN_MAC_LEN],
                                      const char* ssid, const char* pass);
/* Process M1 (AP -> STA). Returns bytes written to out (M2), or 0. */
DCN_API int  dcn_wpa2_on_m1(dcn_wpa2_supplicant_t* s, const uint8_t* anonce, uint8_t* out);
/* Process M3. Verifies MIC, unwraps GTK, returns bytes written to out (M4), or 0 on auth failure. */
DCN_API int  dcn_wpa2_on_m3(dcn_wpa2_supplicant_t* s, const uint8_t* m3, size_t m3len, uint8_t* out);

DCN_API void dcn_wpa2_authenticator_init(dcn_wpa2_authenticator_t* a,
                                         const uint8_t aa[DCN_MAC_LEN],
                                         const uint8_t spa[DCN_MAC_LEN],
                                         const uint8_t pmk[DCN_PMK_LEN],
                                         const uint8_t gtk[DCN_GTK_LEN]);
DCN_API int  dcn_wpa2_auth_m1(dcn_wpa2_authenticator_t* a, uint8_t* out);                 /* M1 */
DCN_API int  dcn_wpa2_auth_on_m2(dcn_wpa2_authenticator_t* a, const uint8_t* m2, size_t m2len, uint8_t* out); /* M3 or 0 */
DCN_API int  dcn_wpa2_auth_on_m4(dcn_wpa2_authenticator_t* a, const uint8_t* m4, size_t m4len);              /* 1 ok */

/* ---- WiFiManager FSM + driver abstraction ---- */
typedef struct dcn_wifi_driver {
    void* ctx;
    int (*scan)(void* ctx, dcn_wifi_net_t* out, int max);                 /* returns count */
    int (*send)(void* ctx, const uint8_t* eapol, size_t len);            /* send to AP */
    int (*recv)(void* ctx, uint8_t* buf, size_t cap);                    /* recv from AP (bytes) */
} dcn_wifi_driver_t;

typedef struct dcn_wifi dcn_wifi_t;

DCN_API dcn_wifi_t* dcn_wifi_create(const uint8_t self_mac[DCN_MAC_LEN]);
DCN_API void        dcn_wifi_destroy(dcn_wifi_t* w);
DCN_API void        dcn_wifi_set_ap_mac(dcn_wifi_t* w, const uint8_t aa[DCN_MAC_LEN]);
DCN_API void        dcn_wifi_set_credential(dcn_wifi_t* w, const char* ssid, const char* pass);
DCN_API dcn_wifi_state dcn_wifi_stateof(dcn_wifi_t* w);
DCN_API int         dcn_wifi_scan(dcn_wifi_t* w, dcn_wifi_driver_t* drv, dcn_wifi_net_t* out, int max);
/* Runs the full 4-way handshake against the driver (AP). Returns DCN_OK / DCN_ERR_*. */
DCN_API int         dcn_wifi_connect_sync(dcn_wifi_t* w, dcn_wifi_driver_t* drv);

/* Inspect derived keys (for verification) */
DCN_API void        dcn_wifi_ptk(dcn_wifi_t* w, uint8_t out[DCN_PTK_LEN]);
DCN_API void        dcn_wifi_gtk(dcn_wifi_t* w, uint8_t out[DCN_GTK_LEN]);

/* Mock AP driver for host tests (wraps an authenticator). */
DCN_API dcn_wifi_driver_t* dcn_wifi_mock_ap_create(const uint8_t aa[DCN_MAC_LEN],
                                                   const uint8_t spa[DCN_MAC_LEN],
                                                   const uint8_t pmk[DCN_PMK_LEN],
                                                   const uint8_t gtk[DCN_GTK_LEN]);
DCN_API void dcn_wifi_mock_ap_destroy(dcn_wifi_driver_t* drv);

#ifdef __cplusplus
}
#endif
#endif /* DCN_WIFI_H */
