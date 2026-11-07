/* ai_config.cpp -- see ai_config.h.  Freestanding, no libc. */

#include "ai_config.h"

static int s_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static int s_eqn(const char* a, const char* b, int n){
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int s_eq(const char* a, const char* b){
    int n = 0; while (a[n] && b[n]) n++;
    return (a[n] == 0 && b[n] == 0) ? s_eqn(a, b, n) : 0;
}
static void s_cpy(char* d, const char* s, int cap){
    int i = 0;
    if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int is_space(char c){
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static int to_int(const char* s){
    int v = 0, neg = 0;
    if (*s == '-'){ neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

const char* ai_provider_name(ai_provider_t p){
    switch (p){
        case AI_PROVIDER_LOCAL:    return "local";
        case AI_PROVIDER_OPENCODE: return "opencode";
        case AI_PROVIDER_DEEPSEEK: return "deepseek";
        case AI_PROVIDER_CUSTOM:   return "custom";
    }
    return "?";
}

int ai_provider_from_name(const char* s){
    if (!s) return -1;
    if (s_eq(s, "local"))    return AI_PROVIDER_LOCAL;
    if (s_eq(s, "opencode")) return AI_PROVIDER_OPENCODE;
    if (s_eq(s, "deepseek")) return AI_PROVIDER_DEEPSEEK;
    if (s_eq(s, "custom"))   return AI_PROVIDER_CUSTOM;
    return -1;
}

static void set_defaults(ai_config_t* c){
    c->provider = AI_PROVIDER_LOCAL;
    c->key[0] = 0;
    s_cpy(c->model, "qwen2-0.5b", AI_CFG_MAX_MODEL);
    s_cpy(c->host, "api.deepseek.com", AI_CFG_MAX_HOST);
    c->max_tokens = 512;
    c->temperature_x10 = 7;
    c->dirty = 0;
    c->enterprise = 0;
    c->locked = 0;
}

/* Strip leading/trailing whitespace in place; returns pointer into s. */
static char* trim(char* s){
    while (*s && is_space(*s)) s++;
    int n = s_len(s);
    while (n > 0 && is_space(s[n-1])) s[--n] = 0;
    return s;
}

int ai_config_set(ai_config_t* c, const char* name, const char* value){
    if (!c || !name) return -1;
    /* Enterprise write lock: provider/host/key are exactly the fields an
       employee could abuse (point at their own endpoint, swap in their key).
       Refuse the whole setter rather than allow cosmetic edits. */
    if (c->locked) return -3;
    if (s_eq(name, "provider")){
        int p = ai_provider_from_name(value ? value : "");
        if (p < 0) return -2;
        c->provider = (ai_provider_t)p;
        return 0;
    }
    if (s_eq(name, "key")){
        s_cpy(c->key, value ? value : "", AI_CFG_MAX_KEY);
        return 0;
    }
    if (s_eq(name, "model")){
        if (!value || !value[0]) return -2;
        s_cpy(c->model, value, AI_CFG_MAX_MODEL);
        return 0;
    }
    if (s_eq(name, "host")){
        if (!value || !value[0]) return -2;
        s_cpy(c->host, value, AI_CFG_MAX_HOST);
        return 0;
    }
    if (s_eq(name, "max_tokens")){
        int v = to_int(value ? value : "0");
        if (v < 16 || v > 8192) return -2;
        c->max_tokens = v;
        return 0;
    }
    if (s_eq(name, "temperature")){
        int v = to_int(value ? value : "0");
        if (v < 0 || v > 20) return -2;
        c->temperature_x10 = v;
        return 0;
    }
    return -1;
}

int ai_config_parse(ai_config_t* out, const char* text){
    if (!out) return -1;
    set_defaults(out);
    if (!text) return 0;

    /* Work on a bounded copy so we can mutate in place. */
    char line[256];
    int li = 0;
    for (const char* p = text; ; p++){
        if (*p == '\n' || *p == 0){
            line[li] = 0;
            char* s = trim(line);
            if (*s && *s != '#'){
                char* eq = s;
                while (*eq && *eq != '=' && !is_space(*eq)) eq++;
                char sep = *eq;
                *eq = 0;
                char* val = eq + 1;
                if (sep == 0) val = eq;   /* bare name: leave value empty */
                val = trim(val);
                if (ai_config_set(out, s, val) != 0) out->dirty = 1;
            }
            li = 0;
            if (*p == 0) break;
            continue;
        }
        if (li < (int)sizeof(line) - 1) line[li++] = *p;
    }
    return 0;
}

static int put(char* buf, int cap, int* at, const char* s){
    while (s && *s){
        if (*at >= cap - 1) return -1;
        buf[(*at)++] = *s++;
    }
    return 0;
}
static int put_int(char* buf, int cap, int* at, int v){
    char t[16]; int n = 0;
    if (v < 0){ if (put(buf, cap, at, "-") < 0) return -1; v = -v; }
    if (v == 0) t[n++] = '0';
    else while (v){ t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n){ char c[2]; c[0] = t[--n]; c[1] = 0; if (put(buf, cap, at, c) < 0) return -1; }
    return 0;
}

int ai_config_serialize(const ai_config_t* c, char* buf, int cap){
    if (!c || !buf || cap <= 0) return -1;
    int at = 0;
    if (put(buf, cap, &at, "# NexOS AI configuration\n") < 0) return -1;
    if (put(buf, cap, &at, "provider=") < 0) return -1;
    if (put(buf, cap, &at, ai_provider_name(c->provider)) < 0) return -1;
    if (put(buf, cap, &at, "\nmodel=") < 0) return -1;
    if (put(buf, cap, &at, c->model) < 0) return -1;
    if (put(buf, cap, &at, "\nhost=") < 0) return -1;
    if (put(buf, cap, &at, c->host) < 0) return -1;
    if (put(buf, cap, &at, "\nmax_tokens=") < 0) return -1;
    if (put_int(buf, cap, &at, c->max_tokens) < 0) return -1;
    if (put(buf, cap, &at, "\ntemperature=") < 0) return -1;
    if (put_int(buf, cap, &at, c->temperature_x10) < 0) return -1;
    if (c->key[0]){
        if (put(buf, cap, &at, "\nkey=") < 0) return -1;
        if (put(buf, cap, &at, c->key) < 0) return -1;
    }
    if (put(buf, cap, &at, "\n") < 0) return -1;
    return at;
}

/* Mask a credential: keep a 4-char prefix and 2-char suffix. */
static void mask_key(const char* key, char* out, int cap){
    int n = s_len(key);
    if (n == 0){ s_cpy(out, "(none)", cap); return; }
    if (n <= 10){ s_cpy(out, "****", cap); return; }
    int at = 0;
    for (int i = 0; i < 4 && at < cap - 1; i++) out[at++] = key[i];
    for (int i = 0; i < 4 && at < cap - 1; i++) out[at++] = '*';
    for (int i = n - 2; i < n && at < cap - 1; i++) out[at++] = key[i];
    out[at] = 0;
}

int ai_config_describe(const ai_config_t* c, char* buf, int cap){
    if (!c || !buf || cap <= 0) return -1;
    int at = 0;
    char mk[64];
    mask_key(c->key, mk, (int)sizeof(mk));
    if (put(buf, cap, &at, "provider="     ) < 0) return -1;
    if (put(buf, cap, &at, ai_provider_name(c->provider)) < 0) return -1;
    if (put(buf, cap, &at, "\n  model="   ) < 0) return -1;
    if (put(buf, cap, &at, c->model) < 0) return -1;
    if (put(buf, cap, &at, "\n  host="    ) < 0) return -1;
    if (put(buf, cap, &at, c->host) < 0) return -1;
    if (put(buf, cap, &at, "\n  key="     ) < 0) return -1;
    if (put(buf, cap, &at, mk) < 0) return -1;
    if (put(buf, cap, &at, "\n  max_tokens=" ) < 0) return -1;
    if (put_int(buf, cap, &at, c->max_tokens) < 0) return -1;
    if (put(buf, cap, &at, "\n  temperature=" ) < 0) return -1;
    if (put_int(buf, cap, &at, c->temperature_x10 / 10) < 0) return -1;
    if (put(buf, cap, &at, ".") < 0) return -1;
    if (put_int(buf, cap, &at, c->temperature_x10 % 10) < 0) return -1;
    if (c->dirty) { if (put(buf, cap, &at, "\n  [warnings: some lines rejected]") < 0) return -1; }
    return at;
}

int ai_config_ready(const ai_config_t* c){
    if (!c) return 0;
    if (c->provider == AI_PROVIDER_LOCAL) return 1;
    if (c->provider == AI_PROVIDER_OPENCODE) return 1;   /* anonymous quota */
    if (c->provider == AI_PROVIDER_DEEPSEEK) return c->key[0] != 0;
    return c->key[0] != 0 && c->host[0] != 0;
}
/* ---- enterprise write lock ----------------------------------------- */

void ai_config_set_enterprise(ai_config_t* c, int on){
    if (!c) return;
    c->enterprise = on ? 1 : 0;
    if (c->enterprise) c->locked = 1;   /* enterprise implies locked by default */
}

int ai_config_is_enterprise(const ai_config_t* c){ return c ? c->enterprise : 0; }
int ai_config_is_locked(const ai_config_t* c){ return c ? c->locked : 0; }

/* Only reachable from the ring-0 console path; ordinary callers have no
 * business unlocking a managed configuration. */
int ai_config_admin_unlock(ai_config_t* c){ if (!c) return -1; c->locked = 0; return 0; }
int ai_config_admin_lock(ai_config_t* c){
    if (!c) return -1;
    if (!c->enterprise) return -2;      /* locking outside enterprise is a no-op */
    c->locked = 1;
    return 0;
}
