/* ai_market.cpp -- see ai_market.h.  Freestanding, no libc. */

#include "ai_market.h"
#include "tls_crypto.h"      /* sha256() -- already linked in KERNEL64_OBJS */

/* =====================================================================
 * 1. freestanding helpers
 * ===================================================================== */
static int m_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static int m_eqn(const char* a, const char* b, int n){
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int m_eq(const char* a, const char* b){
    int n = 0; if (!a || !b) return a == b;
    while (a[n] && b[n]) n++;
    return (a[n] == 0 && b[n] == 0) ? m_eqn(a, b, n) : 0;
}
static void m_cpy(char* d, const char* s, int cap){
    if (cap <= 0) return;
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static void m_cat(char* b, int* p, int cap, const char* s){
    while (s && *s && *p < cap - 1) b[(*p)++] = *s++;
    b[*p] = 0;
}
static void m_int(char* b, int* p, int cap, int v){
    if (*p >= cap - 1) { b[*p] = 0; return; }
    if (v < 0) { m_cat(b, p, cap, "-"); v = -v; }
    char t[12]; int n = 0;
    if (v == 0) t[n++] = 48;
    while (v > 0 && n < 11) { t[n++] = (char)(48 + v % 10); v /= 10; }
    while (n > 0 && *p < cap - 1) b[(*p)++] = t[--n];
    b[*p] = 0;
}
static int is_space(char c){ return c == 32 || c == 9 || c == 13 || c == 10; }
static int to_lower(int c){ return (c >= 65 && c <= 90) ? c + 32 : c; }

void ai_market_sha256_hex(const void* data, int len, char out[AI_MARKET_SHA_MAX]){
    uint8_t d[32];
    const char* hx = "0123456789abcdef";
    sha256(data, len, d);
    for (int i = 0; i < 32; i++){
        out[i * 2]     = hx[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = hx[d[i] & 0xF];
    }
    out[64] = 0;
}

int ai_market_sha_equal(const char* a, const char* b){
    if (!a || !b) return 0;
    if (m_len(a) != 64 || m_len(b) != 64) return 0;
    for (int i = 0; i < 64; i++) if (to_lower(a[i]) != to_lower(b[i])) return 0;
    return 1;
}

/* =====================================================================
 * 2. a very small, defensive JSON reader
 *
 * The registry is untrusted, so this is a scanner, not a parser: it walks
 * the text once, never allocates, never trusts a length field, and skips
 * anything it does not understand.  Strings are handled with a single level
 * of backslash escaping; no unicode decoding is attempted (ids and paths are
 * ASCII by construction).
 * ===================================================================== */

/* Skip a JSON string starting at *pp (which must point at the opening quote).
 * On return *pp points past the closing quote, or at the NUL if unterminated. */
static void skip_string(const char** pp, const char* end){
    const char* p = *pp;
    if (p >= end || *p != 34) return;
    p++;
    while (p < end && *p){
        if (*p == 92) { p += 2; continue; }   /* escaped char */
        if (*p == 34) { p++; break; }
        p++;
    }
    *pp = p;
}

/* Copy the string value of "key" inside [obj, obj_end) into out.  Returns 1
 * when the key was found (out is always NUL-terminated), 0 otherwise. */
static int json_str(const char* obj, const char* obj_end, const char* key,
                    char* out, int cap){
    int klen = m_len(key);
    if (cap > 0) out[0] = 0;
    for (const char* p = obj; p && p < obj_end; p++){
        if (*p != 34) continue;
        const char* ks = p + 1;
        const char* ke = ks;
        while (ke < obj_end && *ke && *ke != 34 && *ke != 92) ke++;
        if (*ke != 34) break;
        if ((int)(ke - ks) != klen || !m_eqn(ks, key, klen)) { p = ke; continue; }
        /* found "key" -- now expect : "value" */
        const char* q = ke + 1;
        while (q < obj_end && is_space(*q)) q++;
        if (q >= obj_end || *q != 58) continue;   /* colon */
        q++;
        while (q < obj_end && is_space(*q)) q++;
        if (q >= obj_end) return 0;
        if (*q == 34){
            const char* vs = q + 1;
            const char* ve = vs;
            while (ve < obj_end && *ve && *ve != 34){
                if (*ve == 92 && (ve + 1) < obj_end) ve += 2; else ve++;
            }
            int n = 0;
            for (const char* s = vs; s < ve && n < cap - 1; s++){
                if (*s == 92 && (s + 1) < ve){
                    s++;
                    char c = *s;
                    if (c == 110) c = 10; else if (c == 116) c = 9; else if (c == 114) c = 13;
                    out[n++] = c;
                } else {
                    out[n++] = *s;
                }
            }
            if (cap > 0) out[n] = 0;
            return 1;
        }
        /* non-string scalar: copy the token verbatim */
        const char* vs = q;
        while (q < obj_end && *q && *q != 44 && *q != 125 && !is_space(*q)) q++;
        int n = 0;
        for (const char* s = vs; s < q && n < cap - 1; s++) out[n++] = *s;
        if (cap > 0) out[n] = 0;
        return 1;
    }
    return 0;
}

/* First object inside the array value of "key": sets os/oe.  Returns 1. */
static int json_first_object(const char* txt, const char* end, const char* key,
                             const char** os, const char** oe){
    int klen = m_len(key);
    for (const char* p = txt; p < end; p++){
        if (*p != 34) continue;
        const char* ks = p + 1;
        const char* ke = ks;
        while (ke < end && *ke && *ke != 34) ke++;
        if ((int)(ke - ks) != klen || !m_eqn(ks, key, klen)) { p = ke; continue; }
        const char* q = ke + 1;
        while (q < end && is_space(*q)) q++;
        if (q >= end || *q != 58) continue;
        q++;
        while (q < end && is_space(*q)) q++;
        if (q >= end || *q != 91) continue;   /* [ */
        q++;
        while (q < end && is_space(*q)) q++;
        if (q >= end || *q != 123) return 0;  /* { */
        *os = q;
        /* find matching } */
        int depth = 0;
        const char* r = q;
        while (r < end && *r){
            if (*r == 34) { skip_string(&r, end); continue; }
            if (*r == 123) depth++;
            else if (*r == 125) { depth--; if (depth == 0) { *oe = r + 1; return 1; } }
            r++;
        }
        return 0;
    }
    return 0;
}

/* Walk to the next sibling object after *p (which sits at a closing brace or
 * just after one).  Returns 1 and sets os/oe when another object is found. */
static int json_next_object(const char* p, const char* end, const char** os, const char** oe){
    while (p < end && *p && *p != 123) p++;      /* { */
    if (p >= end || *p != 123) return 0;
    *os = p;
    int depth = 0;
    while (p < end && *p){
        if (*p == 34) { skip_string(&p, end); continue; }
        if (*p == 123) depth++;
        else if (*p == 125) { depth--; if (depth == 0) { *oe = p + 1; return 1; } }
        p++;
    }
    return 0;
}
/* =====================================================================
 * 3. registry -> app table
 * ===================================================================== */

/* Digest buffer strategy (approved design).
 *
 * A market that verifies downloads needs the bytes in one place.  Reserving
 * 64 KB of .bss for a module that nothing calls yet is wasteful, so:
 *   - the static fallback is small (16 KB), covers small .mex/.drv artefacts;
 *   - a dist bigger than that is verified through a temporary allocation when
 *     the caller supplies alloc/free;
 *   - without an allocator such a dist is REFUSED (code -10) rather than
 *     truncated, because a digest over partial bytes is worse than no digest.
 * A streaming hash (no buffer at all) stays a Stage 1 TODO. */
/* NO static dist buffer.  This module contributed 16 KB of .bss that pushed the
 * 64-bit image past HEAP_START (see [PMM] FATAL in the boot serial), so the
 * buffer is now purely caller-supplied: the verifier allocates through the
 * alloc/free hooks and releases immediately.  Without those hooks a claim is
 * REFUSED (-10) -- an explicit refusal beats a silent truncation, and it beats
 * quietly overlapping the kernel .bss with the heap.
 * A streaming hash (no buffer at all) stays a Stage 1 TODO. */

/* Hand out a buffer able to hold need bytes.  Returns NULL when that is not
 * possible; owned says whether the caller must release it via hooks.free. */
static char* dist_buffer(ai_market_ctx* m, int need, int* owned, int* cap){
    *owned = 0; *cap = 0;
    if (need <= 0) return 0;
    if (m->hooks.alloc){
        char* p = (char*)m->hooks.alloc(need);
        if (p){ *owned = 1; *cap = need; return p; }
    }
    return 0;
}

static void m_zero(void* p, int n){
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = 0;
}

static void set_err(ai_market_ctx* m, const char* s){
    if (m) m_cpy(m->last_error, s, AI_MARKET_ERR_MAX);
}

/* Split "a:b:c" into up to n fields (no allocation).  Returns field count. */
static int split_colon(const char* s, char f[][AI_MARKET_PERM_MAX], int n){
    int cnt = 0, i = 0;
    if (n <= 0) return 0;
    f[0][0] = 0;
    for (; *s; s++){
        if (*s == 58){                 /* : */
            f[cnt][i] = 0;
            cnt++;
            if (cnt >= n) return cnt;
            i = 0;
            f[cnt][0] = 0;
            continue;
        }
        if (i < AI_MARKET_PERM_MAX - 1) f[cnt][i++] = *s;
    }
    f[cnt][i] = 0;
    return cnt + 1;
}

/* Read the "perms" array: a list of permission strings.  Grammar (documented):
 *     tool:<short_name>:<app_id>:<tool>      -> whitelist entry
 *     mcp:<app_id>:<tool>                    -> MCP registration + whitelist
 * Anything else is kept for display but grants nothing. */
static int parse_perms(const char* os, const char* oe, ai_market_app* a){
    int klen = 5;                                 /* "perms" */
    for (const char* p = os; p < oe; p++){
        if (*p != 34) continue;
        const char* ks = p + 1;
        const char* ke = ks;
        while (ke < oe && *ke && *ke != 34) ke++;
        if ((int)(ke - ks) != klen || !m_eqn(ks, "perms", klen)) { p = ke; continue; }
        const char* q = ke + 1;
        while (q < oe && is_space(*q)) q++;
        if (q < oe && *q == 58) q++;
        while (q < oe && is_space(*q)) q++;
        if (q >= oe || *q != 91) return 0;        /* [ */
        q++;
        while (q < oe && *q && *q != 93){         /* ] */
            while (q < oe && (is_space(*q) || *q == 44)) q++;
            if (q >= oe || *q == 93) break;
            if (*q != 34) { q++; continue; }
            const char* vs = q + 1;
            const char* ve = vs;
            while (ve < oe && *ve && *ve != 34){ if (*ve == 92) ve++; ve++; }
            if (a->perm_count < AI_MARKET_PERMS_MAX){
                int n = 0;
                for (const char* s = vs; s < ve && n < AI_MARKET_PERM_MAX - 1; s++) a->perms[a->perm_count][n++] = *s;
                a->perms[a->perm_count][n] = 0;
                a->perm_count++;
            }
            q = ve + 1;
        }
        return a->perm_count;
    }
    return 0;
}

static int parse_app(const char* os, const char* oe, ai_market_app* a){
    m_zero(a, (int)sizeof(*a));
    if (!json_str(os, oe, "id", a->id, AI_MARKET_ID_MAX)) return 0;
    if (!a->id[0]) return 0;
    if (!json_str(os, oe, "name", a->name, AI_MARKET_NAME_MAX) || !a->name[0])
        m_cpy(a->name, a->id, AI_MARKET_NAME_MAX);
    json_str(os, oe, "version", a->version, AI_MARKET_VER_MAX);
    json_str(os, oe, "dist", a->dist, AI_MARKET_DIST_MAX);
    json_str(os, oe, "sha256", a->sha256, AI_MARKET_SHA_MAX);
    parse_perms(os, oe, a);
    return 1;
}

void ai_market_init(ai_market_ctx* m, const ai_market_hooks* hooks){
    if (!m) return;
    m_zero(m, (int)sizeof(*m));
    if (hooks) m->hooks = *hooks;
    m->source_url[0] = 0;
}

int ai_market_load(ai_market_ctx* m, const char* json){
    if (!m) return -1;
    m->app_count = 0;
    if (!json || !json[0]) { set_err(m, "empty registry"); return -1; }
    int len = m_len(json);
    const char* end = json + len;
    const char* os = 0; const char* oe = 0;
    if (!json_first_object(json, end, "apps", &os, &oe)) {
        set_err(m, "no apps[] in registry");
        return -1;
    }
    int n = 0;
    const char* p = os;
    while (n < AI_MARKET_MAX_APPS && p && p < end){
        if (!json_next_object(p, end, &os, &oe)) break;
        if (parse_app(os, oe, &m->apps[n])) n++;
        p = oe;
    }
    m->app_count = n;
    if (n == 0) set_err(m, "no usable apps in registry");
    return n;
}

int ai_market_find(const ai_market_ctx* m, const char* id){
    if (!m || !id) return -1;
    for (int i = 0; i < m->app_count; i++) if (m_eq(m->apps[i].id, id)) return i;
    return -1;
}

int ai_market_list(const ai_market_ctx* m, char* buf, int cap){
    if (!m || !buf || cap <= 0) return -1;
    int p = 0; buf[0] = 0;
    for (int i = 0; i < m->app_count; i++){
        const ai_market_app* a = &m->apps[i];
        m_cat(buf, &p, cap, a->id);
        m_cat(buf, &p, cap, "|");
        m_cat(buf, &p, cap, a->name[0] ? a->name : a->id);
        m_cat(buf, &p, cap, "|");
        m_cat(buf, &p, cap, a->version[0] ? a->version : "?");
        m_cat(buf, &p, cap, "|");
        m_int(buf, &p, cap, a->claimed);
        m_cat(buf, &p, cap, "|");
        m_int(buf, &p, cap, a->enabled);
        m_cat(buf, &p, cap, "|");
        m_int(buf, &p, cap, a->sha_ok);
        m_cat(buf, &p, cap, "\n");
    }
    return p;
}
/* =====================================================================
 * 4. persistence (small, diffable, hand-editable)
 * ===================================================================== */

static char g_statebuf[1024];      /* static: kernel stacks are small.  1024 B holds
                                    * claimed/enabled lines for 24 apps with room to spare. */
/* No registry buffer here: the caller supplies it (see ai_market_sync_buf). */

int ai_market_state(const ai_market_ctx* m, char* buf, int cap){
    if (!m || !buf || cap <= 0) return -1;
    int p = 0; buf[0] = 0;
    for (int i = 0; i < m->app_count; i++){
        const ai_market_app* a = &m->apps[i];
        if (a->claimed) { m_cat(buf, &p, cap, "claimed="); m_cat(buf, &p, cap, a->id); m_cat(buf, &p, cap, "\n"); }
        if (a->enabled) { m_cat(buf, &p, cap, "enabled="); m_cat(buf, &p, cap, a->id); m_cat(buf, &p, cap, "\n"); }
    }
    return p;
}

int ai_market_restore(ai_market_ctx* m, const char* text){
    if (!m || !text) return 0;
    int applied = 0;
    const char* p = text;
    while (*p){
        while (*p == 10 || *p == 13 || *p == 32 || *p == 9) p++;
        if (!*p) break;
        char key[24]; int kl = 0;
        while (*p && *p != 61 && *p != 10 && *p != 13 && kl < 23) key[kl++] = *p++;
        key[kl] = 0;
        if (*p != 61) { while (*p && *p != 10) p++; continue; }   /* = */
        p++;
        char val[AI_MARKET_ID_MAX]; int vl = 0;
        while (*p && *p != 10 && *p != 13 && vl < AI_MARKET_ID_MAX - 1) val[vl++] = *p++;
        val[vl] = 0;
        int i = ai_market_find(m, val);
        if (i < 0) continue;
        if (m_eq(key, "claimed")) { m->apps[i].claimed = 1; applied++; }
        else if (m_eq(key, "enabled")) { m->apps[i].enabled = 1; applied++; }
    }
    return applied;
}

static void persist(ai_market_ctx* m){
    if (!m || !m->hooks.fs_write) return;
    int n = ai_market_state(m, g_statebuf, (int)sizeof(g_statebuf));
    if (n > 0) m->hooks.fs_write(AI_MARKET_STATE_PATH, g_statebuf, n);
}

/* =====================================================================
 * 5. Stage 0 entry point
 * ===================================================================== */

int ai_market_sync_buf(ai_market_ctx* m, char* scratch, int scratch_cap){
    if (!m) return -1;
    m->app_count = 0;
    if (!m->hooks.fs_read) { set_err(m, "no fs_read hook"); return -1; }
    if (!scratch || scratch_cap < 64) {
        set_err(m, "sync needs a caller scratch buffer (>= 64 bytes)");
        return -10;
    }
    int n = m->hooks.fs_read(AI_MARKET_REG_PATH, scratch, scratch_cap);
    if (n <= 0) { set_err(m, "registry not found in SFS"); return -1; }
    if (n >= scratch_cap) {
        /* The document may be truncated: refuse instead of parsing half a file.
         * A half registry would silently drop apps, which is worse than an error. */
        set_err(m, "registry larger than the caller scratch buffer: refusing");
        return -10;
    }
    if (ai_market_load(m, scratch) < 0) return -1;
    /* state survives reboots: the claim/enable flags are the whole memory */
    int ns = m->hooks.fs_read(AI_MARKET_STATE_PATH, g_statebuf, (int)sizeof(g_statebuf));
    if (ns > 0) ai_market_restore(m, g_statebuf);
    return m->app_count;
}

/* =====================================================================
 * 6. claim / enable
 * ===================================================================== */

/* SFS path of an app's dist file inside the market cache. */
static int dist_path(const ai_market_app* a, char* out, int cap){
    int p = 0;
    if (cap <= 0) return -1;
    out[0] = 0;
    m_cat(out, &p, cap, AI_MARKET_CACHE_DIR);
    m_cat(out, &p, cap, a->dist);
    return (a->dist[0] && p > 0) ? 0 : -1;
}

int ai_market_claim(ai_market_ctx* m, const char* id){
    int i = ai_market_find(m, id);
    if (i < 0) { set_err(m, "unknown app"); return -1; }
    ai_market_app* a = &m->apps[i];
    if (a->claimed) return 0;                     /* idempotent */
    if (m_len(a->sha256) != 64) { set_err(m, "app has no sha256 digest"); return -2; }
    if (!a->dist[0]) { set_err(m, "app has no dist filename"); return -2; }
    if (!m->hooks.fs_read) { set_err(m, "no fs_read: cannot verify the dist"); return -6; }
    if (!m->hooks.plugin_register) { set_err(m, "no plugin_register hook"); return -7; }
    char path[AI_MARKET_DIST_MAX + 40];
    dist_path(a, path, (int)sizeof(path));

    /* Size the buffer exactly when the FS can tell us; otherwise use the small
     * fallback and treat a full buffer as "possibly truncated".  Both paths
     * REFUSE instead of verifying partial bytes. */
    int owned = 0, cap = 0;
    int need = m->hooks.fs_size ? m->hooks.fs_size(path) : -1;
    if (need <= 0) {
        /* Without a size query there is no safe buffer size, and this build has
         * no static fallback: refuse rather than guess. */
        set_err(m, "cannot size the dist (no fs_size hook): refusing to verify");
        return -10;
    }
    char* buf = dist_buffer(m, need, &owned, &cap);
    if (!buf) {
        set_err(m, "dist verification needs an alloc/free hook (none supplied)");
        return -10;
    }
    int n = m->hooks.fs_read(path, buf, cap);
    if (n <= 0) { if (owned) m->hooks.free(buf); set_err(m, "dist not cached yet"); return -3; }
    if (n > cap) {
        if (owned) m->hooks.free(buf);
        set_err(m, "dist read larger than the buffer: refusing");
        return -10;
    }
    char hex[AI_MARKET_SHA_MAX];
    ai_market_sha256_hex(buf, n, hex);
    m->sha_checked++;
    if (!ai_market_sha_equal(hex, a->sha256)) {
        if (owned) m->hooks.free(buf);
        a->sha_ok = -1;
        m->sha_failed++;
        set_err(m, "sha256 mismatch: refusing to register");
        return -4;
    }
    a->sha_ok = 1;
    if (m->hooks.plugin_register(a->id, a->name[0] ? a->name : a->id) < 0) {
        if (owned) m->hooks.free(buf);
        set_err(m, "plugin_register failed");
        return -8;
    }
    if (owned) m->hooks.free(buf);
    a->claimed = 1;
    persist(m);
    return 0;
}

int ai_market_enable(ai_market_ctx* m, const char* id){
    int i = ai_market_find(m, id);
    if (i < 0) { set_err(m, "unknown app"); return -1; }
    ai_market_app* a = &m->apps[i];
    if (!a->claimed) { set_err(m, "claim the app first"); return -5; }
    int granted = 0;
    int skipped = 0;   /* mcp:/unknown permissions: recognised but not granted */
    for (int k = 0; k < a->perm_count; k++){
        char f[4][AI_MARKET_PERM_MAX];
        int nf = split_colon(a->perms[k], f, 4);
        if (nf >= 4 && m_eq(f[0], "tool")){
            if (m->hooks.tool_allow && m->hooks.tool_allow(f[1], f[2], f[3]) >= 0) granted++;
        } else if (nf >= 3 && m_eq(f[0], "mcp")){
            /* STAGE 0 RULING (kernel-architect): a permission that needs the MCP
             * bus is NOT granted here.  Making an app resident requires the
             * MiniCLR loader to endorse its id (nexos_ai.h:126-131), and only
             * the loader may do that -- wiring it is a separate, reviewed task.
             * Granting a tool the model cannot actually reach would be a lie,
             * so we count it as skipped and let the caller report it. */
            skipped++;
        }
        /* an unrecognised permission grants NOTHING: safe by default */
    }
    a->enabled = 1;
    a->granted_count = granted;
    a->skipped_count = skipped;
    persist(m);
    return granted;
}

int ai_market_disable(ai_market_ctx* m, const char* id){
    int i = ai_market_find(m, id);
    if (i < 0) { set_err(m, "unknown app"); return -1; }
    /* NOTE: this clears the flag but does not yet revoke whitelist entries --
     * ai_tooluse has no remove API.  A follow-up should add ai_tooluse_revoke();
     * until then disable() stops the app being re-enabled implicitly, and the
     * operator can restart the AI session to drop the whitelist. */
    m->apps[i].enabled = 0;
    persist(m);
    return 0;
}

const char* ai_market_error(const ai_market_ctx* m){
    return m ? m->last_error : "";
}
