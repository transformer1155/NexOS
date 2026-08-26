#include "dcn_discovery.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct dcn_discovery {
    dcn_transport_t* t;
    char    self_name[DCN_MAX_NAME];
    int     self_capacity;
    dcn_node_t nodes[DCN_MAX_NODES];
};

static int find_slot(dcn_discovery_t* d, const uint8_t mac[DCN_MAC_LEN]){
    for (int i = 0; i < DCN_MAX_NODES; i++)
        if (d->nodes[i].present && memcmp(d->nodes[i].mac, mac, DCN_MAC_LEN)==0)
            return i;
    return -1;
}
static int free_slot(dcn_discovery_t* d){
    for (int i = 0; i < DCN_MAX_NODES; i++)
        if (!d->nodes[i].present) return i;
    return -1;
}
static void upsert(dcn_discovery_t* d, const uint8_t mac[DCN_MAC_LEN],
                   const char* name, int capacity, uint32_t now){
    int i = find_slot(d, mac);
    if (i < 0) i = free_slot(d);
    if (i < 0) return;                 /* registry full */
    memcpy(d->nodes[i].mac, mac, DCN_MAC_LEN);
    strncpy(d->nodes[i].name, name[0] ? name : "node", DCN_MAX_NAME-1);
    d->nodes[i].name[DCN_MAX_NAME-1] = 0;
    d->nodes[i].capacity = capacity;
    d->nodes[i].last_seen_ms = now;
    d->nodes[i].present = 1;
}

dcn_discovery_t* dcn_discovery_create(dcn_transport_t* t, const char* self_name, int self_capacity){
    dcn_discovery_t* d = (dcn_discovery_t*)malloc(sizeof(*d));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->t = t;
    strncpy(d->self_name, self_name ? self_name : "self", DCN_MAX_NAME-1);
    d->self_name[DCN_MAX_NAME-1] = 0;
    d->self_capacity = self_capacity;
    return d;
}
void dcn_discovery_destroy(dcn_discovery_t* d){ free(d); }

int dcn_discovery_scan(dcn_discovery_t* d){
    static const uint8_t bcast[DCN_MAC_LEN] = DCN_MAC_BCAST;
    const char* q = "QUERY";
    return dcn_transport_send(d->t, bcast, (const uint8_t*)q, strlen(q));
}
int dcn_discovery_emit_beacon(dcn_discovery_t* d){
    static const uint8_t bcast[DCN_MAC_LEN] = DCN_MAC_BCAST;
    char buf[DCN_MAX_NAME + 32];
    int n = snprintf(buf, sizeof(buf), "BEACON %s %d", d->self_name, d->self_capacity);
    return dcn_transport_send(d->t, bcast, (const uint8_t*)buf, (size_t)n);
}

void dcn_discovery_on_message(dcn_discovery_t* d, const dcn_msg_t* m, uint32_t now){
    char buf[160];
    size_t n = (m->len < sizeof(buf)-1) ? m->len : sizeof(buf)-1;
    memcpy(buf, m->data, n); buf[n] = 0;

    char tag[16]; int capacity = 0; char name[DCN_MAX_NAME] = {0};
    if (sscanf(buf, "%15s %63s %d", tag, name, &capacity) >= 1){
        if (strcmp(tag, "BEACON")==0){
            upsert(d, m->src, name, capacity, now);
        } else if (strcmp(tag, "QUERY")==0){
            /* respond with a REPLY so the querier learns us */
            char rbuf[DCN_MAX_NAME + 32];
            int rn = snprintf(rbuf, sizeof(rbuf), "REPLY %s %d", d->self_name, d->self_capacity);
            dcn_transport_send(d->t, m->src, (const uint8_t*)rbuf, (size_t)rn);
        } else if (strcmp(tag, "REPLY")==0){
            upsert(d, m->src, name, capacity, now);
        }
    }
}

void dcn_discovery_tick(dcn_discovery_t* d, uint32_t now){
    for (int i = 0; i < DCN_MAX_NODES; i++){
        if (!d->nodes[i].present) continue;
        uint32_t age = (now >= d->nodes[i].last_seen_ms) ? (now - d->nodes[i].last_seen_ms) : 0;
        if (age > DCN_DISCOVERY_TIMEOUT_MS) d->nodes[i].present = 0;
    }
}

int dcn_discovery_count(dcn_discovery_t* d){
    int c = 0;
    for (int i = 0; i < DCN_MAX_NODES; i++) if (d->nodes[i].present) c++;
    return c;
}
int dcn_discovery_list(dcn_discovery_t* d, dcn_node_t* out, int maxout){
    int c = 0;
    for (int i = 0; i < DCN_MAX_NODES && c < maxout; i++)
        if (d->nodes[i].present) out[c++] = d->nodes[i];
    return c;
}
int dcn_discovery_pick(dcn_discovery_t* d, int* seed){
    int first = -1;
    for (int i = 0; i < DCN_MAX_NODES; i++) if (d->nodes[i].present){ first = i; break; }
    if (first < 0) return -1;
    int start = (*seed) % DCN_MAX_NODES;
    for (int k = 0; k < DCN_MAX_NODES; k++){
        int i = (start + k) % DCN_MAX_NODES;
        if (d->nodes[i].present){ *seed = i + 1; return i; }
    }
    return -1;
}
