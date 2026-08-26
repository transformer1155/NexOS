#ifndef DCN_SCHED_H
#define DCN_SCHED_H

#include "dcn_common.h"
#include "dcn_transport.h"
#include "dcn_discovery.h"
#include "dcn_chunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Distributed scheduler + worker.
 *
 * Turns the standalone library into a real (if in-process) distributed
 * compute network over the transport backend:
 *   - the COORDINATOR splits work and, for every ASSIGNED block, dispatches a
 *     TASK datagram to the discovered peer that owns the block;
 *   - each WORKER pumps its transport inbox, runs the op on the payload, and
 *     ships a RESULT datagram back to the coordinator;
 *   - the coordinator collects RESULTs and records them into the chunk plan,
 *     then merges — exactly like the local path, but the compute happened on
 *     a "remote" node reached through the LAN transport.
 *
 * Wire format (transport payload):
 *   TASK   : [0]=0x10 [1]=op [2..5]=block_id(BE) [6..9]=len(BE) [10..]=payload
 *   RESULT : [0]=0x11 [1..4]=block_id(BE) [5..8]=len(BE) [9..]=result
 * ========================================================================= */

#define DCN_PKT_TASK   0x10
#define DCN_PKT_RESULT 0x11

/* A worker endpoint: wraps a transport, runs received TASKs, replies RESULT.
 * Generic over the built-in op table (dcn_op_run). Create one per compute
 * node. */
typedef struct dcn_worker dcn_worker_t;

DCN_API dcn_worker_t* dcn_worker_create(dcn_transport_t* t);
DCN_API void          dcn_worker_destroy(dcn_worker_t* w);

/* Process up to `max` inbound TASK packets (max<=0 = drain all). Each TASK
   is computed and a RESULT is sent back to the TASK's sender. Returns the
   number of tasks actually processed. */
DCN_API int dcn_worker_pump(dcn_worker_t* w, int max);

/* ---- Coordinator side (stateless helpers) ---- */

/* Send a TASK for every ASSIGNED block to its assigned peer (peer MAC taken
 * from the discovery list, indexed by the block's node_idx). Returns the
 * number of TASK datagrams sent. */
DCN_API int dcn_sched_dispatch(dcn_chunk_plan_t* p, dcn_discovery_t* d,
                               dcn_transport_t* t, uint8_t op);

/* Pump the coordinator inbox; for each RESULT, record the result into the
 * plan by block_id. Returns the number of results collected. */
DCN_API int dcn_sched_collect(dcn_chunk_plan_t* p, dcn_transport_t* t);

#ifdef __cplusplus
}
#endif
#endif /* DCN_SCHED_H */
