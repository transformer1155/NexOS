#include "dcn_crypto.h"
#include <string.h>

/* =========================================================================
 * CRC32 (IEEE 802.3) — used for chunk block checksums
 * ========================================================================= */
static uint32_t g_crc_table[256];
static int      g_crc_init = 0;

static void crc_build(void){
    for (uint32_t i = 0; i < 256; i++){
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[i] = c;
    }
    g_crc_init = 1;
}
DCN_API uint32_t dcn_crc32(const uint8_t* data, size_t len){
    if (!g_crc_init) crc_build();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = g_crc_table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* =========================================================================
 * SHA-1 (RFC 3174)
 * ========================================================================= */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t buflen; } dcn_sha1_ctx;

static void sha1_block(uint32_t h[5], const uint8_t blk[64]){
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = dcn_rd32be(blk + i*4);
    for (int i = 16; i < 80; i++){
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
    for (int i = 0; i < 80; i++){
        uint32_t f,k;
        if      (i < 20){ f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
        else if (i < 40){ f = b ^ c ^ d;                      k = 0x6ED9EBA1u; }
        else if (i < 60){ f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDCu; }
        else            { f = b ^ c ^ d;                      k = 0xCA62C1D6u; }
        uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}
static void dcn_sha1_init(dcn_sha1_ctx* c){
    c->h[0]=0x67452301u; c->h[1]=0xEFCDAB89u; c->h[2]=0x98BADCFEu; c->h[3]=0x10325476u; c->h[4]=0xC3D2E1F0u;
    c->len=0; c->buflen=0;
}
static void dcn_sha1_update(dcn_sha1_ctx* c, const uint8_t* p, size_t n){
    c->len += n;
    while (n > 0){
        size_t take = 64 - c->buflen; if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64){ sha1_block(c->h, c->buf); c->buflen = 0; }
    }
}
static void dcn_sha1_final(dcn_sha1_ctx* c, uint8_t out[20]){
    uint64_t bitlen = c->len * 8ULL;
    uint8_t pad = 0x80; dcn_sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) dcn_sha1_update(c, &zero, 1);
    uint8_t lb[8]; dcn_wr32be(lb,   (uint32_t)(bitlen >> 32));
                 dcn_wr32be(lb+4, (uint32_t) bitlen);
    dcn_sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) dcn_wr32be(out + i*4, c->h[i]);
}
DCN_API void dcn_sha1(const uint8_t* data, size_t len, uint8_t out[20]){
    dcn_sha1_ctx c; dcn_sha1_init(&c); dcn_sha1_update(&c, data, len); dcn_sha1_final(&c, out);
}

/* =========================================================================
 * MD5 (RFC 1321)
 * ========================================================================= */
typedef struct { uint32_t st[4]; uint64_t len; uint8_t buf[64]; size_t buflen; } dcn_md5_ctx;
static const uint32_t MD5_K[64] = {
0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u};
static const int MD5_S[64] = {
7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21};
static uint32_t md5_rot(uint32_t x,int n){ return (x << n) | (x >> (32-n)); }
static void md5_block(uint32_t st[4], const uint8_t blk[64]){
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3];
    uint32_t m[16];
    for (int i=0;i<16;i++) m[i] = dcn_rd32le(blk + i*4);
    for (int i=0;i<64;i++){
        uint32_t f; int g;
        if (i < 16){ f=(b&c)|((~b)&d); g=i; }
        else if (i < 32){ f=(d&b)|((~d)&c); g=(5*i+1)&15; }
        else if (i < 48){ f=b^c^d; g=(3*i+5)&15; }
        else { f=c^(b|(~d)); g=(7*i)&15; }
        uint32_t tmp = d; d=c; c=b;
        b = b + md5_rot((a+f+MD5_K[i]+m[g]), MD5_S[i]);
        a = tmp;
    }
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d;
}
static void dcn_md5_init(dcn_md5_ctx* c){
    c->st[0]=0x67452301u; c->st[1]=0xefcdab89u; c->st[2]=0x98badcfeu; c->st[3]=0x10325476u;
    c->len=0; c->buflen=0;
}
static void dcn_md5_update(dcn_md5_ctx* c, const uint8_t* p, size_t n){
    c->len += n;
    while (n > 0){
        size_t take = 64 - c->buflen; if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64){ md5_block(c->st, c->buf); c->buflen = 0; }
    }
}
static void dcn_md5_final(dcn_md5_ctx* c, uint8_t out[16]){
    uint64_t bitlen = c->len * 8ULL;
    uint8_t pad = 0x80; dcn_md5_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->buflen != 56) dcn_md5_update(c, &zero, 1);
    uint8_t lb[8];
    lb[0]=(uint8_t)(bitlen&0xff); lb[1]=(uint8_t)((bitlen>>8)&0xff); lb[2]=(uint8_t)((bitlen>>16)&0xff); lb[3]=(uint8_t)((bitlen>>24)&0xff);
    lb[4]=(uint8_t)((bitlen>>32)&0xff); lb[5]=(uint8_t)((bitlen>>40)&0xff); lb[6]=(uint8_t)((bitlen>>48)&0xff); lb[7]=(uint8_t)((bitlen>>56)&0xff);
    dcn_md5_update(c, lb, 8);
    for (int i=0;i<4;i++) dcn_wr32le(out + i*4, c->st[i]);
}
DCN_API void dcn_md5(const uint8_t* data, size_t len, uint8_t out[16]){
    dcn_md5_ctx c; dcn_md5_init(&c); dcn_md5_update(&c, data, len); dcn_md5_final(&c, out);
}

/* =========================================================================
 * HMAC (RFC 2202 / RFC 4231)
 * ========================================================================= */
DCN_API void dcn_hmac_sha1(const uint8_t* key,size_t klen,const uint8_t* msg,size_t mlen,uint8_t out[20]){
    uint8_t k[64]; memset(k,0,sizeof(k));
    if (klen > 64) dcn_sha1(key,klen,k); else memcpy(k,key,klen);
    uint8_t blk[64]; dcn_sha1_ctx c;
    for (int i=0;i<64;i++) blk[i]=k[i]^0x36;
    dcn_sha1_init(&c); dcn_sha1_update(&c,blk,64); dcn_sha1_update(&c,msg,mlen);
    uint8_t inner[20]; dcn_sha1_final(&c,inner);
    for (int i=0;i<64;i++) blk[i]=k[i]^0x5c;
    dcn_sha1_init(&c); dcn_sha1_update(&c,blk,64); dcn_sha1_update(&c,inner,20);
    dcn_sha1_final(&c,out);
}
DCN_API void dcn_hmac_md5(const uint8_t* key,size_t klen,const uint8_t* msg,size_t mlen,uint8_t out[16]){
    uint8_t k[64]; memset(k,0,sizeof(k));
    if (klen > 64) dcn_md5(key,klen,k); else memcpy(k,key,klen);
    uint8_t blk[64]; dcn_md5_ctx c;
    for (int i=0;i<64;i++) blk[i]=k[i]^0x36;
    dcn_md5_init(&c); dcn_md5_update(&c,blk,64); dcn_md5_update(&c,msg,mlen);
    uint8_t inner[16]; dcn_md5_final(&c,inner);
    for (int i=0;i<64;i++) blk[i]=k[i]^0x5c;
    dcn_md5_init(&c); dcn_md5_update(&c,blk,64); dcn_md5_update(&c,inner,16);
    dcn_md5_final(&c,out);
}

/* =========================================================================
 * PBKDF2-HMAC-SHA1 (RFC 6070)
 * ========================================================================= */
DCN_API void dcn_pbkdf2_hmac_sha1(const uint8_t* pw,size_t pwlen,const uint8_t* salt,size_t saltlen,
                                  int iters, uint8_t* out, size_t outlen){
    int blocks = (int)((outlen + 19) / 20);
    uint8_t sb[64];
    if (saltlen > 56) saltlen = 56;
    memcpy(sb, salt, saltlen);
    size_t off = 0;
    for (int b = 1; b <= blocks; b++){
        dcn_wr32be(sb + saltlen, (uint32_t)b);
        uint8_t U[20], T[20];
        dcn_hmac_sha1(pw,pwlen, sb, saltlen+4, T);
        memcpy(U, T, 20);
        for (int i = 2; i <= iters; i++){
            dcn_hmac_sha1(pw,pwlen, U, 20, U);
            for (int j = 0; j < 20; j++) T[j] ^= U[j];
        }
        size_t tc = (outlen - off > 20) ? 20 : (outlen - off);
        memcpy(out + off, T, tc); off += tc;
    }
}

/* =========================================================================
 * WPA2 PRF (IEEE 802.11) — HMAC-SHA1 based
 * ========================================================================= */
DCN_API void dcn_prf_x(const uint8_t* key,size_t klen,
                       const uint8_t* label,size_t labellen,
                       const uint8_t* data, size_t datalen,
                       uint8_t* out, size_t outbits){
    size_t outbytes = outbits / 8;
    uint8_t pre[256];
    if (labellen + 1 + datalen > sizeof(pre)) { memset(out,0,outbytes); return; }
    memcpy(pre, label, labellen); pre[labellen] = 0; memcpy(pre + labellen + 1, data, datalen);
    size_t need = labellen + 1 + datalen;
    uint8_t Z[64]; size_t zlen = 0; int i = 0;
    while (zlen < outbytes){
        uint8_t buf[256];
        memcpy(buf, pre, need); buf[need] = (uint8_t)i;
        uint8_t h[20]; dcn_hmac_sha1(key,klen, buf, need+1, h);
        size_t copy = (outbytes - zlen > 20) ? 20 : (outbytes - zlen);
        memcpy(Z + zlen, h, copy); zlen += copy; i++;
    }
    memcpy(out, Z, outbytes);
}

DCN_API void dcn_wpa2_derive_ptk(const uint8_t pmk[DCN_PMK_LEN],
                                 const uint8_t aa[DCN_MAC_LEN],
                                 const uint8_t spa[DCN_MAC_LEN],
                                 const uint8_t anonce[DCN_NONCE_LEN],
                                 const uint8_t snonce[DCN_NONCE_LEN],
                                 uint8_t ptk[DCN_PTK_LEN]){
    uint8_t data[76];
    const uint8_t* mml = (memcmp(aa,spa,6)   <= 0) ? aa   : spa;
    const uint8_t* mmh = (mml == aa)         ? spa  : aa;
    const uint8_t* mnl = (memcmp(anonce,snonce,32) <= 0) ? anonce : snonce;
    const uint8_t* mnh = (mnl == anonce) ? snonce : anonce; /* max = the other one */
    memcpy(data, mml, 6); memcpy(data+6, mmh, 6);
    memcpy(data+12, mnl, 32); memcpy(data+44, mnh, 32);
    static const char label[] = "Pairwise key expansion";
    dcn_prf_x(pmk, DCN_PMK_LEN, (const uint8_t*)label, sizeof(label)-1, data, 76, ptk, 512);
}

DCN_API void dcn_wpa2_derive_gtk(const uint8_t gmk[DCN_PMK_LEN],
                                 const uint8_t aa[DCN_MAC_LEN],
                                 const uint8_t gnonce[DCN_NONCE_LEN],
                                 uint8_t gtk[DCN_GTK_LEN]){
    uint8_t data[38];
    memcpy(data, aa, 6); memcpy(data+6, gnonce, 32);
    static const char label[] = "Group key expansion";
    dcn_prf_x(gmk, DCN_PMK_LEN, (const uint8_t*)label, sizeof(label)-1, data, 38, gtk, 256);
}

DCN_API void dcn_wpa2_mic(const uint8_t kck[DCN_KCK_LEN], const uint8_t* eapol, size_t len, uint8_t mic[DCN_MIC_LEN]){
    uint8_t h[20]; dcn_hmac_sha1(kck, DCN_KCK_LEN, eapol, len, h);
    memcpy(mic, h, 16);
}

/* =========================================================================
 * AES-128 (FIPS-197)
 * ========================================================================= */
static const uint8_t sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
static const uint8_t rsbox[256] = {
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d};
static const uint8_t Rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static uint8_t gmul(uint8_t a, uint8_t b){
    uint8_t p = 0;
    for (int i = 0; i < 8; i++){
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80; a = (uint8_t)(a << 1);
        if (hi) a ^= 0x1b;
        b = (uint8_t)(b >> 1);
    }
    return p;
}
static void aes_keyexp(const uint8_t* key, uint32_t w[44]){
    for (int i = 0; i < 4; i++) w[i] = dcn_rd32be(key + i*4);
    for (int i = 4; i < 44; i++){
        uint32_t t = w[i-1];
        if (i % 4 == 0){
            t = (t << 8) | (t >> 24);            /* RotWord */
            uint8_t b[4]; dcn_wr32be(b, t);
            b[0]=sbox[b[0]]; b[1]=sbox[b[1]]; b[2]=sbox[b[2]]; b[3]=sbox[b[3]];
            t = dcn_rd32be(b) ^ ((uint32_t)Rcon[i/4 - 1] << 24);
        }
        w[i] = w[i-4] ^ t;
    }
}
static void shift_rows(uint8_t s[16], int enc){
    uint8_t t[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++){
        int sc = enc ? ((c + r) & 3) : ((c - r) & 3);
        t[r + 4*c] = s[r + 4*sc];
    }
    memcpy(s, t, 16);
}
static void mix_columns(uint8_t s[16], int enc){
    for (int c = 0; c < 4; c++){
        uint8_t a0=s[0+4*c],a1=s[1+4*c],a2=s[2+4*c],a3=s[3+4*c];
        if (enc){
            s[0+4*c]=gmul(a0,2)^gmul(a1,3)^a2^a3;
            s[1+4*c]=a0^gmul(a1,2)^gmul(a2,3)^a3;
            s[2+4*c]=a0^a1^gmul(a2,2)^gmul(a3,3);
            s[3+4*c]=gmul(a0,3)^a1^a2^gmul(a3,2);
        } else {
            s[0+4*c]=gmul(a0,14)^gmul(a1,11)^gmul(a2,13)^gmul(a3,9);
            s[1+4*c]=gmul(a0,9)^gmul(a1,14)^gmul(a2,11)^gmul(a3,13);
            s[2+4*c]=gmul(a0,13)^gmul(a1,9)^gmul(a2,14)^gmul(a3,11);
            s[3+4*c]=gmul(a0,11)^gmul(a1,13)^gmul(a2,9)^gmul(a3,14);
        }
    }
}
static void add_roundkey(uint8_t s[16], const uint32_t w[44], int rnd){
    for (int c = 0; c < 4; c++){
        uint32_t k = w[rnd*4 + c];
        for (int r = 0; r < 4; r++) s[r+4*c] ^= (uint8_t)(k >> (24 - 8*r));
    }
}
DCN_API void dcn_aes128_ecb_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]){
    uint32_t w[44]; aes_keyexp(key, w);
    uint8_t s[16]; memcpy(s, in, 16);
    add_roundkey(s, w, 0);
    for (int r = 1; r <= 9; r++){
        for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
        shift_rows(s, 1); mix_columns(s, 1); add_roundkey(s, w, r);
    }
    for (int i = 0; i < 16; i++) s[i] = sbox[s[i]];
    shift_rows(s, 1); add_roundkey(s, w, 10);
    memcpy(out, s, 16);
}
DCN_API void dcn_aes128_ecb_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]){
    uint32_t w[44]; aes_keyexp(key, w);
    uint8_t s[16]; memcpy(s, in, 16);
    add_roundkey(s, w, 10);
    for (int r = 9; r >= 1; r--){
        shift_rows(s, 0);
        for (int i = 0; i < 16; i++) s[i] = rsbox[s[i]];
        add_roundkey(s, w, r); mix_columns(s, 0);
    }
    shift_rows(s, 0);
    for (int i = 0; i < 16; i++) s[i] = rsbox[s[i]];
    add_roundkey(s, w, 0);
    memcpy(out, s, 16);
}

/* =========================================================================
 * RFC 3394 AES Key Wrap (KEK = 16 bytes)
 * ========================================================================= */
DCN_API int dcn_aes_key_wrap(const uint8_t kek[16], const uint8_t* plain, size_t plainlen, uint8_t* cipher){
    if (plainlen < 16 || plainlen % 8 != 0 || plainlen > 256) return -1;
    int n = (int)(plainlen / 8);
    uint8_t A[8]; memset(A, 0xA6, 8);
    uint8_t R[33][8];
    for (int i = 1; i <= n; i++) memcpy(R[i], plain + (i-1)*8, 8);
    uint8_t buf[16], enc[16];
    for (int j = 0; j < 6; j++){
        for (int i = 1; i <= n; i++){
            uint64_t t = (uint64_t)(n*j + i);
            memcpy(buf, A, 8); memcpy(buf+8, R[i], 8);
            dcn_aes128_ecb_encrypt(kek, buf, enc);
            memcpy(A, enc, 8); memcpy(R[i], enc+8, 8);
            /* RFC 3394: A = MSB(64, B) XOR t  — XOR AFTER the encrypt */
            for (int k = 0; k < 8; k++) A[k] ^= (uint8_t)(t >> (8*(7-k)));
        }
    }
    memcpy(cipher, A, 8);
    for (int i = 1; i <= n; i++) memcpy(cipher + 8*i, R[i], 8);
    return 0;
}
DCN_API int dcn_aes_key_unwrap(const uint8_t kek[16], const uint8_t* cipher, size_t cipherlen, uint8_t* plain){
    if (cipherlen < 24 || cipherlen % 8 != 0 || cipherlen > 264) return -1;
    int n = (int)(cipherlen / 8) - 1;
    uint8_t A[8]; memcpy(A, cipher, 8);
    uint8_t R[33][8];
    for (int i = 1; i <= n; i++) memcpy(R[i], cipher + 8*i, 8);
    uint8_t buf[16], dec[16];
    for (int j = 5; j >= 0; j--){
        for (int i = n; i >= 1; i--){
            uint64_t t = (uint64_t)(n*j + i);
            uint8_t tt[8]; memset(tt, 0, 8);
            for (int k = 0; k < 8; k++) tt[k] = (uint8_t)(t >> (8*(7-k)));
            for (int k = 0; k < 8; k++) A[k] ^= tt[k];
            memcpy(buf, A, 8); memcpy(buf+8, R[i], 8);
            dcn_aes128_ecb_decrypt(kek, buf, dec);
            memcpy(A, dec, 8); memcpy(R[i], dec+8, 8);
        }
    }
    for (int k = 0; k < 8; k++) if (A[k] != 0xA6) return -2;
    for (int i = 1; i <= n; i++) memcpy(plain + (i-1)*8, R[i], 8);
    return 0;
}

/* =========================================================================
 * Self-test — RFC / FIPS known-answer vectors + real WPA2 capture
 * ========================================================================= */
DCN_API int dcn_crypto_selftest(void){
    int fail = 0;
    uint8_t out[64];
    #define CK(idx, cond) do{ if(!(cond)){ if(!fail) fail=(idx); } }while(0)

    /* SHA-1 */
    dcn_sha1((const uint8_t*)"abc", 3, out);
    CK(1, dcn_memeq(out,(const uint8_t*)"\xa9\x99\x3e\x36\x47\x06\x81\x6a\xba\x3e\x25\x71\x78\x50\xc2\x6c\x9c\xd0\xd8\x9d",20));
    dcn_sha1((const uint8_t*)"", 0, out);
    CK(2, dcn_memeq(out,(const uint8_t*)"\xda\x39\xa3\xee\x5e\x6b\x4b\x0d\x32\x55\xbf\xef\x95\x60\x18\x90\xaf\xd8\x07\x09",20));

    /* MD5 */
    dcn_md5((const uint8_t*)"abc", 3, out);
    CK(3, dcn_memeq(out,(const uint8_t*)"\x90\x01\x50\x98\x3c\xd2\x4f\xb0\xd6\x96\x3f\x7d\x28\xe1\x7f\x72",16));
    dcn_md5((const uint8_t*)"", 0, out);
    CK(4, dcn_memeq(out,(const uint8_t*)"\xd4\x1d\x8c\xd9\x8f\x00\xb2\x04\xe9\x80\x09\x98\xec\xf8\x42\x7e",16));

    /* HMAC-SHA1 (RFC 4231) */
    { const uint8_t k[20]={0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b}; const uint8_t m[]="Hi There";
      dcn_hmac_sha1(k,20,m,8,out);
      CK(5, dcn_memeq(out,(const uint8_t*)"\xb6\x17\x31\x86\x55\x05\x72\x64\xe2\x8b\xc0\xb6\xfb\x37\x8c\x8e\xf1\x46\xbe\x00",20)); }
    dcn_hmac_sha1((const uint8_t*)"Jefe",4,(const uint8_t*)"what do ya want for nothing?",28,out);
    CK(6, dcn_memeq(out,(const uint8_t*)"\xef\xfc\xdf\x6a\xe5\xeb\x2f\xa2\xd2\x74\x16\xd5\xf1\x84\xdf\x9c\x25\x9a\x7c\x79",20));

    /* HMAC-MD5 (RFC 2202) */
    { const uint8_t k[16]={0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b}; const uint8_t m[]="Hi There";
      dcn_hmac_md5(k,16,m,8,out);
      CK(7, dcn_memeq(out,(const uint8_t*)"\x92\x94\x72\x7a\x36\x38\xbb\x1c\x13\xf4\x8e\xf8\x15\x8b\xfc\x9d",16)); }
    dcn_hmac_md5((const uint8_t*)"Jefe",4,(const uint8_t*)"what do ya want for nothing?",28,out);
    CK(8, dcn_memeq(out,(const uint8_t*)"\x75\x0c\x78\x3e\x6a\xb0\xb5\x03\xea\xa8\x6e\x31\x0a\x5d\xb7\x38",16));

    /* PBKDF2-HMAC-SHA1 (RFC 6070) */
    { const uint8_t pw[]="password"; const uint8_t sa[]="salt";
      dcn_pbkdf2_hmac_sha1(pw,8,sa,4,1,out,16);
      CK(9,  dcn_memeq(out,(const uint8_t*)"\x0c\x60\xc8\x0f\x96\x1f\x0e\x71\xf3\xa9\xb5\x24\xaf\x60\x12\x06\x2f\xe0\x37\xa6",16));
      dcn_pbkdf2_hmac_sha1(pw,8,sa,4,2,out,16);
      CK(10, dcn_memeq(out,(const uint8_t*)"\xea\x6c\x01\x4d\xc7\x2d\x6f\x8c\xcd\x1e\xd9\x2a\xce\x1d\x41\xf0\xd8\xde\x89\x57",16));
      dcn_pbkdf2_hmac_sha1(pw,8,sa,4,4096,out,16);
      CK(11, dcn_memeq(out,(const uint8_t*)"\x4b\x00\x79\x01\xb7\x65\x48\x9a\xbe\xad\x49\xd9\x26\xf7\x21\xd0\x65\xa4\x29\xc1",16)); }

    /* AES-128 ECB (FIPS-197 / NIST SP800-38A) */
    { const uint8_t key[16]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
      const uint8_t pt[16]={0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
      uint8_t ct[16], dec[16];
      dcn_aes128_ecb_encrypt(key,pt,ct);
      CK(12, dcn_memeq(ct,(const uint8_t*)"\x69\xc4\xe0\xd8\x6a\x7b\x04\x30\xd8\xcd\xb7\x80\x70\xb4\xc5\x5a",16));
      dcn_aes128_ecb_decrypt(key,ct,dec);
      CK(13, dcn_memeq(dec,pt,16));
      const uint8_t key2[16]={0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
      const uint8_t pt2[16]={0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a};
      dcn_aes128_ecb_encrypt(key2,pt2,ct);
      CK(14, dcn_memeq(ct,(const uint8_t*)"\x3a\xd7\x7b\xb4\x0d\x7a\x36\x60\xa8\x9e\xca\xf3\x24\x66\xef\x97",16)); }

    /* AES Key Wrap (RFC 3394) */
    { const uint8_t kek[16]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
      const uint8_t kd[16]={0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
      uint8_t cw[24], pw[16];
      dcn_aes_key_wrap(kek,kd,16,cw);
      CK(15, dcn_memeq(cw,(const uint8_t*)"\x1f\xa6\x8b\x0a\x81\x12\xb4\x47\xae\xf3\x4b\xd8\xfb\x5a\x7b\x82\x9d\x3e\x86\x23\x71\xd2\xcf\xe5",24));
      CK(16, dcn_aes_key_unwrap(kek,cw,24,pw)==0 && dcn_memeq(pw,kd,16)); }

    /* WPA2 real-capture MIC (KCK = fb18560e63909f84f31d39da03a5d82f) */
    { const uint8_t kck[16]={0xfb,0x18,0x56,0x0e,0x63,0x90,0x9f,0x84,0xf3,0x1d,0x39,0xda,0x03,0xa5,0xd8,0x2f};
      const char* m2="0103007502010a000000000000000000015214c4dbe4a567e78b8f30b2b016a2d90ea50c27d408614c1fc0a0000000000000000000000000000000000000000000000000000000000000000000000000001630140100000fac020100000fac040100000fac020000";
      const char* m3="020300c70213ca00100000000000000002ac9871c9ca129468708ca0d554e22f4f8b6eaa6dbaa121d2233bf33cbc29d346000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000006814ca01f8d15d931b12c98e973fd6b090abb7dc7277fe31823a2e8a19553946cf6236256850a4428244b990fddcabee637536a199b747b7e1f9eaa0e29878988cade1cbd6214cc6ef1a33dcce40539ba9909da2c891aecba97698bd8e3acd85f4002f581c4240009c";
      const char* m4="0103005f02030a0000000000000000000200000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
      uint8_t fr[512]; size_t L;
      auto hex2bin = [](const char* h, uint8_t* b, size_t n){ for(size_t i=0;i<n;i++){ int hi=(h[2*i]>='a'?h[2*i]-'a'+10:(h[2*i]>='A'?h[2*i]-'A'+10:h[2*i]-'0')); int lo=(h[2*i+1]>='a'?h[2*i+1]-'a'+10:(h[2*i+1]>='A'?h[2*i+1]-'A'+10:h[2*i+1]-'0')); b[i]=(uint8_t)((hi<<4)|lo); } };
      /* M2 */
      L=strlen(m2)/2; hex2bin(m2,fr,L); memset(fr+81,0,16); dcn_wpa2_mic(kck,fr,L,out);
      CK(17, dcn_memeq(out,(const uint8_t*)"\x51\x0d\x01\xc5\xd1\x12\xd2\x3a\xc8\x50\xb6\xc0\xe3\x24\xb8\x98",16));
      /* M3 */
      L=strlen(m3)/2; hex2bin(m3,fr,L); memset(fr+81,0,16); dcn_wpa2_mic(kck,fr,L,out);
      CK(18, dcn_memeq(out,(const uint8_t*)"\x96\x87\x17\x16\x11\xe4\x9a\x46\x11\xc1\x3e\x0f\x3c\x6a\xc7\x9b",16));
      /* M4 */
      L=strlen(m4)/2; hex2bin(m4,fr,L); memset(fr+81,0,16); dcn_wpa2_mic(kck,fr,L,out);
      CK(19, dcn_memeq(out,(const uint8_t*)"\x27\xa9\xb0\x2f\x89\x3e\x6e\xbb\x25\xc5\x9a\x82\xde\xf9\xc0\x84",16)); }

    /* WPA2 PTK min/max ordering symmetry (structural) */
    { const uint8_t pmk[32]={0}; const uint8_t aa[6]={0x00,0x0b,0x85,0x11,0x59,0xac};
      const uint8_t spa[6]={0x00,0x0b,0x85,0x11,0x4c,0xa0};
      const uint8_t an[32]={0x25,0x91}; const uint8_t sn[32]={0x53,0x29};
      uint8_t p1[64], p2[64];
      dcn_wpa2_derive_ptk(pmk,aa,spa,an,sn,p1);
      dcn_wpa2_derive_ptk(pmk,spa,aa,sn,an,p2);
      CK(20, dcn_memeq(p1,p2,64) && !dcn_memeq(p1,pmk,32)); }

    /* CRC32 */
    dcn_crc32((const uint8_t*)"123456789",9);
    CK(21, dcn_crc32((const uint8_t*)"123456789",9)==0xCBF43926u);

    #undef CK
    return fail;
}
