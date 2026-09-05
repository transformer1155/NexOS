#include "dcn_sched.h"
#include "dcn_op.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Worker
 * ========================================================================= */
struct dcn_worker {
    dcn_transport_t* t;
    int processed;
    int max;
};

dcn_worker_t* dcn_worker_create(dcn_transport_t* t){
    dcn_worker_t* w = (dcn_worker_t*)malloc(sizeof(*w));
    if (!w) return NULL;
    w->t = t;
    w->processed = 0;
    w->max = 0;
    return w;
}
void dcn_worker_destroy(dcn_worker_t* w){ free(w); }

/* Handle one inbound datagram. Returns 1 when the worker should stop
 * draining (max reached), 0 to keep going. */
static int worker_cb(void* u, dcn_msg_t* m){
    dcn_worker_t* w = (dcn_worker_t*)u;
    if (w->max > 0 && w->processed >= w->max) return 1;

    /* only TASK datagrams are ours; discovery beacons etc. are ignored */
    if (m->len < 10 || m->data[0] != DCN_PKT_TASK) return 0;

    uint8_t  op = m->data[1];
    uint32_t block_id = dcn_rd32be(m->data + 2);
    uint32_t plen     = dcn_rd32be(m->data + 6);
    if (plen > m->len - 10) return 0;                 /* malformed */

    uint8_t out[4096];
    size_t  outlen = 0;
    if (dcn_op_run(op, m->data + 10, plen, out, &outlen) != DCN_OK)
        return 0;                                     /* unknown op -> drop */

    /* reply RESULT to the TASK's sender */
    uint8_t* rb = (uint8_t*)malloc(9 + outlen);
    if (!rb) return 0;
    rb[0] = DCN_PKT_RESULT;
    dcn_wr32be(rb + 1, block_id);
    dcn_wr32be(rb + 5, (uint32_t)outlen);
    memcpy(rb + 9, out, outlen);
    dcn_transport_send(w->t, m->src, rb, 9 + outlen);
    free(rb);

    w->processed++;
    return (w->max > 0 && w->processed >= w->max) ? 1 : 0;
}

int dcn_worker_pump(dcn_worker_t* w, int max){
    if (!w) return 0;
    w->processed = 0;
    w->max = max;
    dcn_transport_pump(w->t, worker_cb, w);
    return w->processed;
}

/* =========================================================================
 * Coordinator
 * ========================================================================= */
int dcn_sched_dispatch(dcn_chunk_plan_t* p, dcn_discovery_t* d,
                       dcn_transport_t* t, uint8_t op){
    if (!p || !d || !t) return DCN_ERR_PARAM;
    dcn_node_t peers[DCN_MAX_NODES];
    int n = dcn_discovery_list(d, peers, DCN_MAX_NODES);
    if (n <= 0) return DCN_ERR_NOPEER;

    int sent = 0;
    int nblocks = dcn_chunk_count(p);
    for (int i = 0; i < nblocks; i++){
        const dcn_block_t* b = dcn_chunk_block(p, i);
        if (!b || b->state != DCN_BLOCK_ASSIGNED) continue;
        if (b->node_idx < 0 || b->node_idx >= n) continue;

        /* TASK: [type][op][block_id:4][len:4][payload] */
        const uint8_t* src = dcn_chunk_block_data(p, i);
        if (!src) continue;
        uint8_t* pkt = (uint8_t*)malloc(10 + b->len);
        if (!pkt) continue;
        pkt[0] = DCN_PKT_TASK;
        pkt[1] = op;
        dcn_wr32be(pkt + 2, b->id);
        dcn_wr32be(pkt + 6, b->len);
        memcpy(pkt + 10, src, b->len);
        dcn_transport_send(t, peers[b->node_idx].mac, pkt, 10 + b->len);
        free(pkt);
        sent++;
    }
    return sent;
}

struct collect_ctx { dcn_chunk_plan_t* p; int collected; };
static int collect_cb(void* u, dcn_msg_t* m){
    struct collect_ctx* c = (struct collect_ctx*)u;
    if (m->len < 9 || m->data[0] != DCN_PKT_RESULT) return 0;
    uint32_t block_id = dcn_rd32be(m->data + 1);
    uint32_t rlen     = dcn_rd32be(m->data + 5);
    if (rlen > m->len - 9) return 0;                  /* malformed */
    if (dcn_chunk_complete(c->p, (int)block_id, m->data + 9, rlen) == DCN_OK)
        c->collected++;
    return 0;
}

int dcn_sched_collect(dcn_chunk_plan_t* p, dcn_transport_t* t){
    if (!p || !t) return 0;
    struct collect_ctx c = { p, 0 };
    dcn_transport_pump(t, collect_cb, &c);
    return c.collected;
}
