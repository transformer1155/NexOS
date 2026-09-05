// knowledge_base.c - in-kernel verified-fact knowledge base (freestanding 64-bit C).
//
// See knowledge_base.h for the design principle: truth-from-practice, and
// authoritative-institution fallback when a question cannot be verified.

#include "knowledge_base.h"

// ---------------------------------------------------------------------------
// Minimal freestanding string helpers (no libc under -ffreestanding)
// ---------------------------------------------------------------------------

static int kb_tolower(int c){
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

// Case-insensitive substring search: does `needle` occur in `hay`?
static int kb_stristr(const char* hay, const char* needle){
    if (!hay || !needle || !*needle) return 1; // empty needle matches
    int nlen = 0;
    while (needle[nlen]) nlen++;
    int hlen = 0;
    while (hay[hlen]) hlen++;
    if (nlen > hlen) return 0;
    for (int i = 0; i <= hlen - nlen; i++){
        int ok = 1;
        for (int j = 0; j < nlen; j++){
            if (kb_tolower((unsigned char)hay[i+j]) != kb_tolower((unsigned char)needle[j])){
                ok = 0; break;
            }
        }
        if (ok) return 1;
    }
    return 0;
}

// Append `src` to `buf` (already len bytes), respecting cap; returns new len.
static int kb_cat(char* buf, int len, int cap, const char* src){
    if (!src) return len;
    int i = 0;
    while (src[i] && len < cap - 1){
        buf[len++] = src[i++];
    }
    buf[len] = 0;
    return len;
}

// ---------------------------------------------------------------------------
// Curated, VERIFIED facts.  Each carries an authoritative source.
// Extend this array as more verified knowledge is added.
// ---------------------------------------------------------------------------

static const char* k_keys_deepseek[] = { "deepseek", "深度求索", 0 };
static const char* k_keys_nexos[]    = { "nexos", "迷你os", "minios", "迷你 os", 0 };
static const char* k_keys_qwen[]     = { "qwen", "通义千问", "通义", 0 };
static const char* k_keys_llamacpp[] = { "llama.cpp", "llamacpp", "llama cpp", "llama-cpp", 0 };
static const char* k_keys_openai[]   = { "openai", "gpt", "chatgpt", "chat gpt", 0 };
static const char* k_keys_mbedtls[]  = { "mbedtls", "polarssl", "mbed tls", 0 };

static const kb_entry_t g_kb[] = {
    {
        "DeepSeek", k_keys_deepseek,
        "DeepSeek（深度求索）是由杭州深度求索人工智能基础技术研究有限公司（DeepSeek-AI）开发的，"
        "创始人为梁文锋，团队脱胎于量化私募幻方量化（High-Flyer）。它发布了 DeepSeek-V3、"
        "DeepSeek-R1 等开源大模型，并非阿里巴巴的产品。",
        "DeepSeek 官方（deepseek.com）/ 公开工商信息"
    },
    {
        "NexOS", k_keys_nexos,
        "NexOS（前称 MiniOS）是一个爱好型开源操作系统，由本项目作者使用 C/C++/汇编写成，"
        "包含 32 位与 64 位双内核、自研 GUI（NexOS.Forms）与 C# 托管 Shell，以及自研的本地大模型推理引擎。",
        "本项目源代码仓库"
    },
    {
        "Qwen", k_keys_qwen,
        "Qwen（通义千问）是由阿里巴巴集团的通义实验室（阿里云）开发的大语言模型系列，"
        "与 DeepSeek 是不同公司的产品。",
        "阿里云 / 通义千问官方（qwen.ai / aliyun.com）"
    },
    {
        "llama.cpp", k_keys_llamacpp,
        "llama.cpp 是由 Georgi Gerganov 发起、社区维护的开源本地大模型推理框架，"
        "使用 ggml 计算后端，支持 CPU/GPU 量化推理，是 Ollama 等工具的底层引擎之一。",
        "llama.cpp 官方仓库（github.com/ggerganov/llama.cpp）"
    },
    {
        "OpenAI", k_keys_openai,
        "GPT 系列与 ChatGPT 由 OpenAI 公司开发，CEO 为 Sam Altman；OpenAI 是独立于 DeepSeek、"
        "阿里巴巴的公司。",
        "OpenAI 官方（openai.com）"
    },
    {
        "mbedTLS", k_keys_mbedtls,
        "mbedTLS（原 PolarSSL）是由 Arm 维护的开源 TLS/DTLS 加密库，提供 SSL/TLS 与加密原语，"
        "广泛用于嵌入式与物联网设备。",
        "Arm / TrustedFirmware 官方（mbedtls.org）"
    },
};

static const int g_kb_n = (int)(sizeof(g_kb) / sizeof(g_kb[0]));

int kb_count(void){ return g_kb_n; }

// ---------------------------------------------------------------------------
// Retrieval: score each entry by how many of its keywords appear in the
// (case-insensitively matched) question; pick the highest-scoring match.
// ---------------------------------------------------------------------------

int kb_lookup(const char* question,
              char* out_fact, int fact_cap,
              char* out_src,  int src_cap,
              const char** out_topic)
{
    if (out_fact && fact_cap > 0) out_fact[0] = 0;
    if (out_src  && src_cap  > 0) out_src[0]  = 0;
    if (!question) return 0;

    int best = -1, best_score = 0;
    for (int i = 0; i < g_kb_n; i++){
        int score = 0;
        for (int k = 0; g_kb[i].keys[k]; k++){
            if (kb_stristr(question, g_kb[i].keys[k])) score++;
        }
        if (score > best_score){ best_score = score; best = i; }
    }
    if (best < 0 || best_score == 0) return 0;

    if (out_fact && fact_cap > 0) kb_cat(out_fact, 0, fact_cap, g_kb[best].fact);
    if (out_src  && src_cap  > 0) kb_cat(out_src,  0, src_cap,  g_kb[best].source);
    if (out_topic) *out_topic = g_kb[best].topic;
    return 1;
}

// ---------------------------------------------------------------------------
// System-prompt builder.
//   matched   -> inject the verified fact (RAG context).
//   unmatched -> instruct the model to cite authoritative sources, not guess.
// ---------------------------------------------------------------------------

int kb_build_prompt(const char* question, char* out_prompt, int cap){
    if (!out_prompt || cap <= 0) return 0;
    out_prompt[0] = 0;

    const char* base =
        "你是一个知识严谨的中文助手。回答事实类问题时，优先依据下方提供的『已知事实』。";

    char fact[1024]; char src[256];
    int hit = kb_lookup(question, fact, (int)sizeof(fact), src, (int)sizeof(src), 0);

    int len = 0;
    len = kb_cat(out_prompt, len, cap, base);
    if (hit){
        len = kb_cat(out_prompt, len, cap, "【已知事实】");
        len = kb_cat(out_prompt, len, cap, fact);
        len = kb_cat(out_prompt, len, cap, "（来源：");
        len = kb_cat(out_prompt, len, cap, src);
        len = kb_cat(out_prompt, len, cap, "）请严格依据上述事实作答，不得与之矛盾或自行编造。");
    } else {
        len = kb_cat(out_prompt, len, cap,
            "若问题超出已验证知识库，请勿臆测或编造；应说明需要查阅权威机构或官方来源"
            "（如企业工商信息、政府官网、论文原文）核实后再作答。");
    }
    return hit;
}
