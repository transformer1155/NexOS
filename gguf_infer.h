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

// Quantisation control.  mode 1 (default) uses int8-quantised activations
// (W4A8/W8A8 integer dot products); mode 0 falls back to float dots.
void qwen_set_quant_mode(int mode);
int  qwen_quant_mode(void);

// ---- resumable / asynchronous generation -----------------------------
// qwen_gen_begin() tokenises the prompt and arms the first forward.
// qwen_gen_slice(layers, piece, cap) then runs AT MOST `layers` transformer
// layers per call and returns 1 (still running), 0 (finished) or -1 (error).
// `piece` receives any text produced by that slice.  The host GUI calls
// qwen_gen_slice() a few layers at a time from its frame loop so the mouse
// keeps moving while the model decodes.
#define QWEN_GEN_IDLE   0
#define QWEN_GEN_RUN    1
#define QWEN_GEN_DONE   2
#define QWEN_GEN_ERROR  3
int  qwen_gen_begin(const char* prompt, int max_new, float temp);
int  qwen_gen_slice(int layers, char* piece, int piece_cap);
int  qwen_gen_status(void);
int  qwen_gen_produced(void);
void qwen_gen_abort(void);

// Full prompt -> streamed completion.  emit() receives UTF-8 pieces.
typedef void (*QwenEmitFn)(const char* piece);
int  qwen_generate(const char* prompt, int max_new, float temp, QwenEmitFn emit);

// ---- chat (ChatML template) -----------------------------------------
// Wrap an optional system prompt + N (role, content) turns in a ChatML
// template, ending with the <|im_start|>assistant generation prompt.
// Returns the number of characters written (excl. NUL).  roles/contents are
// parallel arrays of length n_turns.
int  qwen_format_chat(const char* system, const char** roles, const char** contents,
                      int n_turns, char* out, int cap);
// End-to-end chat: format system + single user turn, then generate.
int  qwen_chat(const char* system, const char* user, int max_new, float temp, QwenEmitFn emit);

// PRNG seed for sampling (mixed with the TSC by the caller if desired).
void qwen_seed(uint32_t s);

#ifdef __cplusplus
}
#endif
#endif
