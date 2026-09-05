#ifndef DCN_TRANSPORT_H
#define DCN_TRANSPORT_H

#include "dcn_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DCN_MAC_BCAST {0xff,0xff,0xff,0xff,0xff,0xff}

typedef struct dcn_msg {
    uint8_t        src[DCN_MAC_LEN];
    uint8_t        dst[DCN_MAC_LEN];
    const uint8_t* data;
    size_t         len;
} dcn_msg_t;

typedef struct dcn_transport dcn_transport_t;

typedef int  (*dcn_transport_tx_fn)(void* ctx, const dcn_msg_t* m);
typedef int  (*dcn_transport_rx_fn)(void* ctx, dcn_msg_t* out);   /* returns 1 if a msg was filled, 0 if empty */
typedef void (*dcn_transport_destroy_fn)(void* ctx);

typedef struct dcn_transport_ops {
    dcn_transport_tx_fn      tx;
    dcn_transport_rx_fn      rx;
    dcn_transport_destroy_fn destroy;
} dcn_transport_ops_t;

DCN_API dcn_transport_t* dcn_transport_create(const dcn_transport_ops_t* ops, void* ctx,
                                              const uint8_t self_mac[DCN_MAC_LEN]);
DCN_API void             dcn_transport_destroy(dcn_transport_t* t);
DCN_API const uint8_t*   dcn_transport_self(dcn_transport_t* t);

/* Enqueue a datagram (broadcast = DCN_MAC_BCAST). Returns DCN_OK / DCN_ERR_*. */
DCN_API int dcn_transport_send(dcn_transport_t* t, const uint8_t dst[DCN_MAC_LEN],
                               const uint8_t* data, size_t len);

/* Drain all pending inbound messages for this endpoint; cb returns 0 to stop.
   Returns the number of messages delivered (each m->data is freed afterwards). */
DCN_API int dcn_transport_pump(dcn_transport_t* t, int (*cb)(void* u, dcn_msg_t* m), void* u);

/* ---- Loopback (in-process bus) backend : host unit tests only ---- */
DCN_API dcn_transport_t* dcn_transport_loopback_create(const uint8_t self_mac[DCN_MAC_LEN]);
DCN_API void             dcn_transport_loopback_reset(void); /* clear the shared bus */

#ifdef __cplusplus
}
#endif
#endif /* DCN_TRANSPORT_H */
