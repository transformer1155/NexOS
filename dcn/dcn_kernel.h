#ifndef DCN_KERNEL_H
#define DCN_KERNEL_H

#include "dcn_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * DCN kernel thin interface.
 *
 * This is the ONLY layer the NexOS kernel talks to. The heavy crypto /
 * protocol logic lives in the standalone library (dcn_crypto / dcn_wifi /
 * dcn_discovery / dcn_chunk). Here we adapt it to kernel primitives.
 *
 * Integration points (to be wired by the kernel build):
 *   - A real "socket" transport backend wrapping the kernel UDP stack
 *     (net.cpp: udp_send / udp_recv on ports 5455/5456/5457, see distnet).
 *   - A real WiFi driver wrapping the kernel's 802.11 / netdev layer
 *     (the dcn_wifi_driver_t send/recv pair).
 * Until those are wired, dcn_kernel uses the in-process loopback backend
 * so the whole stack remains testable on the host.
 * ========================================================================= */

/* Initialize DCN inside the kernel (create the in-kernel node + transport). */
DCN_API int  dcn_kernel_init(void);
DCN_API void dcn_kernel_shutdown(void);

/* Discover LAN compute peers; returns number of peers found, or <0 on error. */
DCN_API int  dcn_kernel_scan(void);

/* Return up to `max` discovered peer MACs (6 bytes each). Returns count. */
DCN_API int  dcn_kernel_peers(uint8_t* out_macs, int max);

/* Connect to a WPA2 network (real 4-way handshake via the wifi driver). */
DCN_API int  dcn_kernel_wifi_connect(const char* ssid, const char* pass);

/* Wire the kernel WiFi driver (real 802.11/netdev layer) and the AP BSSID. */
DCN_API void dcn_kernel_wifi_set_driver(dcn_wifi_driver_t* drv);
DCN_API void dcn_kernel_wifi_set_ap(const uint8_t aa[DCN_MAC_LEN]);

/* Split `data` into blocks, dispatch across discovered peers, merge results.
 * `fn` is the per-block compute kernel (runs locally in this reference impl;
 * on real nodes it would be RPC-dispatched). Returns DCN_OK or DCN_ERR_*. */
typedef void (*dcn_kernel_compute_fn)(const uint8_t* in, size_t inlen,
                                      uint8_t* out, size_t* outlen, void* u);
DCN_API int  dcn_kernel_distribute(const uint8_t* data, size_t len,
                                   size_t block_size,
                                   dcn_kernel_compute_fn fn, void* u,
                                   uint8_t* out, size_t outcap, size_t* outlen);

#ifdef __cplusplus
}
#endif
#endif /* DCN_KERNEL_H */
