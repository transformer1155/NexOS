/* ai_redact.cpp -- see ai_redact.h.  Freestanding, no libc. */

#include "ai_redact.h"

static int s_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static void s_cpy(char* d, const char* s, int cap){
    int i = 0; if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static char lc(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int contains_ci(const char* hay, int hlen, const char* needle){
    int nl = s_len(needle);
    if (nl == 0 || nl > hlen) return 0;
    for (int i = 0; i + nl <= hlen; i++){
        int j = 0;
        while (j < nl && lc(hay[i + j]) == lc(needle[j])) j++;
        if (j == nl) return 1;
    }
    return 0;
}

static char g_canary[AI_REDACT_MAX_CANARY][AI_REDACT_CANARY_LEN];
static int  g_canary_n = 0;

void ai_redact_reset(void){ g_canary_n = 0; }

int ai_redact_add_canary(const char* token){
    if (!token || !token[0]) return -1;
    if (g_canary_n >= AI_REDACT_MAX_CANARY) return -1;
    s_cpy(g_canary[g_canary_n], token, AI_REDACT_CANARY_LEN);
    g_canary_n++;
    return 0;
}

/* Obvious credential / host-secret patterns.  Kept short and specific: a
 * fuzzy rule here would block legitimate prompts and get disabled, which is
 * worse than no rule at all. */
static const char* g_patterns[] = {
    "BEGIN PRIVATE KEY",
    "BEGIN RSA PRIVATE KEY",
    "sk-",
    "api_key",
    "apikey",
    "/etc/passwd",
    "/etc/shadow",
    "authorization: bearer",
};
static const int g_pattern_n = 8;

int ai_redact_check(const char* payload, int len, int enterprise,
                    const char** reason){
    if (reason) *reason = "ok";
    if (!payload || len <= 0) return AI_REDACT_OK;
    if (len > 65536){ if (reason) *reason = "payload too large"; return AI_REDACT_TOO_LONG; }

    for (int i = 0; i < g_pattern_n; i++){
        if (contains_ci(payload, len, g_patterns[i])){
            if (reason) *reason = "contains credential-like text";
            return AI_REDACT_PATTERN;
        }
    }

    /* Canaries are the enterprise guarantee: contents of the private KB must
       never appear in an outbound request.  In personal mode a canary hit is
       still suspicious, so we refuse there too. */
    for (int i = 0; i < g_canary_n; i++){
        if (contains_ci(payload, len, g_canary[i])){
            if (reason) *reason = "contains protected knowledge-base text";
            return AI_REDACT_CANARY;
        }
    }

    (void)enterprise;   /* reserved: stricter rules may apply per edition */
    return AI_REDACT_OK;
}

int ai_redact_gate(const char* payload, int len, int enterprise){
    const char* why = 0;
    return ai_redact_check(payload, len, enterprise, &why);
}
