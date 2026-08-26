#ifndef DCN_COMMON_H
#define DCN_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DCN_API
#define DCN_API
#endif

/* ---- Sizes & limits ---- */
#define DCN_MAC_LEN      6
#define DCN_NONCE_LEN    32
#define DCN_SSID_MAX     32
#define DCN_PASS_MAX     63
#define DCN_MAX_NODES    64
#define DCN_MAX_BLOCKS   256
#define DCN_MAX_NAME     64
#define DCN_MIC_LEN      16
#define DCN_PMK_LEN      32
#define DCN_PTK_LEN      64
#define DCN_KCK_LEN      16
#define DCN_KEK_LEN      16
#define DCN_TK_LEN       16
#define DCN_GTK_LEN      32
#define DCN_KEYDATA_MAX  256

/* ---- Result codes ---- */
typedef enum {
    DCN_OK         =  0,
    DCN_ERR        = -1,
    DCN_ERR_NOMEM  = -2,
    DCN_ERR_PARAM  = -3,
    DCN_ERR_TIMEOUT= -4,
    DCN_ERR_AUTH   = -5,
    DCN_ERR_NOPEER = -6,
    DCN_ERR_BUSY   = -7
} dcn_err_t;

/* ---- Endian helpers (network fields are big-endian) ---- */
static inline uint16_t dcn_rd16be(const uint8_t* p){ return (uint16_t)(((uint16_t)p[0]<<8)|p[1]); }
static inline uint32_t dcn_rd32be(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3]; }
static inline void     dcn_wr16be(uint8_t* p, uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static inline void     dcn_wr32be(uint8_t* p, uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static inline uint32_t dcn_rd32le(const uint8_t* p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline void     dcn_wr32le(uint8_t* p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* ---- Constant-time memory compare (MIC / key checks) ---- */
static inline int dcn_memeq(const uint8_t* a, const uint8_t* b, size_t n){
    uint8_t diff = 0;
    for (size_t i=0;i<n;i++) diff |= (uint8_t)(a[i]^b[i]);
    return diff==0;
}

/* ---- CRC32 (IEEE 802.3, used for chunk block checksums) ---- */
DCN_API uint32_t dcn_crc32(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* DCN_COMMON_H */
