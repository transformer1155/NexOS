#include "dcn_transport.h"
#include <string.h>
#include <stdlib.h>

/* =========================================================================
 * Generic transport wrapper
 * ========================================================================= */
struct dcn_transport {
    const dcn_transport_ops_t* ops;
    void* ctx;
    uint8_t self[DCN_MAC_LEN];
};

dcn_transport_t* dcn_transport_create(const dcn_transport_ops_t* ops, void* ctx,
                                      const uint8_t self_mac[DCN_MAC_LEN]){
    dcn_transport_t* t = (dcn_transport_t*)malloc(sizeof(*t));
    if (!t) return NULL;
    t->ops = ops; t->ctx = ctx;
    memcpy(t->self, self_mac, DCN_MAC_LEN);
    return t;
}
void dcn_transport_destroy(dcn_transport_t* t){
    if (!t) return;
    if (t->ops && t->ops->destroy) t->ops->destroy(t->ctx);
    free(t);
}
const uint8_t* dcn_transport_self(dcn_transport_t* t){ return t->self; }

int dcn_transport_send(dcn_transport_t* t, const uint8_t dst[DCN_MAC_LEN],
                       const uint8_t* data, size_t len){
    if (!t || !t->ops || !t->ops->tx) return DCN_ERR;
    dcn_msg_t m;
    memcpy(m.src, t->self, DCN_MAC_LEN);
    memcpy(m.dst, dst, DCN_MAC_LEN);
    m.data = data; m.len = len;
    return t->ops->tx(t->ctx, &m);
}

int dcn_transport_pump(dcn_transport_t* t, int (*cb)(void* u, dcn_msg_t* m), void* u){
    if (!t || !t->ops || !t->ops->rx) return 0;
    int n = 0;
    for (;;){
        dcn_msg_t m;
        m.data = NULL; m.len = 0;
        int got = t->ops->rx(t->ctx, &m);
        if (!got) break;
        n++;
        int stop = cb ? cb(u, &m) : 0;
        if (m.data) free((void*)m.data);
        if (stop) break;
    }
    return n;
}

/* =========================================================================
 * Loopback backend — an in-process shared bus simulates the LAN.
 * Broadcast and unicast messages are delivered to every endpoint whose
 * self MAC matches dst (or dst == broadcast). Single-threaded use.
 * ========================================================================= */
struct lb_msg {
    uint8_t dst[DCN_MAC_LEN];
    uint8_t src[DCN_MAC_LEN];
    uint8_t* data;
    size_t   len;
    struct lb_msg* next;
};
/* One inbox queue per endpoint. A broadcast enqueues a copy to every
 * endpoint's inbox (true LAN semantics); a unicast enqueues only to the
 * addressed endpoint. This fixes the old single shared bus, which
 * destructively delivered each broadcast to only the first pumping node. */
struct lb_endpoint {
    uint8_t self[DCN_MAC_LEN];
    struct lb_msg* inbox_head;
    struct lb_msg* inbox_tail;
    struct lb_endpoint* next;
};
static struct lb_endpoint* g_lb_endpoints = NULL;

static struct lb_endpoint* lb_find(const uint8_t mac[DCN_MAC_LEN]){
    for (struct lb_endpoint* e = g_lb_endpoints; e; e = e->next)
        if (memcmp(e->self, mac, DCN_MAC_LEN)==0) return e;
    return NULL;
}
static int lb_enqueue(struct lb_endpoint* e, const dcn_msg_t* m){
    struct lb_msg* node = (struct lb_msg*)malloc(sizeof(*node));
    if (!node) return DCN_ERR_NOMEM;
    node->data = (uint8_t*)malloc(m->len ? m->len : 1);
    if (!node->data){ free(node); return DCN_ERR_NOMEM; }
    memcpy(node->dst, m->dst, DCN_MAC_LEN);
    memcpy(node->src, m->src, DCN_MAC_LEN);
    memcpy(node->data, m->data, m->len);
    node->len = m->len;
    node->next = NULL;
    if (e->inbox_tail) e->inbox_tail->next = node;
    else e->inbox_head = node;
    e->inbox_tail = node;
    return DCN_OK;
}
static void lb_clear_inbox(struct lb_endpoint* e){
    struct lb_msg* p = e->inbox_head;
    while (p){ struct lb_msg* n = p->next; free(p->data); free(p); p = n; }
    e->inbox_head = e->inbox_tail = NULL;
}

static int lb_tx(void* ctx, const dcn_msg_t* m){
    struct lb_endpoint* me = (struct lb_endpoint*)ctx;
    (void)me;
    int is_bcast = (m->dst[0]==0xff && m->dst[1]==0xff && m->dst[2]==0xff &&
                    m->dst[3]==0xff && m->dst[4]==0xff && m->dst[5]==0xff);
    if (is_bcast){
        int delivered = 0;
        for (struct lb_endpoint* e = g_lb_endpoints; e; e = e->next)
            if (lb_enqueue(e, m) == DCN_OK) delivered++;
        return delivered ? DCN_OK : DCN_ERR;
    }
    struct lb_endpoint* e = lb_find(m->dst);
    if (!e) return DCN_ERR;
    return lb_enqueue(e, m);
}

static int lb_rx(void* ctx, dcn_msg_t* out){
    struct lb_endpoint* e = (struct lb_endpoint*)ctx;
    if (!e->inbox_head) return 0;
    struct lb_msg* cur = e->inbox_head;
    e->inbox_head = cur->next;
    if (!e->inbox_head) e->inbox_tail = NULL;
    out->data = cur->data;          /* ownership transferred to caller */
    out->len  = cur->len;
    memcpy(out->src, cur->src, DCN_MAC_LEN);
    memcpy(out->dst, cur->dst, DCN_MAC_LEN);
    free(cur);
    return 1;
}

static void lb_destroy(void* ctx){
    struct lb_endpoint* e = (struct lb_endpoint*)ctx;
    lb_clear_inbox(e);
    struct lb_endpoint** pp = &g_lb_endpoints;
    while (*pp){ if (*pp == e){ *pp = e->next; break; } pp = &(*pp)->next; }
    free(e);
}

dcn_transport_t* dcn_transport_loopback_create(const uint8_t self_mac[DCN_MAC_LEN]){
    static const dcn_transport_ops_t ops = { lb_tx, lb_rx, lb_destroy };
    struct lb_endpoint* e = (struct lb_endpoint*)malloc(sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    memcpy(e->self, self_mac, DCN_MAC_LEN);
    e->next = g_lb_endpoints;
    g_lb_endpoints = e;
    return dcn_transport_create(&ops, e, self_mac);
}

void dcn_transport_loopback_reset(void){
    for (struct lb_endpoint* e = g_lb_endpoints; e; e = e->next)
        lb_clear_inbox(e);
}
