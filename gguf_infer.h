#ifndef GGUF_INFER_H
#define GGUF_INFER_H
// gguf_infer.h - real transformer inference over GGUF weights (qwen2 / qwen3 /
// llama family).  Weights stay quantised in RAM; matmuls dequantise on the fly.
//
// Only linked into the 64-bit kernel: GB-scale weights need the 4 GiB PMM pool
// and the 1 GiB-page identity map that only long mode provides.

#include <stdint.h>
#include "gguf.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- lifecycle ------------------------------------------------------
// blob must stay resident for the whole session: tensors are referenced in
// place, never copied.  Returns 0 on success, negative on error.
int  qwen_load(const uint8_t* blob, uint64_t size, uint32_t max_ctx);
void qwen_unload(void);
int  qwen_ready(void);
const GGUFModelInfo* qwen_info(void);
const char* qwen_error(void);

// Bytes of scratch/KV memory taken from big_alloc (0 when not loaded).
uint64_t qwen_runtime_bytes(void);

// ---- tokenizer ------------------------------------------------------
// Greedy longest-match over the GGUF vocabulary, GPT-2 byte-level mapping
// applied first, so UTF-8 (incl. Chinese) round-trips correctly.
int  qwen_tokenize(const char* text, int32_t* out, int max_out);
// Decoded UTF-8 text of one token, valid until the next call.
const char* qwen_detokenize(int32_t id);

// ---- inference ------------------------------------------------------
// One decode step.  Returns the logits array (vocab_size floats) or NULL.
const float* qwen_forward(int32_t token, int32_t pos);
// Sample from logits (temp <= 0 => greedy argmax).
int32_t qwen_sample(const float* logits, float temp, float top_p);
void qwen_reset(void);       // clear KV cache / position

// Full prompt -> streamed completion.  emit() receives UTF-8 pieces.
typedef void (*QwenEmitFn)(const char* piece);
int  qwen_generate(const char* prompt, int max_new, float temp, QwenEmitFn emit);

// PRNG seed for sampling (mixed with the TSC by the caller if desired).
void qwen_seed(uint32_t s);

#ifdef __cplusplus
}
#endif
#endif
