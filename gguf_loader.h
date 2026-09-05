// gguf_loader.h -- GGUF (v2/v3) parser over the backend-agnostic gguf_io.
//
// Strict Route A port, phase 0.  This is the new, self-contained parser for
// the parallel ggml subsystem (it does NOT depend on gguf.cpp / gguf_infer.cpp
// and reads exclusively through file_adapter's gguf_io, so it works over both
// the in-RAM and raw-disk backends).  Phases 1-6 build the quant kernels,
// dispatch and transformer ops on top of the gguf_ctx this produces; eventually
// it replaces gguf_infer.cpp's ai_gguf_parse.
#ifndef NEXOS_GGUF_LOADER_H
#define NEXOS_GGUF_LOADER_H

#include <stdint.h>
#include "file_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GGUF_LOADER_MAX_TENSORS 1024

typedef struct {
    char     name[72];
    uint32_t n_dims;
    uint64_t dims[4];
    uint32_t type;       // ggml type number (matches gguf.h / llama.cpp)
    uint64_t offset;     // relative to tensor data start
} gguf_loader_tensor;

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
    uint64_t tokens_offset;    // file offset of tokenizer.ggml.tokens[0]
    uint64_t merges_offset;
    uint64_t merges_count;
    uint32_t bos_id;
    uint32_t eos_id;
    // --- layout ---
    uint64_t tensor_data_offset;  // absolute offset of tensor data
    uint64_t alignment;
    int      n_tensors;
    gguf_loader_tensor tensors[GGUF_LOADER_MAX_TENSORS];
} gguf_ctx;

// Parse a GGUF via the backend-agnostic `io`. Returns 0 on success, <0 on error.
int gguf_load(gguf_io* io, gguf_ctx* ctx);

// Human-readable ggml type name ("Q4_K", "F16", ...).  Used by the self-test.
const char* gguf_loader_type_name(uint32_t type);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_GGUF_LOADER_H
