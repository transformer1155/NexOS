#include "dcn_chunk.h"
#include <string.h>
#include <stdlib.h>

struct dcn_chunk_plan {
    const uint8_t* data;
    size_t data_len;
    dcn_block_t blocks[DCN_MAX_BLOCKS];
    int n_blocks;
    int rr_seed;
};

dcn_chunk_plan_t* dcn_chunk_create(const uint8_t* data, size_t len){
    dcn_chunk_plan_t* p = (dcn_chunk_plan_t*)malloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->data = data; p->data_len = len;
    return p;
}
void dcn_chunk_destroy(dcn_chunk_plan_t* p){
    if (!p) return;
    for (int i = 0; i < p->n_blocks; i++) if (p->blocks[i].out) free(p->blocks[i].out);
    free(p);
}

static int do_split(dcn_chunk_plan_t* p, size_t block_size){
    if (block_size == 0) return DCN_ERR_PARAM;
    if (p->data_len == 0) return DCN_ERR_PARAM;
    int n = 0;
    for (size_t off = 0; off < p->data_len; off += block_size){
        if (n >= DCN_MAX_BLOCKS) return DCN_ERR_NOMEM;
        size_t len = p->data_len - off; if (len > block_size) len = block_size;
        dcn_block_t* b = &p->blocks[n];
        memset(b, 0, sizeof(*b));
        b->id = (uint32_t)n; b->offset = (uint32_t)off; b->len = (uint32_t)len;
        b->crc = dcn_crc32(p->data + off, len);
        b->state = DCN_BLOCK_PENDING; b->node_idx = -1;
        n++;
    }
    p->n_blocks = n;
    return DCN_OK;
}
int dcn_chunk_split_by_size(dcn_chunk_plan_t* p, size_t block_size){ return do_split(p, block_size); }
int dcn_chunk_split_by_count(dcn_chunk_plan_t* p, int count){
    if (count <= 0) return DCN_ERR_PARAM;
    size_t bs = (p->data_len + count - 1) / (size_t)count;
    if (bs == 0) bs = 1;
    return do_split(p, bs);
}
int dcn_chunk_count(dcn_chunk_plan_t* p){ return p->n_blocks; }
const dcn_block_t* dcn_chunk_block(dcn_chunk_plan_t* p, int i){
    return (i >= 0 && i < p->n_blocks) ? &p->blocks[i] : NULL;
}
const uint8_t* dcn_chunk_block_data(dcn_chunk_plan_t* p, int i){
    const dcn_block_t* b = dcn_chunk_block(p, i);
    return b ? (p->data + b->offset) : NULL;
}

int dcn_chunk_assign(dcn_chunk_plan_t* p, const dcn_node_t* nodes, int n_nodes){
    if (n_nodes <= 0) return DCN_ERR_NOPEER;
    int assigned = 0;
    for (int i = 0; i < p->n_blocks; i++){
        if (p->blocks[i].state != DCN_BLOCK_PENDING) continue;
        int ni = p->rr_seed % n_nodes;
        p->rr_seed++;
        p->blocks[i].node_idx = ni;
        p->blocks[i].state = DCN_BLOCK_ASSIGNED;
        assigned++;
    }
    return assigned;
}
int dcn_chunk_fail(dcn_chunk_plan_t* p, int block_idx){
    if (block_idx < 0 || block_idx >= p->n_blocks) return DCN_ERR_PARAM;
    if (p->blocks[block_idx].out){ free(p->blocks[block_idx].out); p->blocks[block_idx].out = NULL; }
    p->blocks[block_idx].out_len = 0;
    p->blocks[block_idx].state = DCN_BLOCK_PENDING;   /* re-enter scheduling pool */
    p->blocks[block_idx].node_idx = -1;
    return DCN_OK;
}
int dcn_chunk_complete(dcn_chunk_plan_t* p, int block_idx, const uint8_t* result, size_t rlen){
    if (block_idx < 0 || block_idx >= p->n_blocks) return DCN_ERR_PARAM;
    dcn_block_t* b = &p->blocks[block_idx];
    if (b->out) free(b->out);
    b->out = (uint8_t*)malloc(rlen ? rlen : 1);
    if (!b->out) return DCN_ERR_NOMEM;
    memcpy(b->out, result, rlen);
    b->out_len = (uint32_t)rlen;
    b->state = DCN_BLOCK_DONE;
    return DCN_OK;
}
void dcn_chunk_run_local(dcn_chunk_plan_t* p, dcn_compute_fn fn, void* u){
    for (int i = 0; i < p->n_blocks; i++){
        dcn_block_t* b = &p->blocks[i];
        if (b->state != DCN_BLOCK_ASSIGNED) continue;
        size_t outlen = 0;
        uint8_t out[4096];
        fn(p->data + b->offset, b->len, out, &outlen, u);
        dcn_chunk_complete(p, i, out, outlen);
    }
}
int dcn_chunk_all_done(dcn_chunk_plan_t* p){
    for (int i = 0; i < p->n_blocks; i++)
        if (p->blocks[i].state != DCN_BLOCK_DONE) return 0;
    return 1;
}
int dcn_chunk_merge(dcn_chunk_plan_t* p, uint8_t* out, size_t outcap, size_t* outlen){
    size_t total = 0;
    for (int i = 0; i < p->n_blocks; i++){
        if (p->blocks[i].state != DCN_BLOCK_DONE) return DCN_ERR;
        total += p->blocks[i].out_len;
    }
    if (total > outcap) return DCN_ERR_NOMEM;
    size_t pos = 0;
    /* blocks are already in id/offset order */
    for (int i = 0; i < p->n_blocks; i++){
        memcpy(out + pos, p->blocks[i].out, p->blocks[i].out_len);
        pos += p->blocks[i].out_len;
    }
    *outlen = pos;
    return DCN_OK;
}
