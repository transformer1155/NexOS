// ai_model.h -- open-source model file recognition + registry
//
// Shared between ai_engine.cpp (implementation) and the 32/64-bit kernels
// (command layer).  Pure C-compatible so it can be linked from both.
//
// Reality note for this OS: the on-board inference engine is a tiny
// Markov + mini-GPT (CPU, no external weights).  A real 1.7B model does
// not fit in the available RAM/storage, so `recognize`/`run` identify and
// record the model but inference still uses the built-in engine.  The
// recognition logic itself is genuine (magic bytes + header parsing).

#ifndef AI_MODEL_H
#define AI_MODEL_H

#ifdef __cplusplus
extern "C" {
#endif

enum ModelFormat {
    MODEL_FMT_UNKNOWN    = 0,
    MODEL_FMT_GGUF,        // llama.cpp / GGUF
    MODEL_FMT_SAFETENSORS, // HF safetensors
    MODEL_FMT_PYTORCH,     // .bin/.pt/.ckpt (zip or pickle)
    MODEL_FMT_ONNX,        // ONNX protobuf
    MODEL_FMT_GGML,        // legacy ggml
    MODEL_FMT_CHECKPOINT   // generic checkpoint
};

struct ModelInfo {
    int                fmt;        // ModelFormat
    char               family[32]; // e.g. "qwen2", "llama"
    char               name[64];   // model name (from file/metadata)
    char               quant[24];  // e.g. "Q4_K_M", "F16"
    unsigned long long params;     // parameter count, 0 if unknown
    unsigned long long file_size;  // total bytes, 0 if unknown
};

struct KnownModel {
    const char* name;       // registry key, e.g. "qwen1.7b"
    const char* family;     // e.g. "qwen2"
    const char* params_str; // human readable, e.g. "1.7B"
    int         fmt;        // ModelFormat
    const char* quant;      // default quantization
    const char* url;        // canonical download URL
    unsigned long long approx_size; // approximate bytes
};

// Recognize a model from the first `len` bytes of a file.
// Fills `info` (family/name/quant/params when derivable). Returns fmt.
int ai_model_recognize_mem(const void* data, int len, struct ModelInfo* info);

// Human-readable format name for a ModelFormat value.
const char* ai_model_fmt_name(int fmt);

// Registry of known open-source models (Qwen-1.7B is the default).
int ai_model_count(void);
const struct KnownModel* ai_model_get(int i);
const struct KnownModel* ai_model_find(const char* name);
int  ai_model_set_default(const char* name);
const struct KnownModel* ai_model_default(void);

// Currently "loaded" model (set by `model run`).
void ai_model_set_active_name(const char* name);
const char* ai_model_active_name(void);

#ifdef __cplusplus
}
#endif

#endif // AI_MODEL_H
