// tools/test_gguf_infer.cpp - host-side numeric harness for gguf_infer.cpp.
//
// The kernel inference engine is freestanding, so it cannot be exercised from
// a normal test program directly.  Compiling it with -DGGUF_HOST_TEST swaps
// the serial-port logger for gguf_host_log(), which we implement here, and
// big_alloc/big_free become plain malloc/free.
//
// We then load build/test_model.gguf (a real, if tiny, qwen2 GGUF produced by
// tools/make_test_gguf.py with mixed Q4_K / Q5_K / Q6_K / Q8_0 / Q4_0 / F16
// tensors) and compare the final-position logits against the pure-Python
// reference forward pass in build/test_ref.txt.
//
// Build:
//   g++ -O2 -I. -DGGUF_HOST_TEST -c gguf_infer.cpp     -o build/h_infer.o
//   g++ -O2 -I.                  -c gguf.cpp           -o build/h_gguf.o
//   g++ -O2 -I. -DGGUF_HOST_TEST -c tools/test_gguf_infer.cpp -o build/h_main.o
//   g++ build/h_*.o -o build/test_gguf

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>

#include "gguf.h"
#include "gguf_infer.h"

// ---- hooks the freestanding engine expects ---------------------------
extern "C" void gguf_host_log(const char* s) { fputs(s, stderr); }

static uint64_t g_alloc_total = 0;
static uint64_t g_alloc_peak = 0;
static uint64_t g_alloc_live = 0;

extern "C" void* big_alloc(uint32_t bytes) {
    void* p = calloc(1, bytes ? bytes : 1);
    if (p) {
        g_alloc_total += bytes;
        g_alloc_live += bytes;
        if (g_alloc_live > g_alloc_peak) g_alloc_peak = g_alloc_live;
    }
    return p;
}
extern "C" void big_free(void* p, uint32_t bytes) {
    if (p) { free(p); if (g_alloc_live >= bytes) g_alloc_live -= bytes; }
}

// ---- helpers ---------------------------------------------------------
static uint8_t* slurp(const char* path, uint64_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return nullptr; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "short read on %s\n", path); fclose(f); return nullptr;
    }
    fclose(f);
    *out_size = (uint64_t)n;
    return buf;
}

struct Ref {
    int      tokens[64];
    int      n_tokens = 0;
    int      vocab = 0;
    float*   logits = nullptr;
};

static bool load_ref(const char* path, Ref* r) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return false; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "tokens", 6)) {
            char* p = line + 6;
            while (*p && r->n_tokens < 64) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p < '0' || *p > '9') break;
                r->tokens[r->n_tokens++] = (int)strtol(p, &p, 10);
            }
        } else if (!strncmp(line, "vocab", 5)) {
            r->vocab = atoi(line + 5);
        } else if (!strncmp(line, "logits", 6)) {
            if (r->vocab <= 0) { fclose(f); return false; }
            r->logits = (float*)malloc(sizeof(float) * r->vocab);
            for (int i = 0; i < r->vocab; i++) {
                if (!fgets(line, sizeof line, f)) { fclose(f); return false; }
                r->logits[i] = strtof(line, nullptr);
            }
        }
    }
    fclose(f);
    return r->n_tokens > 0 && r->logits != nullptr;
}

static void top5(const float* v, int n, int* idx, float* val) {
    for (int k = 0; k < 5; k++) { idx[k] = -1; val[k] = -1e30f; }
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 5; k++) {
            if (v[i] > val[k]) {
                for (int j = 4; j > k; j--) { val[j] = val[j-1]; idx[j] = idx[j-1]; }
                val[k] = v[i]; idx[k] = i;
                break;
            }
        }
    }
}

int main(int argc, char** argv) {
    const char* model = argc > 1 ? argv[1] : "build/test_model.gguf";
    const char* refp  = argc > 2 ? argv[2] : "build/test_ref.txt";

    Ref ref;
    if (!load_ref(refp, &ref)) { printf("FAIL: cannot parse reference\n"); return 1; }

    uint64_t sz = 0;
    uint8_t* blob = slurp(model, &sz);
    if (!blob) { printf("FAIL: cannot read model\n"); return 1; }
    printf("model    : %s (%llu bytes)\n", model, (unsigned long long)sz);

    int rc = qwen_load(blob, sz, 128);
    if (rc != 0) {
        printf("FAIL: qwen_load rc=%d err=%s\n", rc, qwen_error());
        return 1;
    }
    const GGUFModelInfo* mi = qwen_info();
    printf("arch     : %s\n", mi->arch);
    printf("layers   : %u  embd %u  heads %u/%u  ff %u  vocab %u\n",
           mi->block_count, mi->embed_length, mi->head_count,
           mi->head_count_kv, mi->feed_forward_length, mi->vocab_size);
    printf("rms_eps  : %.8g   rope_base %.1f\n", mi->rms_eps, mi->rope_theta);
    printf("runtime  : %llu bytes scratch/KV (peak host alloc %llu)\n",
           (unsigned long long)qwen_runtime_bytes(),
           (unsigned long long)g_alloc_peak);

    if ((int)mi->vocab_size != ref.vocab) {
        printf("FAIL: vocab mismatch %u vs %d\n", mi->vocab_size, ref.vocab);
        return 1;
    }

    // Decode the reference token sequence, keeping the last-position logits.
    const float* logits = nullptr;
    for (int i = 0; i < ref.n_tokens; i++) {
        logits = qwen_forward(ref.tokens[i], i);
        if (!logits) { printf("FAIL: forward returned NULL at pos %d\n", i); return 1; }
    }

    double max_abs = 0.0, sum_abs = 0.0, max_rel = 0.0;
    int worst = -1;
    for (int i = 0; i < ref.vocab; i++) {
        double d = fabs((double)logits[i] - (double)ref.logits[i]);
        double denom = fabs((double)ref.logits[i]);
        if (denom > 0.1) { double rel = d / denom; if (rel > max_rel) max_rel = rel; }
        sum_abs += d;
        if (d > max_abs) { max_abs = d; worst = i; }
    }
    printf("logits   : max|d| = %.6g at idx %d, mean|d| = %.6g, max rel = %.4g%%\n",
           max_abs, worst, sum_abs / ref.vocab, max_rel * 100.0);

    int ri[5], ki[5]; float rv[5], kv[5];
    top5(ref.logits, ref.vocab, ri, rv);
    top5(logits, ref.vocab, ki, kv);
    printf("ref top5 :");
    for (int k = 0; k < 5; k++) printf(" %d(%.5f)", ri[k], rv[k]);
    printf("\nkrn top5 :");
    for (int k = 0; k < 5; k++) printf(" %d(%.5f)", ki[k], kv[k]);
    printf("\n");

    bool order_ok = true;
    for (int k = 0; k < 5; k++) if (ri[k] != ki[k]) order_ok = false;

    // fp32 accumulation order differs between Python (doubles) and the kernel,
    // so require closeness rather than bit equality.
    bool num_ok = max_abs < 2e-3;

    // Tokenizer round-trip on ASCII + UTF-8 (the vocab is synthetic byte-level).
    int32_t toks[64];
    int nt = qwen_tokenize("Hello", toks, 64);
    printf("tokenize : \"Hello\" -> %d tokens:", nt);
    for (int i = 0; i < nt; i++) printf(" %d", toks[i]);
    printf("  (ref:");
    for (int i = 0; i < ref.n_tokens; i++) printf(" %d", ref.tokens[i]);
    printf(")\n");
    bool tok_ok = (nt == ref.n_tokens);
    for (int i = 0; i < nt && tok_ok; i++) if (toks[i] != ref.tokens[i]) tok_ok = false;

    char round[256]; round[0] = 0;
    for (int i = 0; i < nt; i++) {
        const char* piece = qwen_detokenize(toks[i]);
        if (piece) strncat(round, piece, sizeof(round) - strlen(round) - 1);
    }
    bool rt_ok = !strcmp(round, "Hello");
    printf("detok    : \"%s\" %s\n", round, rt_ok ? "(round-trip OK)" : "(MISMATCH)");

    // UTF-8 round-trip: byte-level BPE must split Chinese into raw bytes and
    // reassemble them exactly.
    const char* cn = "你好，世界";
    int32_t ctoks[64];
    int cn_n = qwen_tokenize(cn, ctoks, 64);
    char cround[256]; cround[0] = 0;
    for (int i = 0; i < cn_n; i++) {
        const char* piece = qwen_detokenize(ctoks[i]);
        if (piece) strncat(cround, piece, sizeof(cround) - strlen(cround) - 1);
    }
    bool cn_ok = !strcmp(cround, cn);
    printf("utf8     : \"%s\" -> %d tokens -> \"%s\" %s\n",
           cn, cn_n, cround, cn_ok ? "(OK)" : "(MISMATCH)");
    rt_ok = rt_ok && cn_ok;

    // Multi-step decode: greedy continuation must stay finite and in range.
    qwen_reset();
    int32_t cur = ref.tokens[0];
    int pos = 0;
    bool gen_ok = true;
    printf("greedy   :");
    for (int step = 0; step < 8; step++) {
        const float* lg = qwen_forward(cur, pos++);
        if (!lg) { gen_ok = false; break; }
        cur = qwen_sample(lg, 0.0f, 1.0f);       // temp 0 => argmax
        if (cur < 0 || cur >= (int)mi->vocab_size) { gen_ok = false; break; }
        for (int i = 0; i < (int)mi->vocab_size; i++)
            if (!(lg[i] == lg[i]) || fabsf(lg[i]) > 1e6f) { gen_ok = false; break; }
        printf(" %d", cur);
    }
    printf("  %s\n", gen_ok ? "(finite, in-range)" : "(BROKEN)");

    qwen_unload();
    printf("unload   : live host bytes after free = %llu\n",
           (unsigned long long)g_alloc_live);

    bool all_ok = num_ok && order_ok && tok_ok && rt_ok && gen_ok;
    printf("\n%s  numeric=%s topk-order=%s tokenizer=%s detok=%s decode=%s\n",
           all_ok ? "PASS" : "FAIL",
           num_ok ? "ok" : "BAD", order_ok ? "ok" : "BAD",
           tok_ok ? "ok" : "BAD", rt_ok ? "ok" : "BAD", gen_ok ? "ok" : "BAD");
    return all_ok ? 0 : 1;
}
