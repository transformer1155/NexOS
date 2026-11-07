/* ai_coop.cpp -- see ai_coop.h.  Freestanding, no libc.
 *
 * PARSER STYLE: scanning, not DOM.  This is the same approach nexos_ai.cpp
 * uses for /apps.json -- walk the text, match a key, read its value from a
 * bounded region.  A full JSON tree would need an allocator, and the schema
 * is flat enough that scanning is both smaller and easier to audit.
 */

#include "ai_coop.h"
#include "ai_tooluse.h"   /* ai_local_tool_fn + ai_tooluse_set_local_dispatch */

static int s_len(const char* s){ int n = 0; if (s) while (s[n]) n++; return n; }
static int s_eq(const char* a, const char* b){
    int i = 0; while (a[i] && b[i]){ if (a[i] != b[i]) return 0; i++; } return a[i] == 0 && b[i] == 0;
}
static void s_cpy(char* d, const char* s, int cap){
    int i = 0; if (cap <= 0) return;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static int is_ws(char c){ return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static void skip_ws(const char** p){ while (**p && is_ws(**p)) (*p)++; }

/* Match a literal at *p; advance past it on success. */
static int lit(const char** p, const char* s){
    int i = 0;
    while (s[i]){ if ((*p)[i] != s[i]) return 0; i++; }
    *p += i;
    return 1;
}

/* Read a JSON string value from a region, into dst.  Stops at the closing
 * quote or at the region end.  Handles the escapes we emit in templates. */
static int rd_string(const char* p, const char* end, char* dst, int cap){
    int o = 0;
    if (!p || p >= end || *p != '"') return 0;
    p++;
    while (p < end && *p && *p != '"'){
        char c = *p++;
        if (c == '\\' && p < end){
            char e = *p++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else c = e;
        }
        if (o < cap - 1) dst[o++] = c;
    }
    dst[o] = 0;
    return 1;
}

static int rd_int(const char* p, const char* end, int def){
    int v = 0, neg = 0, any = 0;
    if (!p || p >= end) return def;
    if (*p == '-'){ neg = 1; p++; }
    while (p < end && *p >= '0' && *p <= '9'){ v = v * 10 + (*p++ - '0'); any = 1; }
    if (!any) return def;
    return neg ? -v : v;
}

static int rd_bool(const char* p, const char* end, int def){
    if (!p || p >= end) return def;
    const char* q = p;
    if (lit(&q, "true") && q <= end) return 1;
    q = p;
    if (lit(&q, "false") && q <= end) return 0;
    return def;
}

/* Locate a top-level "key" in [b,e); returns pointer just after the ':' or 0. */
static const char* find_key(const char* b, const char* e, const char* key){
    char pat[COOP_NAME + 4];
    int n = 0;
    pat[n++] = '"';
    for (int i = 0; key[i] && n < COOP_NAME + 2; i++) pat[n++] = key[i];
    pat[n++] = '"'; pat[n] = 0;
    const char* p = b;
    while (p < e){
        if (lit(&p, pat)){
            skip_ws(&p);
            if (p < e && *p == ':'){ p++; skip_ws(&p); return p; }
            continue;
        }
        p++;
    }
    return 0;
}

static void get_str_field(const char* b, const char* e, const char* key,
                          char* dst, int cap){
    dst[0] = 0;
    const char* p = find_key(b, e, key);
    if (p) rd_string(p, e, dst, cap);
}
static int get_int_field(const char* b, const char* e, const char* key, int def){
    const char* p = find_key(b, e, key);
    return p ? rd_int(p, e, def) : def;
}
static int get_bool_field(const char* b, const char* e, const char* key, int def){
    const char* p = find_key(b, e, key);
    return p ? rd_bool(p, e, def) : def;
}

/* Find the balanced [...] that follows "key", giving its interior range. */
static int find_array(const char* b, const char* e, const char* key,
                      const char** ab, const char** ae){
    const char* p = find_key(b, e, key);
    if (!p || p >= e || *p != '[') return 0;
    p++;
    const char* start = p;
    int depth = 0, instr = 0;
    while (p < e){
        if (instr){
            if (*p == '\\'){ p += 2; continue; }
            if (*p == '"') instr = 0;
        } else if (*p == '"'){ instr = 1; }
        else if (*p == '[' || *p == '{'){ depth++; }
        else if (*p == ']' || *p == '}'){
            if (depth == 0 && *p == ']'){ *ab = start; *ae = p; return 1; }
            depth--;
        }
        p++;
    }
    return 0;
}

/* Iterate array of objects: returns interior range of the next {...}. */
static const char* next_object(const char** pp, const char* ae,
                               const char** oe){
    const char* p = *pp;
    while (p < ae && *p != '{') p++;
    if (p >= ae) return 0;
    const char* start = p + 1;
    int depth = 0, instr = 0;
    while (p < ae){
        if (instr){
            if (*p == '\\'){ p += 2; continue; }
            if (*p == '"') instr = 0;
        } else if (*p == '"') instr = 1;
        else if (*p == '{') depth++;
        else if (*p == '}'){
            depth--;
            if (depth == 0){ *oe = p; *pp = p + 1; return start; }
        }
        p++;
    }
    return 0;
}

/* Split a comma-separated array of quoted strings into dst[][cap]. */
static int get_string_list(const char* b, const char* e, const char* key,
                           char dst[][COOP_NAME], int max, int cap){
    const char* ab; const char* ae;
    if (!find_array(b, e, key, &ab, &ae)) return 0;
    int n = 0;
    const char* p = ab;
    while (p < ae && n < max){
        while (p < ae && (*p == ' ' || *p == ',' || is_ws(*p))) p++;
        if (p >= ae) break;
        if (*p != '"') break;
        if (!rd_string(p, ae, dst[n], cap)) break;
        /* advance past the string we just read */
        p++;
        while (p < ae && *p != '"'){ if (*p == '\\') p++; p++; }
        if (p < ae) p++;
        n++;
    }
    return n;
}

/* Find the balanced {...} that follows "key" (for nested objects). */
static int find_object(const char* b, const char* e, const char* key,
                       const char** ob, const char** oe){
    const char* p = find_key(b, e, key);
    if (!p || p >= e || *p != '{') return 0;
    p++;
    const char* start = p;
    int depth = 0, instr = 0;
    while (p < e){
        if (instr){
            if (*p == '\\'){ p += 2; continue; }
            if (*p == '"') instr = 0;
        } else if (*p == '"'){ instr = 1; }
        else if (*p == '{') depth++;
        else if (*p == '}'){
            if (depth == 0){ *ob = start; *oe = p; return 1; }
            depth--;
        }
        p++;
    }
    return 0;
}

static int parse_roles(coop_template* t, const char* b, const char* e){
    const char* ab; const char* ae;
    if (!find_array(b, e, "roles", &ab, &ae)) return 0;
    const char* p = ab; const char* oe;
    const char* ob;
    while (t->n_roles < COOP_MAX_ROLES){
        ob = next_object(&p, ae, &oe);
        if (!ob) break;
        coop_role* r = &t->roles[t->n_roles];
        get_str_field(ob, oe, "role", r->role, COOP_NAME);
        r->n_caps = get_string_list(ob, oe, "capability", r->capability, COOP_MAX_REFS, COOP_NAME);
        r->count  = get_int_field(ob, oe, "count", 1);
        get_str_field(ob, oe, "fallback", r->fallback, COOP_NAME);
        if (r->role[0]) t->n_roles++;
    }
    return t->n_roles;
}

static int parse_phases(coop_template* t, const char* b, const char* e){
    const char* ab; const char* ae;
    if (!find_array(b, e, "phases", &ab, &ae)) return 0;
    const char* p = ab; const char* oe; const char* ob;
    while (t->n_phases < COOP_MAX_PHASES){
        ob = next_object(&p, ae, &oe);
        if (!ob) break;
        coop_phase* ph = &t->phases[t->n_phases];
        get_str_field(ob, oe, "phase", ph->phase, COOP_NAME);
        get_str_field(ob, oe, "role",  ph->role,  COOP_NAME);
        get_str_field(ob, oe, "goal",  ph->goal,  COOP_GOAL);
        const char* vab; const char* vae;
        if (find_object(ob, oe, "action", &vab, &vae)){
            get_str_field(vab, vae, "kind",    ph->action_kind, 16);
            get_str_field(vab, vae, "subject", ph->action_subject, COOP_GOAL);
            get_str_field(vab, vae, "template", ph->action_template, COOP_NAME);
        }
        ph->n_in  = get_string_list(ob, oe, "context_in",  ph->context_in,  COOP_MAX_REFS, COOP_NAME);
        ph->n_out = get_string_list(ob, oe, "context_out", ph->context_out, COOP_MAX_REFS, COOP_NAME);
        const char* hab; const char* hae;
        if (find_object(ob, oe, "handoff", &hab, &hae)){
            ph->n_requires = get_string_list(hab, hae, "requires", ph->need, COOP_MAX_REFS, COOP_NAME);
            get_str_field(hab, hae, "to", ph->handoff_to, COOP_NAME);
        }
        if (ph->phase[0]) t->n_phases++;
    }
    return t->n_phases;
}

int coop_parse(coop_template* t, const char* text, const char** reason){
    if (reason) *reason = "ok";
    if (!t || !text){ if (reason) *reason = "null input"; return -1; }
    for (int i = 0; i < (int)sizeof(*t); i++) ((char*)t)[i] = 0;
    const char* b = text;
    const char* e = text + s_len(text);

    t->schema = get_int_field(b, e, "schema", 0);
    get_str_field(b, e, "id", t->id, COOP_NAME);
    get_str_field(b, e, "title", t->title, COOP_GOAL);
    get_str_field(b, e, "version", t->version, 16);
    get_str_field(b, e, "summary", t->summary, COOP_GOAL);

    if (t->schema != 1){ if (reason) *reason = "unsupported schema version"; return -2; }
    if (!t->id[0]){ if (reason) *reason = "missing id"; return -3; }

    /* inputs: name + required flag */
    {
        const char* ab; const char* ae;
        if (find_array(b, e, "inputs", &ab, &ae)){
            const char* p = ab; const char* ob; const char* oe;
            while (t->n_inputs < COOP_MAX_NAMES){
                ob = next_object(&p, ae, &oe);
                if (!ob) break;
                char nm[COOP_NAME]; nm[0] = 0;
                get_str_field(ob, oe, "name", nm, COOP_NAME);
                if (nm[0]){
                    s_cpy(t->inputs[t->n_inputs], nm, COOP_NAME);
                    t->input_required[t->n_inputs] = get_bool_field(ob, oe, "required", 0);
                    t->n_inputs++;
                }
            }
        }
    }

    /* guards */
    {
        const char* gb; const char* ge;
        if (find_object(b, e, "guards", &gb, &ge)){
            t->max_phases = get_int_field(gb, ge, "max_phases", COOP_MAX_PHASES);
            t->require_handoff_artifacts = get_bool_field(gb, ge, "require_handoff_artifacts", 1);
            t->fail_closed = get_bool_field(gb, ge, "fail_closed", 1);
        } else {
            t->max_phases = COOP_MAX_PHASES;
            t->require_handoff_artifacts = 1;
            t->fail_closed = 1;
        }
    }
    {
        const char* sb; const char* se;
        if (find_object(b, e, "skill", &sb, &se))
            t->skill_exposed = get_bool_field(sb, se, "exposed", 0);
    }

    parse_roles(t, b, e);
    parse_phases(t, b, e);
    return 0;
}

int coop_validate(const coop_template* t, const char** reason){
    if (reason) *reason = "ok";
    if (!t){ if (reason) *reason = "null"; return -1; }
    if (t->n_phases == 0){ if (reason) *reason = "no phases"; return -2; }
    if (t->n_roles == 0){ if (reason) *reason = "no roles"; return -3; }
    for (int i = 0; i < t->n_phases; i++){
        const coop_phase* ph = &t->phases[i];
        int found = 0;
        for (int r = 0; r < t->n_roles; r++)
            if (s_eq(t->roles[r].role, ph->role)){ found = 1; break; }
        if (!found){ if (reason) *reason = "phase references unknown role"; return -4; }
        if (s_eq(ph->action_kind, "task") || s_eq(ph->action_kind, "report") ||
            s_eq(ph->action_kind, "template")){ /* ok */ }
        else { if (reason) *reason = "unknown action kind"; return -5; }
    }
    return 0;
}

static int put(char* o, int cap, int at, const char* s){
    while (s && *s){ if (at >= cap - 1) return at; o[at++] = *s++; }
    o[at] = 0; return at;
}

int coop_plan(const coop_template* t, const char* symptom_hint, char* out, int cap){
    if (!t || !out || cap <= 0) return 0;
    out[0] = 0;
    int at = 0;
    at = put(out, cap, at, "[coop] template: ");
    at = put(out, cap, at, t->id);
    at = put(out, cap, at, "  (");
    at = put(out, cap, at, t->title);
    at = put(out, cap, at, ")\n");
    at = put(out, cap, at, "  roles needed:\n");
    for (int i = 0; i < t->n_roles; i++){
        at = put(out, cap, at, "    - ");
        at = put(out, cap, at, t->roles[i].role);
        at = put(out, cap, at, " (capabilities: ");
        for (int k = 0; k < t->roles[i].n_caps; k++){
            if (k) at = put(out, cap, at, ",");
            at = put(out, cap, at, t->roles[i].capability[k]);
        }
        at = put(out, cap, at, t->roles[i].fallback[0] ? "; fallback: " : "");
        at = put(out, cap, at, t->roles[i].fallback);
        at = put(out, cap, at, ")\n");
    }
    at = put(out, cap, at, "  phases (handoff is mandatory):\n");
    for (int i = 0; i < t->n_phases; i++){
        const coop_phase* ph = &t->phases[i];
        at = put(out, cap, at, "    ");
        char nbuf[4]; nbuf[0] = (char)('1' + (i % 9)); nbuf[1] = 0;
        at = put(out, cap, at, nbuf);
        at = put(out, cap, at, ". ");
        at = put(out, cap, at, ph->phase);
        at = put(out, cap, at, " [");
        at = put(out, cap, at, ph->role);
        at = put(out, cap, at, "] ");
        at = put(out, cap, at, ph->action_kind);
        if (ph->n_requires){
            at = put(out, cap, at, "  requires: ");
            for (int k = 0; k < ph->n_requires; k++){
                if (k) at = put(out, cap, at, "+");
                at = put(out, cap, at, ph->need[k]);
            }
        }
        if (ph->handoff_to[0]){
            at = put(out, cap, at, "  -> ");
            at = put(out, cap, at, ph->handoff_to);
        }
        at = put(out, cap, at, "\n");
    }
    if (symptom_hint && symptom_hint[0]){
        at = put(out, cap, at, "  input symptom: ");
        at = put(out, cap, at, symptom_hint);
        at = put(out, cap, at, "\n");
    }
    at = put(out, cap, at, "[coop] stage-1 plan only: no tasks created, no messages sent.\n");
    return at;
}

/* ---- local tool: cooperation.run ------------------------------------ */

static coop_read_fn g_reader = 0;
void coop_set_reader(coop_read_fn fn){ g_reader = fn; }

#ifndef COOP_HOST_TEST
extern "C" int kern_fs_ready(void);
extern "C" int kern_fs_read(const char* name, unsigned char* buf, int bufsize);

static int coop_kern_read(const char* name, char* buf, int cap){
    if (!kern_fs_ready()) return -1;
    int n = kern_fs_read(name, (unsigned char*)buf, cap);
    if (n <= 0 && name[0] != '/'){
        char alt[COOP_NAME + 48];
        int k = 0;
        alt[k++] = '/';
        for (int i = 0; name[i] && k < (int)sizeof(alt) - 1; i++) alt[k++] = name[i];
        alt[k] = 0;
        n = kern_fs_read(alt, (unsigned char*)buf, cap);
    }
    return n;
}

void coop_install_kernel_reader(void){ coop_set_reader(coop_kern_read); }
#endif

/* Declared in ai_tooluse.h; declared again here so this file compiles even if
 * it is unit-tested standalone on the host. */
void ai_tooluse_set_local_dispatch(ai_local_tool_fn fn);

extern "C" const char* coop_run_local(const char* tool, const char* json_args);

void ai_coop_install(void){
#ifndef COOP_HOST_TEST
    coop_install_kernel_reader();
#endif
    ai_tooluse_set_local_dispatch((ai_local_tool_fn)coop_run_local);
}

static char g_out[COOP_OUT_CAP];

/* Build the SFS name for a template id, without snprintf.
 *
 * WHY THERE IS NO DIRECTORY PREFIX: the SFS directory entry name field is
 * 20 bytes (19 chars + NUL) -- see sfs_gen.py and kernel64.cpp FS_NAME_LEN.
 * The packer SILENTLY truncates anything longer (sfs_gen.py keeps only [:19]),
 * while the reader does an exact full-name compare, so a name that gets
 * truncated on write can never be found on read.  "ai/cooperation/" is 15
 * chars and ".json" is 5, so ANY prefixed path exceeds 19 before the id is
 * even appended -- every template was unaddressable, regardless of its id.
 * Templates therefore live at the SFS root as coop-<id>.json
 * ("coop-" + 4 + ".json" = 14 chars for a 4-char id). */
static void template_path(const char* id, char* out, int cap){
    const char* pre = "coop-";
    const char* suf = ".json";
    int n = 0;
    for (int i = 0; pre[i] && n < cap - 1; i++) out[n++] = pre[i];
    for (int i = 0; id && id[i] && n < cap - 1; i++) out[n++] = id[i];
    for (int i = 0; suf[i] && n < cap - 1; i++) out[n++] = suf[i];
    out[n] = 0;
}

const char* coop_run_local(const char* tool, const char* json_args){
    (void)tool;
    g_out[0] = 0;
    if (!g_reader){
        s_cpy(g_out, "{\"code\":5,\"error\":\"no reader registered\"}", (int)sizeof(g_out));
        return g_out;
    }
    /* find \"template\" : \"<id>\" in the args */
    const char* b = json_args ? json_args : "";
    const char* e = b + s_len(b);
    char id[COOP_NAME]; id[0] = 0;
    get_str_field(b, e, "template", id, COOP_NAME);
    if (!id[0]){
        s_cpy(g_out, "{\"code\":4,\"error\":\"missing template\"}", (int)sizeof(g_out));
        return g_out;
    }

    static char tpl_text[COOP_TPL_CAP];  /* shipped template measured 3,085 B on disk */
    char path[COOP_NAME + 32];
    template_path(id, path, (int)sizeof(path));
    int n = g_reader(path, tpl_text, (int)sizeof(tpl_text) - 1);
    if (n <= 0){
        /* Report the path actually attempted.  Three rounds were spent on this
           failure because the old message merged 'not mounted' with 'name
           mismatch' and never showed what was looked up. */
        s_cpy(g_out, "{\"code\":2,\"error\":\"template not found: \"", (int)sizeof(g_out));
        int at = s_len(g_out);
        for (int i = 0; path[i] && at < (int)sizeof(g_out) - 3; i++) g_out[at++] = path[i];
        g_out[at++] = '"'; g_out[at++] = '}'; g_out[at] = 0;
        return g_out;
    }
    /* A read that fills the buffer is almost certainly a truncated template.
       Parsing half a JSON file yields a confusing error, so refuse here and
       name the real cause. This is exactly the failure I shipped once by
       sizing the buffer (2,560) below the template (3,085). */
    if (n >= (int)sizeof(tpl_text) - 1){
        s_cpy(g_out, "{\"code\":2,\"error\":\"template too large for COOP_TPL_CAP\"}", (int)sizeof(g_out));
        return g_out;
    }
    tpl_text[n] = 0;

    static coop_template tpl;
    const char* why = "?";
    if (coop_parse(&tpl, tpl_text, &why) != 0){
        s_cpy(g_out, "{\"code\":2,\"error\":\"parse failed: ", (int)sizeof(g_out));
        int at = s_len(g_out);
        for (int i = 0; why[i] && at < (int)sizeof(g_out) - 3; i++) g_out[at++] = why[i];
        g_out[at++] = '"'; g_out[at++] = '}'; g_out[at] = 0;
        return g_out;
    }
    if (coop_validate(&tpl, &why) != 0){
        s_cpy(g_out, "{\"code\":2,\"error\":\"invalid: ", (int)sizeof(g_out));
        int at = s_len(g_out);
        for (int i = 0; why[i] && at < (int)sizeof(g_out) - 3; i++) g_out[at++] = why[i];
        g_out[at++] = '"'; g_out[at++] = '}'; g_out[at] = 0;
        return g_out;
    }

    char symptom[COOP_GOAL]; symptom[0] = 0;
    get_str_field(b, e, "symptom", symptom, COOP_GOAL);

    static char plan[COOP_PLAN_CAP];
    coop_plan(&tpl, symptom, plan, (int)sizeof(plan) - 1);
    /* JSON-escape the plan into g_out (stage-1 returns text, not a task list) */
    s_cpy(g_out, "{\"code\":0,\"plan\":\"", (int)sizeof(g_out));
    int at = s_len(g_out);
    for (int i = 0; plan[i] && at < (int)sizeof(g_out) - 4; i++){
        char ch = plan[i];
        if (ch == '"' || ch == '\\'){ g_out[at++] = '\\'; g_out[at++] = ch; }
        else if (ch == '\n'){ g_out[at++] = '\\'; g_out[at++] = 'n'; }
        else if (ch == '\r'){ /* drop */ }
        else g_out[at++] = ch;
    }
    g_out[at++] = '"'; g_out[at++] = '}'; g_out[at] = 0;
    return g_out;
}

/* Capability match: an agent satisfies a role if it declares EVERY capability
 * the role requires.  Missing one is a rejection, not a partial credit --
 * a reproducer who cannot read the serial log will not produce evidence. */
static int has_cap(const char* csv, const char* cap){
    int cl = s_len(cap);
    for (int i = 0; csv[i]; i++){
        if (i > 0 && csv[i-1] != ',') continue;
        int j = 0;
        while (j < cl && csv[i + j] == cap[j]) j++;
        if (j == cl && (csv[i + cl] == 0 || csv[i + cl] == ',')) return 1;
    }
    return 0;
}

int coop_match_role(const coop_template* t, const coop_role* r,
                    const char* const* names, const char* const* caps, int n_agents){
    (void)t;
    if (!r || !names || !caps) return -1;
    for (int a = 0; a < n_agents; a++){
        int ok = 1;
        for (int k = 0; k < r->n_caps; k++)
            if (!has_cap(caps[a], r->capability[k])){ ok = 0; break; }
        if (ok) return a;
    }
    return -1;
}
