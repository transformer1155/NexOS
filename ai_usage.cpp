/* ai_usage.cpp -- see ai_usage.h. */

#include "ai_usage.h"

static int s_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static int s_eq(const char* a, const char* b){
    int i = 0; while (a[i] && b[i]){ if (a[i] != b[i]) return 0; i++; } return a[i] == 0 && b[i] == 0;
}
static void s_cpy(char* d, const char* s, int cap){
    int i = 0; if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

void ai_usage_reset(ai_usage* u){
    if (!u) return;
    u->n = 0;
    u->since_boot_requests = 0;
    for (int i = 0; i < AI_USAGE_MAX_PROVIDERS; i++){
        u->rows[i].provider[0] = 0;
        u->rows[i].requests = u->rows[i].failures = 0;
        u->rows[i].chars_in = u->rows[i].chars_out = 0;
        u->rows[i].ms_total = u->rows[i].ms_max = 0;
    }
}

static ai_usage_row* row_for(ai_usage* u, const char* provider, int create){
    if (!u || !provider || !provider[0]) return 0;
    for (int i = 0; i < u->n; i++)
        if (s_eq(u->rows[i].provider, provider)) return &u->rows[i];
    if (!create || u->n >= AI_USAGE_MAX_PROVIDERS) return 0;
    s_cpy(u->rows[u->n].provider, provider, (int)sizeof(u->rows[u->n].provider));
    u->rows[u->n].requests = u->rows[u->n].failures = 0;
    u->rows[u->n].chars_in = u->rows[u->n].chars_out = 0;
    u->rows[u->n].ms_total = u->rows[u->n].ms_max = 0;
    return &u->rows[u->n++];
}

int ai_usage_record(ai_usage* u, const char* provider,
                    uint32_t chars_in, uint32_t chars_out,
                    uint32_t ms, int ok){
    if (!u) return -1;
    ai_usage_row* r = row_for(u, provider, 1);
    if (!r) return -1;   /* table full: drop silently, never fail the call */
    r->requests++;
    if (!ok) r->failures++;
    r->chars_in += chars_in;
    r->chars_out += chars_out;
    r->ms_total += ms;
    if ((uint64_t)ms > r->ms_max) r->ms_max = ms;
    u->since_boot_requests++;
    return 0;
}

static int put(char* buf, int cap, int at, const char* s){
    while (s && *s){ if (at >= cap - 1) return at; buf[at++] = *s++; }
    buf[at] = 0; return at;
}
static int put_u(char* buf, int cap, int at, uint64_t v){
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    else while (v){ t[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n){ char one[2]; one[0] = t[--n]; one[1] = 0; at = put(buf, cap, at, one); }
    return at;
}

int ai_usage_describe(const ai_usage* u, char* buf, int cap){
    if (!u || !buf || cap <= 0) return 0;
    int at = 0;
    at = put(buf, cap, at, "provider     req  fail   chars_in   chars_out   avg_ms  max_ms\n");
    for (int i = 0; i < u->n; i++){
        const ai_usage_row* r = &u->rows[i];
        at = put(buf, cap, at, r->provider);
        at = put(buf, cap, at, "  ");
        at = put_u(buf, cap, at, r->requests);
        at = put(buf, cap, at, "  ");
        at = put_u(buf, cap, at, r->failures);
        at = put(buf, cap, at, "  ");
        at = put_u(buf, cap, at, r->chars_in);
        at = put(buf, cap, at, "  ");
        at = put_u(buf, cap, at, r->chars_out);
        at = put(buf, cap, at, "  ");
        at = put_u(buf, cap, at, r->requests ? r->ms_total / r->requests : 0);
        at = put(buf, cap, at, "  ");
        at = put_u(buf, cap, at, r->ms_max);
        at = put(buf, cap, at, "\n");
    }
    return at;
}

int ai_usage_summary(const ai_usage* u, char* buf, int cap){
    if (!u || !buf || cap <= 0) return 0;
    int at = 0;
    for (int i = 0; i < u->n; i++){
        if (i) at = put(buf, cap, at, " ");
        at = put(buf, cap, at, u->rows[i].provider);
        at = put(buf, cap, at, ":");
        at = put_u(buf, cap, at, u->rows[i].requests);
    }
    return at;
}

int ai_usage_serialize(const ai_usage* u, char* buf, int cap){
    if (!u || !buf || cap <= 0) return 0;
    int at = 0;
    for (int i = 0; i < u->n; i++){
        const ai_usage_row* r = &u->rows[i];
        at = put(buf, cap, at, r->provider);
        at = put(buf, cap, at, "=");
        at = put_u(buf, cap, at, r->requests);   at = put(buf, cap, at, ",");
        at = put_u(buf, cap, at, r->failures);   at = put(buf, cap, at, ",");
        at = put_u(buf, cap, at, r->chars_in);   at = put(buf, cap, at, ",");
        at = put_u(buf, cap, at, r->chars_out);  at = put(buf, cap, at, ",");
        at = put_u(buf, cap, at, r->ms_total);   at = put(buf, cap, at, ",");
        at = put_u(buf, cap, at, r->ms_max);     at = put(buf, cap, at, "\n");
    }
    return at;
}

static uint64_t parse_u(const char** pp){
    uint64_t v = 0;
    while (**pp >= '0' && **pp <= '9') v = v * 10 + (uint64_t)(**pp - '0'), (*pp)++;
    return v;
}

int ai_usage_parse(ai_usage* u, const char* text){
    if (!u) return -1;
    ai_usage_reset(u);
    if (!text) return 0;
    const char* p = text;
    while (*p){
        char name[16]; int n = 0;
        while (*p && *p != '=' && *p != '\n' && n < 15) name[n++] = *p++;
        name[n] = 0;
        if (*p != '='){ while (*p && *p != '\n') p++; if (*p) p++; continue; }
        p++;
        ai_usage_row* r = row_for(u, name, 1);
        if (!r){ while (*p && *p != '\n') p++; if (*p) p++; continue; }
        r->requests  = (uint32_t)parse_u(&p); if (*p == ',') p++;
        r->failures  = (uint32_t)parse_u(&p); if (*p == ',') p++;
        r->chars_in  = parse_u(&p);           if (*p == ',') p++;
        r->chars_out = parse_u(&p);           if (*p == ',') p++;
        r->ms_total  = parse_u(&p);           if (*p == ',') p++;
        r->ms_max    = parse_u(&p);
        if (*p == '\n') p++;
    }
    return 0;
}
/* ---- enterprise quota ---------------------------------------------- */

static ai_quota_row* quota_find(ai_usage* u, const char* agent, int create){
    if (!u || !agent || !agent[0]) return 0;
    for (int i = 0; i < u->n_quota; i++)
        if (s_eq(u->quota[i].agent, agent)) return &u->quota[i];
    if (!create || u->n_quota >= AI_QUOTA_MAX_AGENTS) return 0;
    s_cpy(u->quota[u->n_quota].agent, agent, (int)sizeof(u->quota[u->n_quota].agent));
    u->quota[u->n_quota].daily_tokens = 0;
    u->quota[u->n_quota].used_tokens = 0;
    u->quota[u->n_quota].rejected = 0;
    u->quota[u->n_quota].period_day = 0;
    return &u->quota[u->n_quota++];
}

int ai_usage_quota_set(ai_usage* u, const char* agent, uint32_t tokens){
    if (!u || !agent || !agent[0]) return -1;
    ai_quota_row* r = quota_find(u, agent, 1);
    if (!r) return -2;
    r->daily_tokens = tokens;
    return 0;
}

/* Roll the counter over at a day boundary.  Called from every accessor so a
 * machine left running overnight does not carry yesterday's spend. */
static void quota_maybe_roll(ai_quota_row* r, uint32_t today){
    if (r->period_day != today){
        r->period_day = today;
        r->used_tokens = 0;
        r->rejected = 0;
    }
}

int ai_usage_quota_check(ai_usage* u, const char* agent,
                         uint32_t est_tokens, uint32_t today){
    if (!u || !agent || !agent[0]) return -1;
    ai_quota_row* r = quota_find(u, agent, 0);
    if (!r) return -1;                       /* no quota configured = unknown */
    quota_maybe_roll(r, today);
    if (r->daily_tokens == 0) return 1;      /* unlimited                     */
    if (r->used_tokens >= r->daily_tokens){
        r->rejected++;
        return -2;                           /* refuse before spending        */
    }
    if (r->used_tokens + est_tokens >= r->daily_tokens) return 0;  /* last one */
    return 1;
}

uint32_t ai_usage_quota_charge(ai_usage* u, const char* agent,
                               uint32_t tokens, uint32_t today){
    if (!u || !agent || !agent[0]) return 0;
    ai_quota_row* r = quota_find(u, agent, 1);
    if (!r) return 0;
    quota_maybe_roll(r, today);
    r->used_tokens += tokens;
    if (r->daily_tokens == 0) return 0xFFFFFFFFu;   /* unlimited sentinel */
    if (r->used_tokens >= r->daily_tokens) return 0;
    return r->daily_tokens - r->used_tokens;
}

int ai_usage_quota_exhausted(const ai_usage* u, const char* agent){
    if (!u || !agent || !agent[0]) return 0;
    for (int i = 0; i < u->n_quota; i++)
        if (s_eq(u->quota[i].agent, agent)){
            if (u->quota[i].daily_tokens == 0) return 0;
            return u->quota[i].used_tokens >= u->quota[i].daily_tokens;
        }
    return 0;
}
