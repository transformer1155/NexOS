#include "dcn_op.h"
#include <string.h>

int dcn_op_run(uint8_t op, const uint8_t* in, size_t inlen,
               uint8_t* out, size_t* outlen){
    switch (op){
    case DCN_OP_SUM: {
        uint32_t s = 0;
        for (size_t i = 0; i < inlen; i++) s += in[i];
        dcn_wr32be(out, s);
        *outlen = 4;
        return DCN_OK;
    }
    case DCN_OP_ECHO:
        memcpy(out, in, inlen);
        *outlen = inlen;
        return DCN_OK;
    case DCN_OP_XOR: {
        uint8_t x = 0;
        for (size_t i = 0; i < inlen; i++) x ^= in[i];
        out[0] = x;
        *outlen = 1;
        return DCN_OK;
    }
    default:
        *outlen = 0;
        return DCN_ERR_PARAM;
    }
}
