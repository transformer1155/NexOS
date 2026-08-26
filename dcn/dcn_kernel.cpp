#include "dcn.h"
#include "dcn_kernel.h"
#include <string.h>
#include <stdlib.h>

/* In-process state for the kernel node. Uses the loopback transport so the
   whole stack is exercisable on the host; the real kernel build swaps in a
   UDP-backed transport and a real WiFi driver at the marked points. */
typedef struct {
    uint8_t            self[DCN_MAC_LEN];
    uint8_t            ap_mac[DCN_MAC_LEN];
    dcn_transport_t*   t;
    dcn_discovery_t*   disc;
    dcn_wifi_driver_t* wdrv;
    uint32_t           now;
} dcn_kernel_state;

static dcn_kernel_state* g_k = NULL;

static int k_on_msg(void* u, dcn_msg_t* m){
    dcn_kernel_state* st = (dcn_kernel_state*)u;
    if (memcmp(m->src, st->self, 6) == 0) return 0;   /* ignore our own frames */
    dcn_discovery_on_message(st->disc, m, st->now);
    return 0;
}

int dcn_kernel_init(void){
    if (g_k) return DCN_OK;
    dcn_transport_loopback_reset();
    dcn_kernel_state* s = (dcn_kernel_state*)malloc(sizeof(*s));
    if (!s) return DCN_ERR_NOMEM;
    memset(s, 0, sizeof(*s));
    s->self[0]=0x02; s->self[1]=0x00; s->self[2]=0x00; s->self[3]=0x00; s->self[4]=0x00; s->self[5]=0x01;
    s->now = 1000;
    s->t = dcn_transport_loopback_create(s->self);
    s->disc = dcn_discovery_create(s->t, "nexos-core", 8);
    g_k = s;
    return (s->t && s->disc) ? DCN_OK : DCN_ERR;
}
void dcn_kernel_shutdown(void){
    if (!g_k) return;
    dcn_discovery_destroy(g_k->disc);
    dcn_transport_destroy(g_k->t);
    dcn_transport_loopback_reset();
    free(g_k); g_k = NULL;
}

int dcn_kernel_scan(void){
    if (!g_k) return DCN_ERR;
    g_k->now += 1000;
    dcn_discovery_emit_beacon(g_k->disc);
    dcn_discovery_scan(g_k->disc);
    dcn_transport_pump(g_k->t, k_on_msg, g_k);
    dcn_discovery_tick(g_k->disc, g_k->now);
    return dcn_discovery_count(g_k->disc);
}
int dcn_kernel_peers(uint8_t* out_macs, int max){
    if (!g_k) return DCN_ERR;
    dcn_node_t nodes[DCN_MAX_NODES];
    int n = dcn_discovery_list(g_k->disc, nodes, DCN_MAX_NODES);
    int c = 0;
    for (int i = 0; i < n && c < max; i++){
        memcpy(out_macs + c*6, nodes[i].mac, 6);
        c++;
    }
    return c;
}

void dcn_kernel_wifi_set_driver(dcn_wifi_driver_t* drv){ if (g_k) g_k->wdrv = drv; }
void dcn_kernel_wifi_set_ap(const uint8_t aa[DCN_MAC_LEN]){ if (g_k) memcpy(g_k->ap_mac, aa, 6); }

int dcn_kernel_wifi_connect(const char* ssid, const char* pass){
    if (!g_k || !g_k->wdrv) return DCN_ERR;
    dcn_wifi_t* w = dcn_wifi_create(g_k->self);
    if (!w) return DCN_ERR_NOMEM;
    dcn_wifi_set_ap_mac(w, g_k->ap_mac);
    dcn_wifi_set_credential(w, ssid, pass);
    int rc = dcn_wifi_connect_sync(w, g_k->wdrv);
    dcn_wifi_destroy(w);
    return rc;
}

int dcn_kernel_distribute(const uint8_t* data, size_t len, size_t block_size,
                          dcn_kernel_compute_fn fn, void* u,
                          uint8_t* out, size_t outcap, size_t* outlen){
    if (!g_k) return DCN_ERR;
    dcn_chunk_plan_t* p = dcn_chunk_create(data, len);
    if (!p) return DCN_ERR_NOMEM;
    int r = dcn_chunk_split_by_size(p, block_size);
    if (r != DCN_OK){ dcn_chunk_destroy(p); return r; }

    dcn_node_t nodes[DCN_MAX_NODES + 1];
    int n = dcn_discovery_list(g_k->disc, nodes, DCN_MAX_NODES);
    if (n == 0){                                  /* no remote peers -> local worker */
        memset(&nodes[0], 0, sizeof(dcn_node_t));
        memcpy(nodes[0].mac, g_k->self, 6);
        nodes[0].capacity = dcn_chunk_count(p);
        n = 1;
    }
    dcn_chunk_assign(p, nodes, n);
    dcn_chunk_run_local(p, fn, u);
    int rc = dcn_chunk_merge(p, out, outcap, outlen);
    dcn_chunk_destroy(p);
    return rc;
}
