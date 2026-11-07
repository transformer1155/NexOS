/* ai_context.cpp -- see ai_context.h. */

#include "ai_context.h"

static int s_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }

void ai_context_reset(ai_context* c){
    if (!c) return;
    c->head = 0; c->count = 0; c->dropped = 0;
    if (c->budget == 0) c->budget = AI_CTX_BUDGET;
}

/* Copy at most cap-1 bytes, never splitting a UTF-8 sequence: walk back over
 * continuation bytes (10xxxxxx).  Truncating mid-glyph shows up as mojibake in
 * the prompt and measurably degrades small models. */
static void copy_utf8(char* d, const char* s, int cap){
    int n = 0;
    if (cap <= 0) return;
    while (s && s[n] && n < cap - 1) n++;
    if (s && s[n]){
        while (n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80) n--;
    }
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
}

int ai_context_add(ai_context* c, ai_role_t role, const char* text){
    if (!c || !text) return -1;
    if (c->budget == 0) c->budget = AI_CTX_BUDGET;

    int idx;
    if (c->count < AI_CTX_MAX_TURNS){
        idx = (c->head + c->count) % AI_CTX_MAX_TURNS;
        c->count++;
    } else {
        idx = c->head;
        c->head = (c->head + 1) % AI_CTX_MAX_TURNS;
        c->dropped++;
    }
    c->turns[idx].role = (unsigned char)role;
    copy_utf8(c->turns[idx].text, text, AI_CTX_TURN_CHARS);
    return 0;
}

static int put(char* out, int cap, int at, const char* s){
    while (s && *s){
        if (at >= cap - 1) return at;
        out[at++] = *s++;
    }
    out[at] = 0;
    return at;
}

static const char* role_tag(unsigned char r){
    if (r == AI_ROLE_ASSISTANT) return "assistant: ";
    if (r == AI_ROLE_TOOL) return "tool: ";
    return "user: ";
}

int ai_context_render_recent(const ai_context* c, int n, char* out, int cap){
    if (!c || !out || cap <= 0) return 0;
    if (c->budget == 0) return 0;
    out[0] = 0;

    int start = 0;
    if (n > 0 && n < c->count) start = c->count - n;

    int at = 0;
    if (c->dropped > 0 && start == 0){
        at = put(out, cap, at, "[earlier turns omitted]\n");
        if (at >= cap - 1) return at;
    }

    /* Walk newest-first to respect the budget, then emit oldest-first. */
    int used = 0, first = -1;
    for (int i = c->count - 1; i >= start; i--){
        int idx = (c->head + i) % AI_CTX_MAX_TURNS;
        int cost = s_len(c->turns[idx].text) + 16;
        if (used + cost > c->budget) break;
        used += cost;
        first = i;
    }
    if (first < 0) first = c->count - 1;   /* always include at least one */

    for (int i = first; i < c->count; i++){
        int idx = (c->head + i) % AI_CTX_MAX_TURNS;
        at = put(out, cap, at, role_tag(c->turns[idx].role));
        if (at >= cap - 1) break;
        at = put(out, cap, at, c->turns[idx].text);
        if (at >= cap - 1) break;
        at = put(out, cap, at, "\n");
        if (at >= cap - 1) break;
    }
    return at;
}

int ai_context_render(const ai_context* c, char* out, int cap){
    return ai_context_render_recent(c, 0, out, cap);
}

int ai_context_trim(ai_context* c, int keep){
    if (!c || keep < 0) return -1;
    if (keep >= c->count) return 0;
    int drop = c->count - keep;
    c->head = (c->head + drop) % AI_CTX_MAX_TURNS;
    c->count = keep;
    c->dropped += drop;
    return drop;
}

int ai_context_count(const ai_context* c){ return c ? c->count : 0; }
int ai_context_dropped(const ai_context* c){ return c ? c->dropped : 0; }
