#ifndef GGUF_H
#define GGUF_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// GGML tensor types we support (subset of ggml_type from llama.cpp).
// IMPORTANT: these must match the official GGML type numbers used in GGUF
// files (see llama.cpp ggml_type).  The older custom ordering broke Q8_0
// (was 6, files use 8) and shifted every K-quant, so real-world Qwen/Qwen2
// models failed with "unsupported quantisation".
enum {
    GGUF_T_F32  = 0,
    GGUF_T_F16  = 1,
    GGUF_T_Q4_0 = 2,
    GGUF_T_Q4_1 = 3,
    GGUF_T_Q5_0 = 6,
    GGUF_T_Q5_1 = 7,
    GGUF_T_Q8_0 = 8,
    GGUF_T_Q8_1 = 9,
    GGUF_T_Q2_K = 10,
    GGUF_T_Q3_K = 11,
    GGUF_T_Q4_K = 12,
    GGUF_T_Q5_K = 13,
    GGUF_T_Q6_K = 14,
    GGUF_T_Q8_K = 15,
    GGUF_T_I8   = 23,
    GGUF_T_I16  = 24,
    GGUF_T_I32  = 25,
    GGUF_T_BF16 = 20
};

#define GGUF_MAX_TENSORS 1024

typedef struct {
    char     name[72];
    uint32_t n_dims;
    uint64_t dims[4];
    uint32_t type;       // GGMLTensorType
    uint64_t offset;     // relative to tensor data start
} GGUFTensor;

typedef struct {
    uint32_t version;
    uint64_t tensor_count;
    uint64_t kv_count;
    char     arch[32];        // e.g. "qwen2"
    char     quant[32];       // e.g. "Q4_K_M"
    uint32_t block_count;
    uint32_t embed_length;
    uint32_t head_count;
    uint32_t head_count_kv;
    uint32_t context_length;
    uint32_t feed_forward_length;
    uint32_t vocab_size;
    float    rope_theta;
    float    rms_eps;
    // --- tokenizer ---
    uint64_t tokens_offset;    // file offset of the 1st entry of tokenizer.ggml.tokens
    uint64_t merges_offset;    // file offset of the 1st entry of tokenizer.ggml.merges
    uint64_t merges_count;
    uint32_t bos_id;
    uint32_t eos_id;
    // --- layout ---
    uint64_t tensor_data_offset;  // absolute offset of tensor data in buffer
    uint64_t alignment;
    int      n_tensors;
    GGUFTensor tensors[GGUF_MAX_TENSORS];
} GGUFModelInfo;

// Number of bytes a tensor of `type` with `n_elem` elements occupies.
uint64_t ai_gguf_type_size(uint32_t type, uint64_t n_elem);
// Human readable name of a ggml type ("Q4_K", "F16", ...).
const char* ai_gguf_type_name(uint32_t type);

// Parse a GGUF file already loaded into memory. Returns 0 on success.
int ai_gguf_parse(const uint8_t* data, uint64_t size, GGUFModelInfo* info);

// Find a tensor by exact name; returns pointer into info->tensors or NULL.
const GGUFTensor* ai_gguf_tensor(const GGUFModelInfo* info, const char* name);

#ifdef __cplusplus
}
#endif
#endif
