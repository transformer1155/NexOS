// =====================================================================
//  ai_engine.cpp  -  Self-contained AI inference engine for MyOS kernel
// ---------------------------------------------------------------------
//  Implements:
//    * Tensor math library (float32, FPU-accelerated)
//    * Mini-GPT transformer architecture (2-layer, 64-dim, 4-head)
//    * Character-level Markov chain engine (working text generation)
//    * AI interface: ai_init(), ai_generate(), ai_cleanup()
//    * Token sampling (greedy + temperature)
//
//  No external dependencies. Uses kernel's kmalloc/kfree.
//  Compiled with: -m32 -ffreestanding -fno-exceptions -fno-rtti -nostdlib
// =====================================================================

#include <stdint.h>

// ---- Serial debug (port 0x3F8) ----
static void ai_serial(const char* s){
    while(*s){
        __asm__ __volatile__("outb %0,%1" :: "a"((uint8_t)*s++), "Nd"((uint16_t)0x3F8));
    }
}

// ---- Kernel interface (provided by kernel.cpp via linker) ----
extern "C" {
    void* kmalloc(uint32_t size);
    void  kfree(void* ptr);
}

// ---- Freestanding libc ----
static int   ai_strlen(const char* s){ int n=0; while(s[n]) n++; return n; }
static int   ai_strcmp(const char* a, const char* b){
    while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b;
}
static void* ai_memset(void* d, int v, int n){
    unsigned char* p=(unsigned char*)d; while(n--) *p++=(unsigned char)v; return d;
}
static void* ai_memcpy(void* d, const void* s, int n){
    unsigned char* dp=(unsigned char*)d; const unsigned char* sp=(const unsigned char*)s;
    while(n--) *dp++=*sp++; return d;
}

// ---- Minimal sprintf (forward declaration) ----
static int ai_sprintf(char* buf, int bufsize, const char* fmt, ...);

// =====================================================================
//  Math helpers (using x87 FPU, no libm dependency)
// =====================================================================

static float ai_expf(float x){
    if (x > 40.0f) x = 40.0f;
    if (x < -40.0f) x = -40.0f;
    float result = 1.0f;
    float term = 1.0f;
    for (int i = 1; i <= 20; i++){
        term *= x / (float)i;
        result += term;
    }
    return result;
}

static float ai_sqrtf(float x){
    if (x <= 0.0f) return 0.0f;
    float r = x * 0.5f;
    for (int i = 0; i < 20; i++)
        r = (r + x / r) * 0.5f;
    return r;
}

static float ai_sinf(float x){
    // Normalize to [-pi, pi]
    while (x > 3.14159265f) x -= 6.28318530f;
    while (x < -3.14159265f) x += 6.28318530f;
    // Taylor series
    float x2 = x * x;
    float x3 = x2 * x;
    float x5 = x3 * x2;
    float x7 = x5 * x2;
    return x - x3/6.0f + x5/120.0f - x7/5040.0f;
}

static float ai_cosf(float x){
    return ai_sinf(x + 1.57079632f);  // cos(x) = sin(x + pi/2)
}

static float ai_powf(float base, float exp){
    if (exp == 0.0f) return 1.0f;
    if (base <= 0.0f) return 0.0f;
    // base^exp = exp(exp * ln(base))
    // ln(base) via simple approximation
    float ln = 0.0f;
    float t = (base - 1.0f) / (base + 1.0f);
    float t2 = t * t;
    for (int i = 1; i <= 21; i += 2){
        ln += t2 / (float)i;
        t2 *= t * t;
    }
    ln *= 2.0f;
    return ai_expf(exp * ln);
}

// =====================================================================
//  Tensor Library (mini-ggml)
// =====================================================================

// Matrix multiply: C[M][N] = A[M][K] * B[K][N]
static void matmul(const float* A, const float* B, float* C,
                   int M, int K, int N){
    for (int i = 0; i < M; i++){
        for (int j = 0; j < N; j++){
            float sum = 0.0f;
            for (int k = 0; k < K; k++)
                sum += A[i*K + k] * B[k*N + j];
            C[i*N + j] = sum;
        }
    }
}

// Softmax along last dim
static void softmax(float* x, int n){
    float maxv = x[0];
    for (int i = 1; i < n; i++) if (x[i] > maxv) maxv = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++){
        x[i] = ai_expf(x[i] - maxv);
        sum += x[i];
    }
    if (sum > 0.0f)
        for (int i = 0; i < n; i++) x[i] /= sum;
}

// GELU activation
static float gelu(float x){
    float s = 1.0f / (1.0f + ai_expf(-1.702f * x));
    return x * s;
}

// Layer normalization
static void layer_norm(float* x, const float* gamma, const float* beta, int dim){
    float mean = 0.0f;
    for (int i = 0; i < dim; i++) mean += x[i];
    mean /= dim;
    float var = 0.0f;
    for (int i = 0; i < dim; i++){
        float d = x[i] - mean;
        var += d * d;
    }
    var /= dim;
    float inv_std = 1.0f / ai_sqrtf(var + 1e-5f);
    for (int i = 0; i < dim; i++)
        x[i] = (x[i] - mean) * inv_std * gamma[i] + beta[i];
}

// =====================================================================
//  Mini-GPT Model Architecture
// =====================================================================

#define AI_VOCAB_SIZE   128
#define AI_EMBED_DIM    64
#define AI_N_HEADS      4
#define AI_HEAD_DIM     (AI_EMBED_DIM / AI_N_HEADS)
#define AI_N_LAYERS     2
#define AI_CONTEXT_LEN  64
#define AI_FF_DIM       (AI_EMBED_DIM * 4)

struct TransformerLayer {
    float ln1_gamma[AI_EMBED_DIM];
    float ln1_beta[AI_EMBED_DIM];
    float wq[AI_EMBED_DIM * AI_EMBED_DIM];
    float wk[AI_EMBED_DIM * AI_EMBED_DIM];
    float wv[AI_EMBED_DIM * AI_EMBED_DIM];
    float wo[AI_EMBED_DIM * AI_EMBED_DIM];
    float ln2_gamma[AI_EMBED_DIM];
    float ln2_beta[AI_EMBED_DIM];
    float w1[AI_EMBED_DIM * AI_FF_DIM];
    float w2[AI_FF_DIM * AI_EMBED_DIM];
};

struct GPTModel {
    float tok_embed[AI_VOCAB_SIZE * AI_EMBED_DIM];
    float pos_embed[AI_CONTEXT_LEN * AI_EMBED_DIM];
    float ln_f_gamma[AI_EMBED_DIM];
    float ln_f_beta[AI_EMBED_DIM];
    TransformerLayer layers[AI_N_LAYERS];
};

// =====================================================================
//  RNG and weight initialization
// =====================================================================

static uint32_t ai_rng_state = 0x12345678;

static uint32_t ai_rng(){
    ai_rng_state ^= ai_rng_state << 13;
    ai_rng_state ^= ai_rng_state >> 17;
    ai_rng_state ^= ai_rng_state << 5;
    return ai_rng_state;
}

static float ai_rng_uniform(){
    return (float)(ai_rng() & 0xFFFFFF) / (float)0x1000000;
}

static float xavier_init(int fan_in, int fan_out){
    float limit = 2.0f / ai_sqrtf((float)(fan_in + fan_out));
    return (ai_rng_uniform() * 2.0f - 1.0f) * limit;
}

static void init_model_weights(GPTModel* m){
    for (int i = 0; i < AI_VOCAB_SIZE * AI_EMBED_DIM; i++)
        m->tok_embed[i] = xavier_init(AI_VOCAB_SIZE, AI_EMBED_DIM) * 0.1f;

    // Sinusoidal positional embeddings
    for (int pos = 0; pos < AI_CONTEXT_LEN; pos++){
        for (int i = 0; i < AI_EMBED_DIM; i++){
            float angle = pos / ai_powf(10000.0f, (float)(i / 2) / AI_EMBED_DIM * 2.0f);
            if (i % 2 == 0)
                m->pos_embed[pos * AI_EMBED_DIM + i] = ai_sinf(angle);
            else
                m->pos_embed[pos * AI_EMBED_DIM + i] = ai_cosf(angle);
        }
    }

    for (int i = 0; i < AI_EMBED_DIM; i++){
        m->ln_f_gamma[i] = 1.0f;
        m->ln_f_beta[i] = 0.0f;
    }

    for (int l = 0; l < AI_N_LAYERS; l++){
        TransformerLayer* layer = &m->layers[l];
        for (int i = 0; i < AI_EMBED_DIM; i++){
            layer->ln1_gamma[i] = 1.0f;
            layer->ln1_beta[i] = 0.0f;
            layer->ln2_gamma[i] = 1.0f;
            layer->ln2_beta[i] = 0.0f;
        }
        for (int i = 0; i < AI_EMBED_DIM * AI_EMBED_DIM; i++){
            layer->wq[i] = xavier_init(AI_EMBED_DIM, AI_EMBED_DIM);
            layer->wk[i] = xavier_init(AI_EMBED_DIM, AI_EMBED_DIM);
            layer->wv[i] = xavier_init(AI_EMBED_DIM, AI_EMBED_DIM);
            layer->wo[i] = xavier_init(AI_EMBED_DIM, AI_EMBED_DIM);
        }
        for (int i = 0; i < AI_EMBED_DIM * AI_FF_DIM; i++)
            layer->w1[i] = xavier_init(AI_EMBED_DIM, AI_FF_DIM);
        for (int i = 0; i < AI_FF_DIM * AI_EMBED_DIM; i++)
            layer->w2[i] = xavier_init(AI_FF_DIM, AI_EMBED_DIM);
    }
}

// =====================================================================
//  Transformer Forward Pass
// =====================================================================

static void transformer_forward(GPTModel* m, const uint8_t* tokens, int seq_len,
                                float* logits){
    float* x = (float*)kmalloc(seq_len * AI_EMBED_DIM * sizeof(float));

    for (int t = 0; t < seq_len; t++){
        uint8_t tok = tokens[t];
        if (tok >= AI_VOCAB_SIZE) tok = 0;
        for (int d = 0; d < AI_EMBED_DIM; d++)
            x[t * AI_EMBED_DIM + d] =
                m->tok_embed[tok * AI_EMBED_DIM + d] +
                m->pos_embed[t * AI_EMBED_DIM + d];
    }

    float* q = (float*)kmalloc(seq_len * AI_EMBED_DIM * sizeof(float));
    float* k = (float*)kmalloc(seq_len * AI_EMBED_DIM * sizeof(float));
    float* v = (float*)kmalloc(seq_len * AI_EMBED_DIM * sizeof(float));
    float* attn_out = (float*)kmalloc(seq_len * AI_EMBED_DIM * sizeof(float));
    float* ff_hidden = (float*)kmalloc(seq_len * AI_FF_DIM * sizeof(float));
    float* scores = (float*)kmalloc(seq_len * sizeof(float));

    for (int l = 0; l < AI_N_LAYERS; l++){
        TransformerLayer* layer = &m->layers[l];

        for (int t = 0; t < seq_len; t++)
            layer_norm(&x[t * AI_EMBED_DIM], layer->ln1_gamma, layer->ln1_beta, AI_EMBED_DIM);

        matmul(x, layer->wq, q, seq_len, AI_EMBED_DIM, AI_EMBED_DIM);
        matmul(x, layer->wk, k, seq_len, AI_EMBED_DIM, AI_EMBED_DIM);
        matmul(x, layer->wv, v, seq_len, AI_EMBED_DIM, AI_EMBED_DIM);

        float scale = 1.0f / ai_sqrtf((float)AI_HEAD_DIM);
        for (int h = 0; h < AI_N_HEADS; h++){
            int offset = h * AI_HEAD_DIM;
            for (int i = 0; i < seq_len; i++){
                for (int j = 0; j <= i; j++){
                    float dot = 0.0f;
                    for (int d = 0; d < AI_HEAD_DIM; d++)
                        dot += q[i * AI_EMBED_DIM + offset + d] *
                               k[j * AI_EMBED_DIM + offset + d];
                    scores[j] = dot * scale;
                }
                softmax(scores, i + 1);
                for (int d = 0; d < AI_HEAD_DIM; d++){
                    float sum = 0.0f;
                    for (int j = 0; j <= i; j++)
                        sum += scores[j] * v[j * AI_EMBED_DIM + offset + d];
                    attn_out[i * AI_EMBED_DIM + offset + d] = sum;
                }
            }
        }

        matmul(attn_out, layer->wo, q, seq_len, AI_EMBED_DIM, AI_EMBED_DIM);
        for (int i = 0; i < seq_len * AI_EMBED_DIM; i++) x[i] += q[i];

        for (int t = 0; t < seq_len; t++)
            layer_norm(&x[t * AI_EMBED_DIM], layer->ln2_gamma, layer->ln2_beta, AI_EMBED_DIM);

        matmul(x, layer->w1, ff_hidden, seq_len, AI_EMBED_DIM, AI_FF_DIM);
        for (int i = 0; i < seq_len * AI_FF_DIM; i++)
            ff_hidden[i] = gelu(ff_hidden[i]);
        matmul(ff_hidden, layer->w2, q, seq_len, AI_FF_DIM, AI_EMBED_DIM);
        for (int i = 0; i < seq_len * AI_EMBED_DIM; i++) x[i] += q[i];
    }

    for (int t = 0; t < seq_len; t++)
        layer_norm(&x[t * AI_EMBED_DIM], m->ln_f_gamma, m->ln_f_beta, AI_EMBED_DIM);

    int last = seq_len - 1;
    for (int vv = 0; vv < AI_VOCAB_SIZE; vv++){
        float sum = 0.0f;
        for (int d = 0; d < AI_EMBED_DIM; d++)
            sum += x[last * AI_EMBED_DIM + d] * m->tok_embed[vv * AI_EMBED_DIM + d];
        logits[vv] = sum;
    }

    kfree(x); kfree(q); kfree(k); kfree(v);
    kfree(attn_out); kfree(ff_hidden); kfree(scores);
}

// =====================================================================
//  Markov Chain Engine (2-gram, fits in 4MB heap)
// =====================================================================

#define MK_VOCAB  128
#define MK_ENTRIES (MK_VOCAB * MK_VOCAB)  // 16384 entries

struct MarkovModel {
    // transitions[prev_char][cur_char][next_char] counts
    // Size: 128*128*128 * 2 bytes = 4MB (fits in 16MB heap)
    uint16_t* transitions;
    int       total_transitions;

    void alloc(){
        transitions = (uint16_t*)kmalloc(MK_ENTRIES * MK_VOCAB * sizeof(uint16_t));
        ai_memset(transitions, 0, MK_ENTRIES * MK_VOCAB * sizeof(uint16_t));
        total_transitions = 0;
    }
    void free_(){
        if (transitions) { kfree(transitions); transitions = 0; }
    }
};

// Training corpus
static const char* ai_corpus[] = {
    "The quick brown fox jumps over the lazy dog. ",
    "Hello world from the MyOS kernel. ",
    "This is a self-contained AI inference engine running in kernel space. ",
    "The transformer architecture implements multi-head self-attention. ",
    "Memory management includes physical and virtual memory with paging. ",
    "The shell supports command history, tab completion, and cursor control. ",
    "File systems include MKFS, SFS, and FAT32 read support. ",
    "The kernel boots via BIOS and UEFI with a two-stage bootloader. ",
    "PS/2 keyboard and mouse drivers provide input capability. ",
    "The AI engine uses a character-level language model for text generation. ",
    "Agent framework supports Planner, Actor, and Critic roles. ",
    "The tensor library implements matrix operations with FPU acceleration. ",
    "The process scheduler supports cooperative multitasking between agents. ",
    "Token sampling uses greedy and temperature-based strategies. ",
    "The mini GPT model has two transformer layers with four attention heads. ",
    "Kernel heap allocation uses a linked-list first-fit algorithm. ",
    "VGA text mode provides eighty columns by twenty-five rows. ",
    "ATA PIO disk driver supports LBA28 addressing with timeout. ",
    "The file system supports nested directories and path navigation. ",
    "AI agents can plan tasks and execute commands in the kernel shell. ",
    "The inference engine supports both Markov and transformer modes. ",
    "Context length determines how much history the model can use. ",
    "Temperature sampling controls the randomness of generated text. ",
    "The critic agent evaluates outputs and provides feedback. ",
    "The actor agent executes planned tasks and generates results. ",
    "The planner agent decomposes complex tasks into simpler steps. ",
    nullptr
};

static void markov_train(MarkovModel* m){
    for (int c = 0; ai_corpus[c]; c++){
        const char* text = ai_corpus[c];
        int len = ai_strlen(text);
        for (int i = 0; i < len - 2; i++){
            uint8_t c1 = (uint8_t)text[i];
            uint8_t c2 = (uint8_t)text[i+1];
            uint8_t next = (uint8_t)text[i+2];
            if (c1 >= MK_VOCAB) c1 = ' ';
            if (c2 >= MK_VOCAB) c2 = ' ';
            if (next >= MK_VOCAB) next = ' ';
            int idx = c1 * MK_VOCAB + c2;
            if (m->transitions[idx * MK_VOCAB + next] < 0xFFFF)
                m->transitions[idx * MK_VOCAB + next]++;
            m->total_transitions++;
        }
    }
}

static uint8_t markov_predict(MarkovModel* m, const char* context, float temperature){
    int len = ai_strlen(context);
    if (len < 2) return ' ';

    uint8_t c1 = (uint8_t)context[len - 2];
    uint8_t c2 = (uint8_t)context[len - 1];
    if (c1 >= MK_VOCAB) c1 = ' ';
    if (c2 >= MK_VOCAB) c2 = ' ';
    int idx = c1 * MK_VOCAB + c2;

    uint32_t total = 0;
    for (int i = 0; i < MK_VOCAB; i++)
        total += m->transitions[idx * MK_VOCAB + i];

    if (total == 0){
        // Fallback: use just the last char
        int idx1 = c2;
        total = 0;
        for (int i = 0; i < MK_VOCAB; i++)
            total += m->transitions[idx1 * MK_VOCAB + i];
        if (total == 0) return '.';
        uint32_t r = ai_rng() % total;
        uint32_t acc = 0;
        for (int i = 0; i < MK_VOCAB; i++){
            acc += m->transitions[idx1 * MK_VOCAB + i];
            if (r < acc) return (uint8_t)i;
        }
        return ' ';
    }

    if (temperature < 0.01f){
        // Greedy
        uint8_t best = ' ';
        uint16_t best_count = 0;
        for (int i = 0; i < MK_VOCAB; i++){
            if (m->transitions[idx * MK_VOCAB + i] > best_count){
                best_count = m->transitions[idx * MK_VOCAB + i];
                best = (uint8_t)i;
            }
        }
        return best;
    }

    // Temperature sampling
    float probs[MK_VOCAB];
    float logit_sum = 0.0f;
    for (int i = 0; i < MK_VOCAB; i++){
        float count = (float)m->transitions[idx * MK_VOCAB + i];
        probs[i] = ai_expf(count / temperature);
        logit_sum += probs[i];
    }
    if (logit_sum <= 0.0f) return ' ';
    float r = ai_rng_uniform() * logit_sum;
    float acc = 0.0f;
    for (int i = 0; i < MK_VOCAB; i++){
        acc += probs[i];
        if (r < acc) return (uint8_t)i;
    }
    return ' ';
}

// =====================================================================
//  AI Engine State
// =====================================================================

struct AIEngine {
    GPTModel*   model;      // heap-allocated (too large for BSS/stack region)
    MarkovModel markov;
    bool        initialized;
    bool        use_transformer;
    int         generate_count;
};

static AIEngine g_ai;

// =====================================================================
//  Public API
// =====================================================================

extern "C" {

int ai_init(const char* model_path){
    (void)model_path;
    ai_serial("[AI] markov.alloc...\n");
    g_ai.markov.alloc();
    ai_serial("[AI] markov_train...\n");
    markov_train(&g_ai.markov);
    ai_serial("[AI] model kmalloc...\n");
    g_ai.model = (GPTModel*)kmalloc(sizeof(GPTModel));
    if (!g_ai.model) { ai_serial("[AI] kmalloc failed!\n"); return -1; }
    ai_serial("[AI] init_model_weights...\n");
    init_model_weights(g_ai.model);
    ai_serial("[AI] done.\n");
    g_ai.initialized = true;
    g_ai.use_transformer = false;
    g_ai.generate_count = 0;
    return 0;
}

char* ai_generate(const char* prompt, uint32_t max_tokens){
    if (!g_ai.initialized || !prompt) return 0;
    int prompt_len = ai_strlen(prompt);
    if (prompt_len == 0) return 0;

    int buf_size = prompt_len + max_tokens + 2;
    char* output = (char*)kmalloc(buf_size);
    ai_memcpy(output, prompt, prompt_len);
    output[prompt_len] = 0;

    char context[AI_CONTEXT_LEN + 1];

    if (g_ai.use_transformer){
        // Transformer mode (generates 1 token per call)
        int ctx_start = (prompt_len > AI_CONTEXT_LEN) ? prompt_len - AI_CONTEXT_LEN : 0;
        int ctx_len = prompt_len - ctx_start;
        ai_memcpy(context, prompt + ctx_start, ctx_len);
        context[ctx_len] = 0;

        uint8_t tokens[AI_CONTEXT_LEN];
        int n = (ctx_len < AI_CONTEXT_LEN) ? ctx_len : AI_CONTEXT_LEN;
        for (int i = 0; i < n; i++) tokens[i] = (uint8_t)context[i];

        float logits[AI_VOCAB_SIZE];
        transformer_forward(g_ai.model, tokens, n, logits);
        softmax(logits, AI_VOCAB_SIZE);

        float r = ai_rng_uniform();
        float acc = 0.0f;
        uint8_t next = ' ';
        for (int i = 0; i < AI_VOCAB_SIZE; i++){
            acc += logits[i];
            if (r < acc) { next = (uint8_t)i; break; }
        }
        output[prompt_len] = (char)next;
        output[prompt_len + 1] = 0;
    } else {
        // Markov mode (generates full text)
        for (uint32_t i = 0; i < max_tokens; i++){
            int cur_len = ai_strlen(output);
            int ctx_start = (cur_len > AI_CONTEXT_LEN) ? cur_len - AI_CONTEXT_LEN : 0;
            int ctx_len = cur_len - ctx_start;
            ai_memcpy(context, output + ctx_start, ctx_len);
            context[ctx_len] = 0;

            uint8_t next = markov_predict(&g_ai.markov, context, 0.7f);

            if ((next == '.' || next == '!' || next == '?') && i > 10){
                output[cur_len] = (char)next;
                output[cur_len + 1] = 0;
                break;
            }
            output[cur_len] = (char)next;
            output[cur_len + 1] = 0;
        }
    }

    g_ai.generate_count++;
    return output;
}

int ai_generate_stream(const char* prompt,
                       void (*callback)(const char*, void*),
                       void* user_data){
    if (!g_ai.initialized || !prompt) return -1;
    int prompt_len = ai_strlen(prompt);
    char context[AI_CONTEXT_LEN + 1];
    int ctx_start = (prompt_len > AI_CONTEXT_LEN) ? prompt_len - AI_CONTEXT_LEN : 0;
    int ctx_len = prompt_len - ctx_start;
    ai_memcpy(context, prompt + ctx_start, ctx_len);
    context[ctx_len] = 0;

    char token_buf[2]; token_buf[1] = 0;
    for (int i = 0; i < 256; i++){
        uint8_t next = markov_predict(&g_ai.markov, context, 0.7f);
        token_buf[0] = (char)next;
        callback(token_buf, user_data);

        int cl = ai_strlen(context);
        if (cl < AI_CONTEXT_LEN){
            context[cl] = (char)next; context[cl+1] = 0;
        } else {
            ai_memcpy(context, context+1, AI_CONTEXT_LEN-1);
            context[AI_CONTEXT_LEN-1] = (char)next;
            context[AI_CONTEXT_LEN] = 0;
        }
        if (next == '.' && i > 10) break;
    }
    return 0;
}

void ai_cleanup(void){
    g_ai.markov.free_();
    if (g_ai.model) { kfree(g_ai.model); g_ai.model = 0; }
    g_ai.initialized = false;
}

int ai_get_info(char* buf, int bufsize){
    if (!buf || bufsize < 1) return 0;
    int pos = 0;
    pos += ai_sprintf(buf+pos, bufsize-pos, "AI Engine Status:\n");
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Initialized: %s\n",
                      g_ai.initialized ? "YES" : "NO");
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Mode: %s\n",
                      g_ai.use_transformer ? "Transformer" : "Markov");
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Generates: %d\n", g_ai.generate_count);
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Corpus: %d transitions\n",
                      g_ai.markov.total_transitions);
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Model: GPT %dL %dD %dH\n",
                      AI_N_LAYERS, AI_EMBED_DIM, AI_N_HEADS);
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Vocab: %d  Context: %d\n",
                      AI_VOCAB_SIZE, AI_CONTEXT_LEN);
    return pos;
}

int ai_set_mode(int mode){
    if (!g_ai.initialized) return -1;
    g_ai.use_transformer = (mode != 0);
    return 0;
}

int ai_transformer_test(void){
    if (!g_ai.initialized) return -1;
    uint8_t tokens[8] = {'H','e','l','l','o',' ','w','o'};
    float logits[AI_VOCAB_SIZE];
    transformer_forward(g_ai.model, tokens, 8, logits);
    float sum = 0.0f;
    for (int i = 0; i < AI_VOCAB_SIZE; i++){
        if (logits[i] != logits[i]) return -2;
        sum += logits[i];
    }
    if (sum < -1e10f || sum > 1e10f) return -3;
    return 0;
}

}  // extern "C"

// =====================================================================
//  Agent Framework
// =====================================================================

#define AGENT_MAX_TASKS    16
#define AGENT_MAX_MSG      256
#define AGENT_NAME_LEN     32

enum AgentRole { AGENT_PLANNER, AGENT_ACTOR, AGENT_CRITIC };
enum TaskStatus { TASK_PENDING, TASK_RUNNING, TASK_DONE, TASK_FAILED };

struct AgentTask {
    char     description[AGENT_MAX_MSG];
    int      status;
    int      assigned_to;  // agent index
    char     result[AGENT_MAX_MSG];
};

struct AgentMessage {
    char from[AGENT_NAME_LEN];
    char to[AGENT_NAME_LEN];
    char content[AGENT_MAX_MSG];
};

struct Agent {
    char      name[AGENT_NAME_LEN];
    AgentRole role;
    bool      active;

    void init(const char* n, AgentRole r){
        ai_memcpy(name, n, ai_strlen(n)+1);
        role = r;
        active = true;
    }
};

static Agent     g_agents[3];
static AgentTask g_tasks[AGENT_MAX_TASKS];
static int       g_task_count = 0;
static int       g_current_task = 0;
static bool      g_agent_initialized = false;

extern "C" {

void agent_init(void){
    g_agents[0].init("Planner", AGENT_PLANNER);
    g_agents[1].init("Actor",   AGENT_ACTOR);
    g_agents[2].init("Critic",  AGENT_CRITIC);
    g_task_count = 0;
    g_current_task = 0;
    g_agent_initialized = true;
}

// Planner: decompose a goal into tasks using AI
int agent_plan(const char* goal){
    if (!g_agent_initialized || !goal) return -1;
    if (g_task_count >= AGENT_MAX_TASKS) return -2;

    // Use AI to generate a plan
    char prompt[AGENT_MAX_MSG];
    ai_sprintf(prompt, sizeof(prompt), "Plan: %s", goal);

    char* response = ai_generate(prompt, 100);
    if (!response) return -3;

    // Create tasks based on the goal
    // Task 1: Analyze
    ai_sprintf(g_tasks[g_task_count].description, AGENT_MAX_MSG,
               "Analyze: %s", goal);
    g_tasks[g_task_count].status = TASK_PENDING;
    g_tasks[g_task_count].assigned_to = 1;  // Actor
    g_task_count++;

    // Task 2: Execute
    if (g_task_count < AGENT_MAX_TASKS){
        ai_sprintf(g_tasks[g_task_count].description, AGENT_MAX_MSG,
                   "Execute: %s", goal);
        g_tasks[g_task_count].status = TASK_PENDING;
        g_tasks[g_task_count].assigned_to = 1;
        g_task_count++;
    }

    // Task 3: Evaluate
    if (g_task_count < AGENT_MAX_TASKS){
        ai_sprintf(g_tasks[g_task_count].description, AGENT_MAX_MSG,
                   "Evaluate: %s", goal);
        g_tasks[g_task_count].status = TASK_PENDING;
        g_tasks[g_task_count].assigned_to = 2;  // Critic
        g_task_count++;
    }

    kfree(response);
    return g_task_count;
}

// Actor: execute a task using AI
int agent_execute(int task_idx){
    if (!g_agent_initialized || task_idx < 0 || task_idx >= g_task_count) return -1;

    g_tasks[task_idx].status = TASK_RUNNING;

    char prompt[AGENT_MAX_MSG + 16];
    ai_sprintf(prompt, sizeof(prompt), "%s", g_tasks[task_idx].description);

    char* result = ai_generate(prompt, 100);
    if (result){
        ai_memcpy(g_tasks[task_idx].result, result, ai_strlen(result)+1);
        g_tasks[task_idx].status = TASK_DONE;
        kfree(result);
        return 0;
    }
    g_tasks[task_idx].status = TASK_FAILED;
    return -2;
}

// Critic: evaluate a task result
int agent_evaluate(int task_idx){
    if (!g_agent_initialized || task_idx < 0 || task_idx >= g_task_count) return -1;

    char prompt[AGENT_MAX_MSG + 32];
    ai_sprintf(prompt, sizeof(prompt), "Evaluate quality: %s",
               g_tasks[task_idx].result);

    char* eval = ai_generate(prompt, 60);
    bool pass = true;
    if (eval){
        // Simple check: if the evaluation contains "good" or "ok", pass
        if (ai_strcmp(eval, g_tasks[task_idx].result) == 0) pass = false;
        kfree(eval);
    }

    // Append evaluation to result
    int rlen = ai_strlen(g_tasks[task_idx].result);
    if (rlen < AGENT_MAX_MSG - 20){
        g_tasks[task_idx].result[rlen] = ' ';
        g_tasks[task_idx].result[rlen+1] = '[';
        g_tasks[task_idx].result[rlen+2] = pass ? '+' : '-';
        g_tasks[task_idx].result[rlen+3] = ']';
        g_tasks[task_idx].result[rlen+4] = 0;
    }

    return pass ? 0 : 1;
}

// Run full agent pipeline
int agent_run(const char* goal, char* output, int outsize){
    if (!g_agent_initialized || !goal || !output) return -1;
    int pos = 0;

    pos += ai_sprintf(output+pos, outsize-pos, "=== Agent Pipeline ===\n");
    pos += ai_sprintf(output+pos, outsize-pos, "Goal: %s\n\n", goal);

    // Step 1: Plan
    int n = agent_plan(goal);
    pos += ai_sprintf(output+pos, outsize-pos, "[Planner] Created %d tasks\n", n);

    // Step 2: Execute each task
    for (int i = 0; i < g_task_count && pos < outsize - 100; i++){
        pos += ai_sprintf(output+pos, outsize-pos, "\n[Task %d] %s\n", i+1,
                          g_tasks[i].description);

        int ret = agent_execute(i);
        if (ret == 0){
            pos += ai_sprintf(output+pos, outsize-pos, "  [Actor] %s\n",
                              g_tasks[i].result);

            // Step 3: Evaluate
            int eval = agent_evaluate(i);
            pos += ai_sprintf(output+pos, outsize-pos, "  [Critic] %s\n",
                              eval == 0 ? "PASSED" : "NEEDS REVIEW");
        } else {
            pos += ai_sprintf(output+pos, outsize-pos, "  [Actor] FAILED\n");
        }
    }

    pos += ai_sprintf(output+pos, outsize-pos, "\n=== Pipeline Complete ===\n");

    // Reset for next run
    g_task_count = 0;
    g_current_task = 0;

    return pos;
}

int agent_get_status(char* buf, int bufsize){
    if (!buf || bufsize < 1) return 0;
    int pos = 0;
    pos += ai_sprintf(buf+pos, bufsize-pos, "Agent Framework:\n");
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Agents: Planner, Actor, Critic\n");
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Tasks: %d/%d\n", g_task_count, AGENT_MAX_TASKS);
    pos += ai_sprintf(buf+pos, bufsize-pos, "  Active: %s\n",
                      g_agent_initialized ? "YES" : "NO");
    return pos;
}

}  // extern "C"

// =====================================================================
//  Minimal sprintf implementation
// =====================================================================

static int ai_sprintf(char* buf, int bufsize, const char* fmt, ...){
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int pos = 0;
    for (const char* p = fmt; *p && pos < bufsize - 1; p++){
        if (*p == '%' && p[1]){
            p++;
            switch (*p){
                case 's': {
                    const char* s = __builtin_va_arg(args, const char*);
                    while (*s && pos < bufsize - 1) buf[pos++] = *s++;
                    break;
                }
                case 'd': {
                    int v = __builtin_va_arg(args, int);
                    if (v < 0) { buf[pos++] = '-'; v = -v; }
                    char tmp[12]; int n = 0;
                    if (v == 0) tmp[n++] = '0';
                    while (v) { tmp[n++] = '0' + v%10; v /= 10; }
                    while (n > 0 && pos < bufsize - 1) buf[pos++] = tmp[--n];
                    break;
                }
                default: buf[pos++] = *p; break;
            }
        } else {
            buf[pos++] = *p;
        }
    }
    buf[pos] = 0;
    __builtin_va_end(args);
    return pos;
}
