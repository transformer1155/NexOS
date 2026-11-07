/* ai_kb.cpp -- see ai_kb.h.  Freestanding, no libc. */

#include "ai_kb.h"

static int s_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static void s_cpy(char* d, const char* s, int cap){
    int i = 0; if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static char lower(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int contains_ci(const char* hay, const char* needle){
    if (!hay || !needle || !needle[0]) return 0;
    int nl = s_len(needle);
    for (int i = 0; hay[i]; i++){
        int j = 0;
        while (j < nl && hay[i + j] && lower(hay[i + j]) == lower(needle[j])) j++;
        if (j == nl) return 1;
    }
    return 0;
}

void ai_kb_reset(ai_kb* kb){
    if (!kb) return;
    kb->n = 0; kb->rejected = 0; kb->dirty = 0;
}

static int fact_duplicate(const ai_kb* kb, const char* fact){
    for (int i = 0; i < kb->n; i++){
        int same = 1;
        for (int k = 0; k < 64; k++){
            char a = kb->entries[i].fact[k];
            char b = fact[k];
            if (a != b){ same = 0; break; }
            if (a == 0) break;
        }
        if (same) return 1;
    }
    return 0;
}

int ai_kb_add(ai_kb* kb, const char* topic, const char* fact,
              const char* source, const char* keys_csv, int builtin){
    if (!kb || !topic || !fact) return -1;
    /* A fact without a source is an assertion, and a small model will repeat
       it as truth.  Refuse it. */
    if (!source || !source[0]){ kb->rejected++; return -2; }
    if (kb->n >= AI_KB_MAX_ENTRIES){ kb->rejected++; return -4; }
    if (fact_duplicate(kb, fact)){ kb->rejected++; return -3; }

    ai_kb_entry* e = &kb->entries[kb->n];
    s_cpy(e->topic, topic, AI_KB_TOPIC_LEN);
    s_cpy(e->fact, fact, AI_KB_FACT_LEN);
    s_cpy(e->source, source, AI_KB_SRC_LEN);
    e->n_keys = 0;
    e->builtin = (uint8_t)(builtin ? 1 : 0);

    if (keys_csv && keys_csv[0]){
        int i = 0;
        while (keys_csv[i] && e->n_keys < AI_KB_KEYS_PER){
            char key[AI_KB_KEY_LEN]; int n = 0;
            while (keys_csv[i] == ' ') i++;
            while (keys_csv[i] && keys_csv[i] != ',' && n < AI_KB_KEY_LEN - 1)
                key[n++] = keys_csv[i++];
            key[n] = 0;
            while (n > 0 && key[n-1] == ' ') key[--n] = 0;
            if (n > 0){
                s_cpy(e->keys[e->n_keys], key, AI_KB_KEY_LEN);
                e->n_keys++;
            }
            if (keys_csv[i] == ',') i++;
        }
    }
    if (e->n_keys == 0){
        s_cpy(e->keys[0], topic, AI_KB_KEY_LEN);
        e->n_keys = 1;
    }
    kb->n++;
    return 0;
}

int ai_kb_lookup(const ai_kb* kb, const char* question,
                 char* fact_out, int fact_cap,
                 char* src_out, int src_cap,
                 char* topic_out, int topic_cap,
                 int* score_out){
    if (fact_out && fact_cap > 0) fact_out[0] = 0;
    if (src_out && src_cap > 0) src_out[0] = 0;
    if (topic_out && topic_cap > 0) topic_out[0] = 0;
    if (score_out) *score_out = 0;
    if (!kb || !question) return 0;

    int best = -1, best_score = 0;
    for (int i = 0; i < kb->n; i++){
        const ai_kb_entry* e = &kb->entries[i];
        int score = 0;
        for (int k = 0; k < e->n_keys; k++)
            if (contains_ci(question, e->keys[k])) score++;
        if (score > best_score){ best_score = score; best = i; }
    }
    if (best < 0) return 0;
    if (fact_out && fact_cap > 0) s_cpy(fact_out, kb->entries[best].fact, fact_cap);
    if (src_out && src_cap > 0)   s_cpy(src_out,  kb->entries[best].source, src_cap);
    if (topic_out && topic_cap > 0) s_cpy(topic_out, kb->entries[best].topic, topic_cap);
    if (score_out) *score_out = best_score;
    return 1;
}

static int put(char* out, int cap, int at, const char* s){
    while (s && *s){ if (at >= cap - 1) return at; out[at++] = *s++; }
    out[at] = 0; return at;
}

int ai_kb_build_prompt(const ai_kb* kb, const char* question,
                       char* out, int cap){
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    char fact[AI_KB_FACT_LEN], src[AI_KB_SRC_LEN], topic[AI_KB_TOPIC_LEN];
    int score = 0;
    int hit = ai_kb_lookup(kb, question, fact, (int)sizeof(fact),
                           src, (int)sizeof(src), topic, (int)sizeof(topic), &score);
    int at = 0;
    at = put(out, cap, at, "你是一个知识严谨的中文助手。\n");
    if (hit){
        at = put(out, cap, at, "【已知事实】");
        at = put(out, cap, at, fact);
        at = put(out, cap, at, "（来源：");
        at = put(out, cap, at, src);
        at = put(out, cap, at, "）请严格依据上述事实作答，不得与之矛盾或自行编造。\n");
    } else {
        at = put(out, cap, at,
                 "若问题超出已验证知识库，请勿臆测或编造；"
                 "应说明需要查阅权威机构或官方来源核实后再作答。\n");
    }
    return hit;
}

int ai_kb_serialize(const ai_kb* kb, char* buf, int cap){
    if (!kb || !buf || cap <= 0) return 0;
    int at = 0;
    at = put(buf, cap, at, "# topic|fact|source|keys\n");
    for (int i = 0; i < kb->n; i++){
        const ai_kb_entry* e = &kb->entries[i];
        at = put(buf, cap, at, e->topic);  at = put(buf, cap, at, "|");
        at = put(buf, cap, at, e->fact);   at = put(buf, cap, at, "|");
        at = put(buf, cap, at, e->source); at = put(buf, cap, at, "|");
        for (int k = 0; k < e->n_keys; k++){
            if (k) at = put(buf, cap, at, ",");
            at = put(buf, cap, at, e->keys[k]);
        }
        at = put(buf, cap, at, "\n");
    }
    return at;
}

/* Split a line into up to 4 fields on '|'.  Trailing spaces trimmed. */
static int split4(const char* line, int len, char* f, int* fl, int fn){
    int fi = 0, pos = 0;
    for (int k = 0; k < fn; k++) f[k * 1024] = 0, fl[k] = 0;
    for (int i = 0; i <= len; i++){
        int end = (i == len) || (line[i] == '|');
        if (end){
            if (fi >= fn) return fi;
            int n = pos;
            while (n > 0 && (f[fi * 1024 + n - 1] == ' ' || f[fi * 1024 + n - 1] == '\r')) n--;
            f[fi * 1024 + n] = 0; fl[fi] = n;
            fi++; pos = 0;
        } else if (pos < 1023){
            f[fi * 1024 + pos++] = line[i];
        }
    }
    return fi;
}

int ai_kb_parse(ai_kb* kb, const char* text){
    if (!kb) return -1;
    ai_kb_reset(kb);
    if (!text) return 0;
    static char fields[4][1024];
    int flen[4];
    const char* p = text;
    while (*p){
        const char* start = p;
        while (*p && *p != '\n') p++;
        int len = (int)(p - start);
        if (*p) p++;
        if (len > 0 && start[0] != '#'){
            int got = split4(start, len, &fields[0][0], flen, 4);
            if (got >= 3 && flen[0] > 0 && flen[1] > 0){
                ai_kb_add(kb, fields[0], fields[1], fields[2], got >= 4 ? fields[3] : 0, 0);
            } else {
                kb->rejected++;
            }
        }
    }
    return 0;
}

int ai_kb_count(const ai_kb* kb){ return kb ? kb->n : 0; }
int ai_kb_rejected(const ai_kb* kb){ return kb ? kb->rejected : 0; }
