#ifndef DCN_H
#define DCN_H

/* Distributed Compute Network (DCN) — top-level umbrella header.
   A hybrid design: a standalone, host-testable library (crypto / transport /
   discovery / chunk / wifi) plus a thin kernel interface (dcn_kernel.h). */

#include "dcn_common.h"
#include "dcn_crypto.h"
#include "dcn_transport.h"
#include "dcn_discovery.h"
#include "dcn_chunk.h"
#include "dcn_op.h"
#include "dcn_sched.h"
#include "dcn_wifi.h"

#endif /* DCN_H */
