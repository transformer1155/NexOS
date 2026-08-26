// kb.cpp - dynamic, rule-driven knowledge base (32-bit freestanding kernel).
//
// See kb.h for the owner's principle. This file implements the store, the
// practice-first / >=3-web-source acceptance rule, and a small RAG retriever
// used by the AI reasoning framework (ai_engine.cpp -> ai_reason()).
//
// No libc under the kernel's freestanding flags: minimal string helpers only.

#include "kb.h"

// ---------------------------------------------------------------------------
// Minimal freestanding string helpers
// ---------------------------------------------------------------------------
static int kb_strlen(const char* s){ int n = 0; while (s && s[n]) n++; return n; }
static int kb_tolower(int c){ if (c >= 'A' && c <= 'Z') return c + 32; return c; }

static int kb_streq_ci(const char* a, const char* b){
    if (!a || !b) return 0;
    while (*a && *b){
        if (kb_tolower((unsigned char)*a) != kb_tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

static void kb_cpy(char* d, const char* s, int cap){
    if (!d || cap <= 0) return;
    int i = 0;
    if (s) while (s[i] && i < cap - 1){ d[i] = s[i]; i++; }
    d[i] = 0;
}

// Case-insensitive substring search.
static int kb_stristr(const char* hay, const char* needle){
    if (!hay || !needle || !*needle) return 1;
    int nlen = kb_strlen(needle), hlen = kb_strlen(hay);
    if (nlen > hlen) return 0;
    for (int i = 0; i <= hlen - nlen; i++){
        int ok = 1;
        for (int j = 0; j < nlen; j++)
            if (kb_tolower((unsigned char)hay[i + j]) != kb_tolower((unsigned char)needle[j])){ ok = 0; break; }
        if (ok) return 1;
    }
    return 0;
}

// Tiny printf: supports %d and %s (enough for KB reporting).
static int kb_sprintf(char* buf, int cap, const char* fmt, ...){
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int pos = 0;
    for (const char* p = fmt; *p && pos < cap - 1; p++){
        if (*p == '%' && p[1]){
            p++;
            if (*p == 'd'){
                int v = __builtin_va_arg(ap, int);
                if (v < 0){ buf[pos++] = '-'; v = -v; }
                char t[12]; int n = 0;
                if (v == 0) t[n++] = '0';
                while (v){ t[n++] = (char)('0' + v % 10); v /= 10; }
                while (n > 0 && pos < cap - 1) buf[pos++] = t[--n];
            } else if (*p == 's'){
                const char* s = __builtin_va_arg(ap, const char*);
                if (s) while (*s && pos < cap - 1) buf[pos++] = *s++;
            } else {
                buf[pos++] = *p;
            }
        } else {
            buf[pos++] = *p;
        }
    }
    buf[pos] = 0;
    __builtin_va_end(ap);
    return pos;
}

// ---------------------------------------------------------------------------
// Fact storage
// ---------------------------------------------------------------------------
typedef struct {
    char statement[KB_STMT_LEN];
    int  status;        // kb_status
    int  provenance;    // kb_prov
    int  practice;      // -1 unknown, 0 fail, 1 pass
    int  n_cites;
    char cite_src[KB_MAX_CITES][KB_SRC_LEN];
    char cite_exc[KB_MAX_CITES][KB_EXC_LEN];
} kb_fact_t;

static kb_fact_t g_facts[KB_MAX_FACTS];
static int       g_kb_count = 0;
static int       g_kb_ready = 0;

const char* kb_status_name(int status){
    return (status == KB_ACCEPTED) ? "ACCEPTED"
         : (status == KB_REJECTED) ? "REJECTED"
                                   : "CANDIDATE";
}
const char* kb_prov_name(int prov){
    return (prov == KB_PROV_PRACTICE) ? "practice"
         : (prov == KB_PROV_WEB)      ? "web"
                                      : "none";
}

static int kb_find(int idx){            // idx is 1-based id
    if (idx < 1 || idx > g_kb_count) return -1;
    return idx - 1;
}

int kb_fact_count(void){ return g_kb_count; }
int kb_accepted(void){
    int n = 0;
    for (int i = 0; i < g_kb_count; i++) if (g_facts[i].status == KB_ACCEPTED) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------
int kb_add(const char* statement){
    if (!statement || !*statement) return -1;
    if (g_kb_count >= KB_MAX_FACTS) return -1;
    for (int i = 0; i < g_kb_count; i++)           // de-dupe identical claims
        if (kb_streq_ci(g_facts[i].statement, statement)) return i + 1;
    int idx = g_kb_count++;
    kb_fact_t* f = &g_facts[idx];
    kb_cpy(f->statement, statement, KB_STMT_LEN);
    f->status     = KB_CANDIDATE;
    f->provenance = KB_PROV_NONE;
    f->practice   = -1;
    f->n_cites    = 0;
    return idx + 1;
}

// Promote if the acceptance rule is satisfied.
static void kb_promote(kb_fact_t* f){
    if (f->status == KB_ACCEPTED) return;
    if (f->provenance == KB_PROV_PRACTICE && f->practice == 1){
        f->status = KB_ACCEPTED;          // 真理来源于实践
        return;
    }
    if (f->n_cites >= KB_WEB_MIN){
        f->status = KB_ACCEPTED;          // >=3 web sources all state the view
        if (f->provenance != KB_PROV_PRACTICE) f->provenance = KB_PROV_WEB;
    }
}

int kb_prove(int idx, int pass){
    int fi = kb_find(idx);
    if (fi < 0) return -1;
    kb_fact_t* f = &g_facts[fi];
    f->practice = pass ? 1 : 0;
    if (pass){
        f->provenance = KB_PROV_PRACTICE;
        f->status     = KB_ACCEPTED;       // practice confirms it directly
    } else {
        // Practice failed -> not truth-from-practice. Needs web corroboration.
        if (f->n_cites >= KB_WEB_MIN){ f->status = KB_ACCEPTED; f->provenance = KB_PROV_WEB; }
        else f->status = KB_CANDIDATE;
    }
    return f->status;
}

int kb_cite(int idx, const char* src, const char* exc){
    int fi = kb_find(idx);
    if (fi < 0) return -1;
    kb_fact_t* f = &g_facts[fi];
    if (!src || !*src) return f->status;
    for (int i = 0; i < f->n_cites; i++)   // de-dupe by source
        if (kb_streq_ci(f->cite_src[i], src)) return f->status;
    if (f->n_cites >= KB_MAX_CITES) return f->status;
    int ci = f->n_cites++;
    kb_cpy(f->cite_src[ci], src, KB_SRC_LEN);
    kb_cpy(f->cite_exc[ci], exc ? exc : "", KB_EXC_LEN);
    kb_promote(f);
    return f->status;
}

// ---------------------------------------------------------------------------
// Retrieval (RAG)
// ---------------------------------------------------------------------------
int kb_query(const char* question, char* out, int cap){
    if (out && cap > 0) out[0] = 0;
    if (!question) return 0;
    int best = -1, best_score = 0;
    for (int i = 0; i < g_kb_count; i++){
        if (g_facts[i].status != KB_ACCEPTED) continue;   // only trusted facts
        // Score = number of 2-char windows of the statement that appear in the
        // question. This works for both CJK (no word boundaries) and Latin text.
        int score = 0;
        const char* stmt = g_facts[i].statement;
        int sl = kb_strlen(stmt);
        for (int a = 0; a + 2 <= sl; a++){
            char w[3];
            w[0] = stmt[a]; w[1] = stmt[a + 1]; w[2] = 0;
            if (kb_stristr(question, w)) score++;
        }
        if (score > best_score){ best_score = score; best = i; }
    }
    if (best < 0 || best_score == 0) return 0;
    if (out && cap > 0) kb_cpy(out, g_facts[best].statement, cap);
    return 1;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
int kb_list(char* out, int cap){
    if (!out || cap <= 0) return 0;
    int pos = 0;
    pos += kb_sprintf(out + pos, cap - pos,
                      "Knowledge Base: %d facts, %d accepted (accept rule: practice OR >=%d web sources)\n",
                      g_kb_count, kb_accepted(), KB_WEB_MIN);
    for (int i = 0; i < g_kb_count; i++){
        kb_fact_t* f = &g_facts[i];
        pos += kb_sprintf(out + pos, cap - pos,
                          " #%d [%s/%s] cites=%d: %s\n",
                          i + 1, kb_status_name(f->status), kb_prov_name(f->provenance),
                          f->n_cites, f->statement);
    }
    return pos;
}

int kb_info(int idx, char* out, int cap){
    int fi = kb_find(idx);
    if (fi < 0 || !out || cap <= 0) return 0;
    kb_fact_t* f = &g_facts[fi];
    int pos = 0;
    pos += kb_sprintf(out + pos, cap - pos, "#%d %s\n", idx, f->statement);
    pos += kb_sprintf(out + pos, cap - pos,
                      "  status=%s provenance=%s practice=%s cites=%d\n",
                      kb_status_name(f->status), kb_prov_name(f->provenance),
                      f->practice > 0 ? "pass" : (f->practice < 0 ? "unknown" : "fail"),
                      f->n_cites);
    for (int i = 0; i < f->n_cites; i++)
        pos += kb_sprintf(out + pos, cap - pos, "   [%d] %s :: %s\n",
                          i + 1, f->cite_src[i], f->cite_exc[i]);
    return pos;
}

// ---------------------------------------------------------------------------
// Seed: curated, already-verified facts.
//   * NexOS        -> KB_PROV_PRACTICE (we built and ran it).
//   * Cardio claim -> KB_PROV_WEB with >=3 REAL independent authoritative
//                     sources (AHA / WHO / NHLBI), gathered by web search.
// ---------------------------------------------------------------------------
static void kb_setcite(kb_fact_t* f, int ci, const char* s, const char* e){
    if (ci < 0 || ci >= KB_MAX_CITES) return;
    kb_cpy(f->cite_src[ci], s, KB_SRC_LEN);
    kb_cpy(f->cite_exc[ci], e ? e : "", KB_EXC_LEN);
}

void kb_init(void){
    if (g_kb_ready) return;
    g_kb_count = 0;
    g_kb_ready = 1;

    // Seed 1: practice-verified (this very OS was built and booted).
    int i = kb_add("NexOS 是本项目的自研爱好型操作系统，含32位与64位双内核、自研GUI与C#托管Shell");
    if (i > 0){
        kb_fact_t* f = &g_facts[i - 1];
        f->provenance = KB_PROV_PRACTICE;
        f->practice   = 1;
        f->status     = KB_ACCEPTED;
    }

    // Seed 2: web-corroborated with >=3 real independent authoritative sources.
    i = kb_add("规律运动可降低心血管疾病风险 (regular exercise lowers cardiovascular disease risk)");
    if (i > 0){
        kb_fact_t* f = &g_facts[i - 1];
        kb_setcite(f, 0, "AHA (Circulation)",
                   "Physical inactivity is an independent risk factor for coronary artery disease.");
        kb_setcite(f, 1, "WHO Guidelines on Physical Activity",
                   "Higher levels of physical activity are associated with lower risk of cardiovascular disease mortality.");
        kb_setcite(f, 2, "NHLBI (US NIH)",
                   "Physical activity lowers your risk for coronary heart disease.");
        f->n_cites    = 3;
        f->provenance = KB_PROV_WEB;
        f->status      = KB_ACCEPTED;     // rule satisfied: 3 sources state the view
    }
}
