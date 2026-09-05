#ifndef DCN_DISCOVERY_H
#define DCN_DISCOVERY_H

#include "dcn_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DCN_DISCOVERY_TIMEOUT_MS 5000   /* a node is evicted if unseen this long */
#define DCN_DISCOVERY_PORT        0xDC01 /* protocol tag (informational) */

typedef struct dcn_node {
    uint8_t mac[DCN_MAC_LEN];
    char    name[DCN_MAX_NAME];
    int     capacity;       /* how many work blocks this node can accept */
    uint32_t last_seen_ms;
    int     present;
} dcn_node_t;

typedef struct dcn_discovery dcn_discovery_t;

DCN_API dcn_discovery_t* dcn_discovery_create(dcn_transport_t* t,
                                              const char* self_name, int self_capacity);
DCN_API void dcn_discovery_destroy(dcn_discovery_t* d);

/* Broadcast a scan request. Callers then pump their transport and feed
   received frames to dcn_discovery_on_message. */
DCN_API int dcn_discovery_scan(dcn_discovery_t* d);

/* Periodic heartbeat (broadcast BEACON). Call on a timer. */
DCN_API int dcn_discovery_emit_beacon(dcn_discovery_t* d);

/* Feed an inbound transport message (BEACON / QUERY / REPLY). */
DCN_API void dcn_discovery_on_message(dcn_discovery_t* d, const dcn_msg_t* m, uint32_t now_ms);

/* Evict nodes not seen within DCN_DISCOVERY_TIMEOUT_MS. */
DCN_API void dcn_discovery_tick(dcn_discovery_t* d, uint32_t now_ms);

DCN_API int  dcn_discovery_count(dcn_discovery_t* d);
DCN_API int  dcn_discovery_list(dcn_discovery_t* d, dcn_node_t* out, int maxout);

/* Pick a node for assignment (round-robin over *seed). Returns index into
   the live list, or -1 if none. */
DCN_API int  dcn_discovery_pick(dcn_discovery_t* d, int* seed);

#ifdef __cplusplus
}
#endif
#endif /* DCN_DISCOVERY_H */
