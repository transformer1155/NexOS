#ifndef DCN_OP_H
#define DCN_OP_H

#include "dcn_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in per-block compute operators. Both the network worker and the
   local test reference dispatch through dcn_op_run, so the distributed
   result is byte-for-byte comparable to a locally computed one. */
#define DCN_OP_SUM  1   /* sum of all input bytes -> 4-byte big-endian */
#define DCN_OP_ECHO 2   /* copy input to output */
#define DCN_OP_XOR  3   /* xor of all input bytes -> 1 byte */

/* Run a built-in op. Returns DCN_OK, or DCN_ERR_PARAM for an unknown op.
   `out` must hold at least 4 bytes for SUM, inlen for ECHO, 1 for XOR. */
DCN_API int dcn_op_run(uint8_t op, const uint8_t* in, size_t inlen,
                       uint8_t* out, size_t* outlen);

#ifdef __cplusplus
}
#endif
#endif /* DCN_OP_H */
