#ifndef DCN_CHUNK_H
#define DCN_CHUNK_H

#include "dcn_common.h"
#include "dcn_discovery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DCN_BLOCK_PENDING = 0,
    DCN_BLOCK_ASSIGNED,
    DCN_BLOCK_DONE,
    DCN_BLOCK_FAILED
} dcn_block_state;

typedef struct dcn_block {
    uint32_t         id;          /* = original block index */
    uint32_t         offset;      /* offset into the source buffer */
    uint32_t         len;         /* bytes of source covered */
    uint32_t         crc;         /* CRC32 of the source bytes (integrity) */
    dcn_block_state  state;
    int              node_idx;    /* node this block was assigned to (-1 = none) */
    uint8_t*         out;         /* computed result (owned by plan) */
    uint32_t         out_len;
} dcn_block_t;

/* user compute kernel: transform in[0..inlen) -> out[0..*outlen) */
typedef void (*dcn_compute_fn)(const uint8_t* in, size_t inlen,
                               uint8_t* out, size_t* outlen, void* u);

typedef struct dcn_chunk_plan dcn_chunk_plan_t;

DCN_API dcn_chunk_plan_t* dcn_chunk_create(const uint8_t* data, size_t len);
DCN_API void dcn_chunk_destroy(dcn_chunk_plan_t* p);

DCN_API int  dcn_chunk_split_by_size(dcn_chunk_plan_t* p, size_t block_size);
DCN_API int  dcn_chunk_split_by_count(dcn_chunk_plan_t* p, int count);
DCN_API int  dcn_chunk_count(dcn_chunk_plan_t* p);
DCN_API const dcn_block_t* dcn_chunk_block(dcn_chunk_plan_t* p, int i);

/* Pointer to the block's own bytes inside the source buffer
   (p->data + block.offset). NULL if the index is out of range. */
DCN_API const uint8_t* dcn_chunk_block_data(dcn_chunk_plan_t* p, int i);

/* Assign all PENDING blocks round-robin across the given node list.
   Returns the number of blocks assigned. */
DCN_API int  dcn_chunk_assign(dcn_chunk_plan_t* p, const dcn_node_t* nodes, int n_nodes);

/* Mark a block as failed -> returns it to PENDING for re-scheduling
   (dynamic scheduling / fault tolerance). */
DCN_API int  dcn_chunk_fail(dcn_chunk_plan_t* p, int block_idx);

/* Record a completed result for a block. */
DCN_API int  dcn_chunk_complete(dcn_chunk_plan_t* p, int block_idx,
                                const uint8_t* result, size_t rlen);

/* Simulate remote nodes computing every ASSIGNED block locally. */
DCN_API void dcn_chunk_run_local(dcn_chunk_plan_t* p, dcn_compute_fn fn, void* u);

DCN_API int  dcn_chunk_all_done(dcn_chunk_plan_t* p);

/* Assemble completed blocks (in id order) into out. Returns DCN_OK or
   DCN_ERR if some blocks are not DONE. */
DCN_API int  dcn_chunk_merge(dcn_chunk_plan_t* p, uint8_t* out, size_t outcap, size_t* outlen);

#ifdef __cplusplus
}
#endif
#endif /* DCN_CHUNK_H */
