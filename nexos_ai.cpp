/* nexos_ai.cpp -- AI-Native gateway + MCP bus, kernel implementation.
 *
 * Freestanding: no libc.  Tiny string helpers are defined locally and the
 * only external symbols used are the host-side hooks ai_reason / ai_set_mode
 * / net_http_get / kern_fs_read plus the built-in service bridges
 * (nexos_mcp_*, browser_*) declared below.
 *
 * ------------------------------------------------------------------------
 *  TABLE-DRIVEN REGISTRY
 * ------------------------------------------------------------------------
 *  There is NO compiled-in seeding.  /apps.json is the single source of
 *  truth: every app, its AI tier, the MCP tools it serves and the apps it
 *  may call are read from the manifest at boot.
 *
 *  What JSON cannot carry is a *function pointer*, so implementations live
 *  in one flat table (g_mcp_impls) keyed by tool name.  The manifest decides
 *  WHICH tools an app exposes and WHO may call them; the table decides HOW
 *  each tool runs.  A tool declared in the manifest with no table entry
 *  falls back to impl_generic (a declarative echo), so adding an app never
 *  requires touching the kernel.
 *
 * ------------------------------------------------------------------------
 *  SECURITY MODEL (anti-forgery)
 * ------------------------------------------------------------------------
 *  A managed app can never choose its caller id.  The MiniCLR records the
 *  basename of the .mex it actually loaded into g_current_app_id and calls
 *  app_registry_note_resident() with it; mcp_dispatch_request() refuses any
 *  caller that is not in that trusted resident set.  A manifest entry is
 *  only a *policy* statement -- it cannot make an app resident, and it
 *  cannot widen another app's access list.
 *
 * ------------------------------------------------------------------------
 *  MEMORY NOTE
 * ------------------------------------------------------------------------
 *  Kernel .bss sits directly below HEAP_START, so every static byte here is
 *  a byte the kernel heap never gets.  The MCP bus is synchronous and
 *  non-reentrant -- one dispatch runs to completion before the next begins,
 *  and a tool's result is copied into the reply envelope before control
 *  returns -- so all tools share one small set of scratch buffers instead of
 *  each carrying its own.
 */
#include "nexos_ai.h"

// Serial debug (port 0x3F8) - freestanding
static void naslog(const char* s){
    while(*s){
        __asm__ __volatile__("outb %0,%1" :: "a"((unsigned char)*s++), "Nd"((unsigned short)0x3F8));
    }
}

extern "C" void* kmalloc(unsigned int);
extern "C" int   ai_reason(const char* prompt, char* out, int outsize);
extern "C" int   ai_set_mode(int mode);
extern "C" int   net_http_get(const char* url, char* out, int outsize);
extern "C" int   kern_fs_read(const char* name, unsigned char* buf, int bufsize);

/* Built-in app services, implemented by the kernel itself.  These are the
 * "app-as-tool" bridges: the Browser/Terminal/File-Manager windows are native
 * code, so their MCP tools call straight into the same routines the GUI uses
 * (browser HTTP client in net.cpp, terminal command dispatcher + file
 * callbacks in kernel.cpp).  Declared extern "C" so the flat dispatch table
 * below can hold plain function pointers. */
extern "C" int  browser_navigate(const char* url);
extern "C" int  browser_status(void);
extern "C" int  browser_get_page(char* buf, int bufsize);

extern "C" int  nexos_mcp_exec(const char* cmd, char* out, int outsize);
extern "C" int  nexos_mcp_fs_list(int fs, char* buf, int bufsize);
extern "C" int  nexos_mcp_fs_read(int fs, const char* name, unsigned char* buf, int bufsize);
extern "C" int  nexos_mcp_fs_write(int fs, const char* name, const unsigned char* buf, int size);
extern "C" int  nexos_mcp_fs_mkdir(int fs, const char* name);
extern "C" int  nexos_mcp_fs_remove(int fs, const char* name);
extern "C" int  nexos_mcp_fs_rename(int fs, const char* old_name, const char* new_name);

/* ---------------- minimal string helpers ---------------- */
static int my_strlen(const char* s){ int n=0; while(s&&s[n]) n++; return n; }
static int my_strcmp(const char* a, const char* b){
    if(!a&&!b) return 0; if(!a) return -1; if(!b) return 1;
    while(*a||*b){ if(*a!=*b) return (unsigned char)*a-(unsigned char)*b; a++; b++; }
    return 0;
}
static void my_strncpy(char* d, const char* s, int n){
    if(!d) return; int i=0; for(; i<n-1 && s && s[i]; i++) d[i]=s[i]; if(n>0) d[i]=0;
}
#include <stdarg.h>
/* Minimal printf-style formatter (kernel is freestanding; no libc sprintf).
   Supports %d %s %c %%.  BOUNDED: never writes past out[size-1], always NUL-
   terminates.  Returns the number of bytes written (excluding NUL).
   my_snprintf replaces the old unbounded my_sprintf. */
static int my_snprintf(char* out, int size, const char* fmt, ...){
    if (!out || size <= 0) return 0;
    va_list ap; va_start(ap, fmt);
    char* p = out;
    char* end = out + size - 1;   /* one slot reserved for NUL */
    auto push = [&](char c){ if (p < end) *p++ = c; };
    for (; fmt && *fmt; ++fmt) {
        if (*fmt != '%') { push(*fmt); continue; }
        char spec = *(++fmt);
        if (spec == '%') { push('%'); continue; }
        if (spec == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s) push(*s++);
            continue;
        }
        if (spec == 'c') { push((char)va_arg(ap, int)); continue; }
        if (spec == 'd') {
            int v = va_arg(ap, int); char t[16]; int n = 0;
            if (v < 0) { push('-'); v = -v; }
            if (v == 0) t[n++] = '0'; else while (v) { t[n++] = '0' + (v % 10); v /= 10; }
            while (n) push(t[--n]);
            continue;
        }
        push('%'); if (spec) push(spec);   /* unknown spec: echo literally */
    }
    *p = 0; va_end(ap);
    return (int)(p - out);
}

/* Persistent string pool (no malloc churn in the kernel).
 * Holds the manifest's app ids/names and per-tool descriptors.  On overflow
 * str_dup returns "" (safe, never NULL). */
static char g_strpool[10240];
static int  g_strpool_used = 0;
static const char* str_dup(const char* s){
    if(!s) s = "";
    int n = my_strlen(s)+1;
    if(g_strpool_used+n > (int)sizeof(g_strpool)) return "";
    char* d = g_strpool + g_strpool_used; g_strpool_used += n;
    my_strncpy(d, s, n); return d;
}

/* Match a literal at *pp; if it matches advance *pp past it and return 1. */
static int match(const char** pp, const char* lit){
    const char* p = *pp; int i=0;
    while(lit[i]){ if(p[i]!=lit[i]) return 0; i++; }
    *pp = p + i; return 1;
}
static void skip_ws(const char** pp){
    const char* p=*pp; while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++; *pp=p;
}
static void skip_ws_colon(const char** pp){
    skip_ws(pp); if(**pp==':'){ (*pp)++; skip_ws(pp); }
}
/* Read a JSON string starting at a double-quote into dst. Advances *pp past
   the closing quote. Returns 1 on success. */
static int read_string(const char** pp, char* dst, int max){
    const char* p=*pp; if(*p!='"') return 0; p++;
    int i=0; while(*p && *p!='"' && i<max-1){ dst[i++]=*p++; } dst[i]=0;
    if(*p=='"') p++; *pp=p; return 1;
}
/* Capture one JSON object/array verbatim into dst (bounded). */
static void capture_raw(const char** pp, char* dst, int max){
    int i = 0;
    skip_ws(pp);
    const char* p = *pp;
    if(*p=='{' || *p=='['){
        char open = *p, close = (open=='{') ? '}' : ']';
        int depth = 0;
        while(*p){
            if(*p == open) depth++;
            else if(*p == close) depth--;
            if(i < max-1) dst[i++] = *p;
            p++;
            if(depth <= 0) break;
        }
        dst[i]=0;
        *pp = p;
        return;
    }
    if(*p=='"'){ read_string(pp, dst, max); return; }
    while(*p && *p!=',' && *p!='}' && *p!=']'){
        if(i < max-1) dst[i++] = *p;
        p++;
    }
    dst[i]=0;
    *pp = p;
}
static int map_mode(const char* s){
    if(my_strcmp(s,"Fast")==0     || my_strcmp(s,"fast")==0)     return AI_FAST;
    if(my_strcmp(s,"Balanced")==0 || my_strcmp(s,"balanced")==0) return AI_BALANCED;
    if(my_strcmp(s,"Heavy")==0    || my_strcmp(s,"heavy")==0)    return AI_HEAVY;
    return AI_BALANCED;
}

/* ---------------- shared scratch buffers ----------------
 * g_raw  : decoded payload (page text, file bytes, command output)
 * g_out  : the JSON a tool hands back to the bus
 * g_env  : the reply envelope ({"code":..,"json":".."}) plus escape scratch
 * g_out and g_env must stay distinct: the envelope is built FROM g_out. */
static char g_raw[1800];
static char g_out[3400];
static char g_env[5400];

/* Append src to dst[*pos] with JSON string escaping, never exceeding cap. */
static void json_escape_into(char* dst, int* pos, const char* src, int cap){
    int j = *pos; const char* q = src ? src : "";
    while(*q && j < cap-2){
        unsigned char c = (unsigned char)*q;
        if(c=='"' || c=='\\'){ dst[j++]='\\'; dst[j++]=(char)c; q++; }
        else if(c=='\n'){ dst[j++]='\\'; dst[j++]='n'; q++; }
        else if(c=='\r'){ dst[j++]='\\'; dst[j++]='r'; q++; }
        else if(c=='\t'){ dst[j++]='\\'; dst[j++]='t'; q++; }
        else if(c < 0x20){ q++; }            /* drop other control chars */
        else { dst[j++]=(char)c; q++; }
    }
    dst[j]=0;
    *pos = j;
}

/* Extract a *string* member from a flat JSON argument object.
 * Returns 1 on success.  Missing/non-string members return 0. */
static int json_get_str(const char* json, const char* key, char* out, int max){
    if(!out || max <= 0) return 0;
    out[0]=0;
    if(!json || !key || !key[0]) return 0;
    char pat[48]; int i=0;
    pat[i++]='"';
    while(*key && i < 44) pat[i++]=*key++;
    pat[i++]='"'; pat[i]=0;
    const char* p = json;
    while(*p){
        if(match(&p, pat)){
            skip_ws_colon(&p);
            return read_string(&p, out, max);
        }
        p++;
    }
    return 0;
}

/* Extract an integer member; accepts both 12 and "12". */
static int json_get_int(const char* json, const char* key, int def){
    if(!json || !key || !key[0]) return def;
    char pat[48]; int i=0;
    pat[i++]='"';
    while(*key && i < 44) pat[i++]=*key++;
    pat[i++]='"'; pat[i]=0;
    const char* p = json;
    while(*p){
        if(match(&p, pat)){
            skip_ws_colon(&p);
            int v=0, neg=0;
            if(*p=='"'){ char tmp[16];
                         if(!read_string(&p,tmp,sizeof(tmp))) return def;
                         const char* q=tmp; if(*q=='-'){neg=1;q++;}
                         while(*q>='0'&&*q<='9'){ v=v*10+(*q-'0'); q++; }
                         return neg?-v:v; }
            if(*p=='-'){neg=1;p++;}
            while(*p>='0'&&*p<='9'){ v=v*10+(*p-'0'); p++; }
            return neg?-v:v;
        }
        p++;
    }
    return def;
}

/* Filesystem selector for the File Manager tools: "sfs" (read-only system
 * volume, default), "mkfs" (writable data disk) or "desktop" (user dir). */
static int parse_fs(const char* json){
    char v[16];
    if(json_get_str(json, "fs", v, sizeof(v))){
        if(my_strcmp(v,"mkfs")==0 || my_strcmp(v,"0")==0)    return 0;
        if(my_strcmp(v,"sfs")==0  || my_strcmp(v,"1")==0)    return 1;
        if(my_strcmp(v,"desktop")==0 || my_strcmp(v,"3")==0) return 3;
    }
    return 1;   /* default: the SFS volume the apps actually live on */
}

/* =====================================================================
 *  Tool implementations (the "how").  The manifest supplies the "what".
 * ===================================================================== */

/* Declarative / demo tools that have no kernel service behind them. */
static const char* impl_generic(const char* src, const char* tgt,
                                const char* tool, const char* args){
    (void)src; (void)tgt;
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"tool\":\"");
    json_escape_into(g_out, &j, tool ? tool : "", (int)sizeof(g_out));
    j += my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\",\"args\":\"");
    json_escape_into(g_out, &j, args ? args : "", (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\",\"implemented\":0}");
    return g_out;
}

static const char* impl_weather(const char* src, const char* tgt,
                                const char* tool, const char* args){
    (void)tgt; (void)tool;
    char loc[96];
    if(!json_get_str(args, "loc", loc, sizeof(loc))) loc[0]=0;
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"temp_c\":21,\"desc\":\"clear\",\"loc\":\"");
    json_escape_into(g_out, &j, loc, (int)sizeof(g_out));
    j += my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\",\"source\":\"");
    json_escape_into(g_out, &j, src ? src : "", (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

/* ---- Browser (built-in app as MCP service) ---- */
static const char* impl_browser_navigate(const char* src, const char* tgt,
                                         const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    char url[256];
    if(!json_get_str(args, "url", url, sizeof(url)) || !url[0])
        return "{\"ok\":0,\"error\":\"missing url\"}";
    int r = browser_navigate(url);
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"url\":\"", r == 0 ? 1 : 0);
    json_escape_into(g_out, &j, url, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

static const char* impl_browser_get_text(const char* src, const char* tgt,
                                         const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool; (void)args;
    int n = browser_get_page(g_raw, (int)sizeof(g_raw)-1);
    if(n < 0) n = 0;
    if(n >= (int)sizeof(g_raw)) n = (int)sizeof(g_raw)-1;
    g_raw[n] = 0;
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"bytes\":%d,\"status\":%d,\"text\":\"",
                       n, browser_status());
    json_escape_into(g_out, &j, g_raw, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

/* ---- Terminal (built-in app as MCP service) ---- */
static const char* impl_terminal_exec(const char* src, const char* tgt,
                                      const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    char cmd[192];
    if(!json_get_str(args, "cmd", cmd, sizeof(cmd)) || !cmd[0])
        return "{\"ok\":0,\"error\":\"missing cmd\"}";
    g_raw[0] = 0;
    int r = nexos_mcp_exec(cmd, g_raw, (int)sizeof(g_raw));
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"cmd\":\"", r == 0 ? 1 : 0);
    json_escape_into(g_out, &j, cmd, (int)sizeof(g_out));
    j += my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\",\"output\":\"");
    json_escape_into(g_out, &j, g_raw, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

/* ---- File Manager (built-in app as MCP service) ---- */
static const char* impl_fs_list(const char* src, const char* tgt,
                                const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    int fs = parse_fs(args);
    g_raw[0] = 0;
    int n = nexos_mcp_fs_list(fs, g_raw, (int)sizeof(g_raw));
    if(n < 0) n = 0;
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"fs\":%d,\"count\":%d,\"entries\":\"", fs, n);
    json_escape_into(g_out, &j, g_raw, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

static const char* impl_fs_read(const char* src, const char* tgt,
                                const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    int fs = parse_fs(args);
    char name[96];
    if(!json_get_str(args, "name", name, sizeof(name)) || !name[0])
        return "{\"ok\":0,\"error\":\"missing name\"}";
    int n = nexos_mcp_fs_read(fs, name, (unsigned char*)g_raw, (int)sizeof(g_raw)-1);
    if(n < 0) n = 0;
    g_raw[n] = 0;
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"fs\":%d,\"bytes\":%d,\"data\":\"",
                       n > 0 ? 1 : 0, fs, n);
    json_escape_into(g_out, &j, g_raw, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

static const char* impl_fs_write(const char* src, const char* tgt,
                                 const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    int fs = parse_fs(args);
    char name[96], data[512];
    if(!json_get_str(args, "name", name, sizeof(name)) || !name[0])
        return "{\"ok\":0,\"error\":\"missing name\"}";
    if(!json_get_str(args, "data", data, sizeof(data))) data[0]=0;
    int n = my_strlen(data);
    int r = nexos_mcp_fs_write(fs, name, (const unsigned char*)data, n);
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"fs\":%d,\"name\":\"",
                       r == 0 ? 1 : 0, fs);
    json_escape_into(g_out, &j, name, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\",\"bytes\":%d}", n);
    return g_out;
}

static const char* impl_fs_mkdir(const char* src, const char* tgt,
                                 const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    int fs = parse_fs(args);
    char name[96];
    if(!json_get_str(args, "name", name, sizeof(name)) || !name[0])
        return "{\"ok\":0,\"error\":\"missing name\"}";
    int r = nexos_mcp_fs_mkdir(fs, name);
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"fs\":%d,\"name\":\"",
                       r == 0 ? 1 : 0, fs);
    json_escape_into(g_out, &j, name, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

static const char* impl_fs_remove(const char* src, const char* tgt,
                                  const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    int fs = parse_fs(args);
    char name[96];
    if(!json_get_str(args, "name", name, sizeof(name)) || !name[0])
        return "{\"ok\":0,\"error\":\"missing name\"}";
    int r = nexos_mcp_fs_remove(fs, name);
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"fs\":%d,\"name\":\"",
                       r == 0 ? 1 : 0, fs);
    json_escape_into(g_out, &j, name, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

static const char* impl_fs_rename(const char* src, const char* tgt,
                                  const char* tool, const char* args){
    (void)src; (void)tgt; (void)tool;
    int fs = parse_fs(args);
    char from[96], to[96];
    if(!json_get_str(args, "from", from, sizeof(from)) || !from[0])
        return "{\"ok\":0,\"error\":\"missing from\"}";
    if(!json_get_str(args, "to", to, sizeof(to)) || !to[0])
        return "{\"ok\":0,\"error\":\"missing to\"}";
    int r = nexos_mcp_fs_rename(fs, from, to);
    int j = my_snprintf(g_out, (int)sizeof(g_out), "{\"ok\":%d,\"fs\":%d,\"from\":\"",
                       r == 0 ? 1 : 0, fs);
    json_escape_into(g_out, &j, from, (int)sizeof(g_out));
    j += my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\",\"to\":\"");
    json_escape_into(g_out, &j, to, (int)sizeof(g_out));
    my_snprintf(g_out + j, (int)sizeof(g_out) - j, "\"}");
    return g_out;
}

/* ---- The dispatch table: tool name -> implementation ---- */
typedef const char* (*McpDispatchFn)(const char*, const char*, const char*, const char*);
struct McpImpl { const char* tool_name; McpDispatchFn fn; };

static const struct McpImpl g_mcp_impls[] = {
    /* demo / declarative */
    { "get_weather",      impl_weather          },
    { "summarize",        impl_generic          },
    /* built-in: Browser */
    { "browser_navigate", impl_browser_navigate },
    { "browser_get_text", impl_browser_get_text },
    /* built-in: Terminal */
    { "terminal_exec",    impl_terminal_exec    },
    /* built-in: File Manager */
    { "fs_list",          impl_fs_list          },
    { "fs_read",          impl_fs_read          },
    { "fs_write",         impl_fs_write         },
    { "fs_mkdir",         impl_fs_mkdir         },
    { "fs_remove",        impl_fs_remove        },
    { "fs_rename",        impl_fs_rename        },
};

static McpDispatchFn tool_impl_find(const char* tool_name){
    for(unsigned i = 0; i < sizeof(g_mcp_impls)/sizeof(g_mcp_impls[0]); i++)
        if(my_strcmp(g_mcp_impls[i].tool_name, tool_name) == 0)
            return g_mcp_impls[i].fn;
    return impl_generic;
}

/* =====================================================================
 *  Registry storage
 * ===================================================================== */
struct AppRegistry g_app_registry[NEXOS_AI_MAX_APPS];
int g_app_registry_count = 0;

struct AIGateway g_ai_gateway = {
    AI_BALANCED, "127.0.0.1", NEXOS_AI_GATEWAY_PORT, 1000, 0
};

struct AppRegistry* app_registry_find(const char* app_id){
    for(int i=0;i<g_app_registry_count;i++)
        if(my_strcmp(g_app_registry[i].app_id, app_id)==0) return &g_app_registry[i];
    return 0;
}

/* ---- Trusted resident set (the ACL principal) ----
 * Only the MiniCLR can add to this: it calls app_registry_note_resident()
 * with the basename of a .mex it actually loaded.  Manifest entries are
 * policy, not identity, so they never appear here on their own. */
static char g_resident_ids[NEXOS_AI_MAX_APPS][64];
static int  g_resident_count = 0;

extern "C" void app_registry_note_resident(const char* app_id){
    if(!app_id || !app_id[0]) return;
    for(int i=0;i<g_resident_count;i++)
        if(my_strcmp(g_resident_ids[i], app_id)==0) return;
    if(g_resident_count < NEXOS_AI_MAX_APPS){
        my_strncpy(g_resident_ids[g_resident_count], app_id, 64);
        g_resident_count++;
    }
    struct AppRegistry* e = app_registry_find(app_id);
    if(e) e->loaded = 1;
}

static int app_is_resident(const char* app_id){
    if(!app_id || !app_id[0]) return 0;
    for(int i=0;i<g_resident_count;i++)
        if(my_strcmp(g_resident_ids[i], app_id)==0) return 1;
    return 0;
}

/* ---- Manifest-driven tool storage ---- */
static struct McpTool  g_toolpool[NEXOS_AI_MAX_TOOLS * NEXOS_AI_MAX_APPS];
static int             g_toolpool_used = 0;
static const struct McpTool* g_new_tools[NEXOS_AI_MAX_APPS][NEXOS_AI_MAX_TOOLS];
static int             g_new_tool_count[NEXOS_AI_MAX_APPS];

/* Per-app ACL storage.  This MUST be one row per app: a single shared scratch
 * array would leave every AppRegistry pointing at whichever app was parsed
 * last, silently leaking one app's access list onto all the others. */
static const char*     g_access_pool[NEXOS_AI_MAX_APPS][NEXOS_AI_MAX_ACCESS];

/* =====================================================================
 *  Manifest parser.  /apps.json is the ONLY source of registry entries:
 *  g_app_registry is rebuilt from scratch on every parse, so there is no
 *  hidden compiled-in state to fall out of sync with the manifest.
 *
 *  Accepted keys (lowercase canonical; the spec's AIMode_Required /
 *  MCP_Server / MCP_Client_Access spellings are accepted as aliases):
 *    app_id, name, version, author,
 *    ai_mode_required | AIMode_Required      (Fast|Balanced|Heavy)
 *    mcp_server | MCP_Server : [ { tool_name, description,
 *                                  parameters_schema }, ... ]
 *    mcp_client_access | MCP_Client_Access : [ "app_id", ... ]
 * ===================================================================== */
static void parse_apps_json(const char* json){
    /* merge==0: reset the registry and rebuild from this manifest (the system
       /apps.json on the read-only SFS volume).
       merge==1: keep what is already registered and upsert by app_id (the
       MKFS overlay written by PackageInstaller).  This is what lets a
       third-party .nexpack register itself without the kernel ever needing a
       write primitive: the host writes mkfs/apps.json, the kernel merges it
       on boot.  See docs/NEXPACK.md §5.1. */
    int merge = (json && json[0] == 'M' && json[1] == 'E' &&
                 json[2] == 'R' && json[3] == 'G') ? 1 : 0;
    if (merge) json += 4;   /* skip the sentinel prefix */
    if (!merge) {
    g_app_registry_count = 0;
    g_toolpool_used       = 0;
    g_strpool_used        = 0;
    for (int i = 0; i < NEXOS_AI_MAX_APPS; i++) {
        g_new_tool_count[i] = 0;
        for (int j = 0; j < NEXOS_AI_MAX_ACCESS; j++) g_access_pool[i][j] = 0;
    }
    }

    const char* p = json;
    int cur = -1;
    struct McpTool* last_tool = 0;
    char tmp[128];

    while(*p){
        if(match(&p, "\"app_id\"")){
            skip_ws_colon(&p);
            if(read_string(&p, tmp, sizeof(tmp))){
                cur = -1;
                /* Merge mode: upsert by app_id so re-installing (upgrading) an
                   app does not create a duplicate registry entry. */
                if(merge){
                    struct AppRegistry* ex = app_registry_find(tmp);
                    if(ex){
                        cur = (int)(ex - g_app_registry);
                        /* Upgrading an app replaces its tool set: reset the
                           per-app tool slot so the new manifest's tools land
                           from index 0 instead of appending to the old ones
                           (which would duplicate every tool on reinstall). */
                        g_new_tool_count[cur] = 0;
                        last_tool = 0;
                        continue;
                    }
                }
                if(g_app_registry_count < NEXOS_AI_MAX_APPS){
                    cur = g_app_registry_count++;
                    struct AppRegistry* e = &g_app_registry[cur];
                    e->app_id                 = str_dup(tmp);
                    e->name                   = e->app_id;
                    e->version                = "1.0.0";
                    e->author                 = "";
                    e->ai_mode_required       = AI_BALANCED;
                    e->mcp_server             = 0;
                    e->mcp_server_count       = 0;
                    e->mcp_client_access      = 0;
                    e->mcp_client_access_count= 0;
                    e->loaded                 = 0;
                    g_new_tool_count[cur]     = 0;
                } else {
                    naslog("[AI] WARN: apps.json app count exceeds NEXOS_AI_MAX_APPS, truncating\n");
                }
                last_tool = 0;
            }
        } else if(match(&p, "\"name\"")){
            skip_ws_colon(&p);
            if(read_string(&p, tmp, sizeof(tmp)) && cur>=0)
                g_app_registry[cur].name = str_dup(tmp);
        } else if(match(&p, "\"version\"")){
            skip_ws_colon(&p);
            if(read_string(&p, tmp, sizeof(tmp)) && cur>=0)
                g_app_registry[cur].version = str_dup(tmp);
        } else if(match(&p, "\"author\"")){
            skip_ws_colon(&p);
            if(read_string(&p, tmp, sizeof(tmp)) && cur>=0)
                g_app_registry[cur].author = str_dup(tmp);
        } else if(match(&p, "\"ai_mode_required\"") || match(&p, "\"AIMode_Required\"")){
            skip_ws_colon(&p);
            if(read_string(&p, tmp, sizeof(tmp)) && cur>=0)
                g_app_registry[cur].ai_mode_required = map_mode(tmp);
        } else if(match(&p, "\"tool_name\"")){
            skip_ws_colon(&p);
            if(read_string(&p, tmp, sizeof(tmp)) && cur>=0){
                if(g_toolpool_used >= (int)(sizeof(g_toolpool)/sizeof(g_toolpool[0]))){
                    naslog("[AI] WARN: apps.json tool pool full, truncating tool\n");
                } else if(g_new_tool_count[cur] >= NEXOS_AI_MAX_TOOLS){
                    naslog("[AI] WARN: apps.json per-app tool count exceeds NEXOS_AI_MAX_TOOLS, truncating\n");
                } else {
                    struct McpTool* t = &g_toolpool[g_toolpool_used++];
                    t->tool_name         = str_dup(tmp);
                    t->description       = "";
                    t->parameters_schema = "";
                    t->dispatch          = tool_impl_find(t->tool_name);
                    last_tool            = t;
                    g_new_tools[cur][g_new_tool_count[cur]++] = t;
                }
            }
        } else if(match(&p, "\"description\"")){
            skip_ws_colon(&p);
            if(read_string(&p, tmp, sizeof(tmp)) && last_tool)
                last_tool->description = str_dup(tmp);
        } else if(match(&p, "\"parameters_schema\"")){
            skip_ws_colon(&p);
            if(*p=='{' || *p=='['){
                char raw[400];
                capture_raw(&p, raw, sizeof(raw));
                if(last_tool) last_tool->parameters_schema = str_dup(raw);
            }
        } else if(match(&p, "\"mcp_client_access\"") || match(&p, "\"MCP_Client_Access\"")){
            skip_ws_colon(&p);
            if(*p=='['){
                p++;
                int an = 0;
                /* Whitespace must be skipped BEFORE testing for the closing
                   bracket: a pretty-printed manifest puts a newline between
                   the last element and ']', and testing first would let the
                   loop walk past the bracket and swallow the rest of the
                   document. */
                for(;;){
                    skip_ws(&p);
                    if(*p != '"') break;
                    if(!read_string(&p, tmp, sizeof(tmp))) break;
                    if(an >= NEXOS_AI_MAX_ACCESS){
                        naslog("[AI] WARN: apps.json mcp_client_access exceeds NEXOS_AI_MAX_ACCESS, truncating\n");
                    } else if(cur >= 0){
                        g_access_pool[cur][an++] = str_dup(tmp);
                    }
                    skip_ws(&p);
                    if(*p == ','){ p++; continue; }
                    break;
                }
                skip_ws(&p);
                if(*p==']') p++;
                if(cur>=0){
                    g_app_registry[cur].mcp_client_access       = an ? g_access_pool[cur] : 0;
                    g_app_registry[cur].mcp_client_access_count = an;
                }
            }
        } else {
            p++;
        }
    }

    /* Attach the freshly-built tool arrays to their apps. */
    for(int i=0;i<g_app_registry_count;i++){
        g_app_registry[i].mcp_server       = g_new_tool_count[i] ? g_new_tools[i] : 0;
        g_app_registry[i].mcp_server_count = g_new_tool_count[i];
    }
    /* Restore the resident flag for apps that are already running: a manifest
       reload must never revoke identity the CLR has already endorsed. */
    for(int i=0;i<g_resident_count;i++){
        struct AppRegistry* e = app_registry_find(g_resident_ids[i]);
        if(e) e->loaded = 1;
    }
}

int ai_gateway_init(void){
    static unsigned char buf[8192];
    int n = kern_fs_read("apps.json", buf, (int)sizeof(buf)-1);
    if(n <= 0) n = kern_fs_read("/apps.json", buf, (int)sizeof(buf)-1);
    if(n > 0){
        buf[n] = 0;
        /* Gateway tunables live in the same manifest. */
        {   int p = json_get_int((const char*)buf, "port", NEXOS_AI_GATEWAY_PORT);
            g_ai_gateway.gateway_port = (p > 0 && p <= 65535) ? p : NEXOS_AI_GATEWAY_PORT;
        }
        {   int q = json_get_int((const char*)buf, "quota_pool", 1000);
            g_ai_gateway.quota_remaining = (q > 0) ? q : 0;
        }
        parse_apps_json((const char*)buf);
    } else {
        /* No manifest -> empty registry.  Callers get MCP_ERR_NO_SRC/NO_TGT,
           which is the honest answer; the kernel no longer fabricates apps. */
        g_app_registry_count = 0;
    }

    /* ---- MKFS overlay: third-party apps installed by PackageInstaller ----
       The system apps.json lives on the read-only SFS volume, so a runtime
       install cannot touch it.  PackageInstaller therefore writes the merged
       registry to MKFS as "apps.overlay.json" and we merge it here, keyed by
       app_id: existing entries are updated in place, new ones are appended.
       The "MERG" prefix tells parse_apps_json to upsert instead of reset.

       NOTE on the file name: kern_fs_read() takes a FLAT name and already
       probes MKFS before SFS (kernel.cpp:6384).  So a plain "apps.json" would
       resolve to the MKFS copy and hide the system manifest when both exist.
       Using a distinct overlay name keeps the two sources independently
       addressable: system = "apps.json" (SFS), installs = "apps.overlay.json"
       (MKFS). */
    {
        static unsigned char ovl[8192];
        int m = kern_fs_read("apps.overlay.json", ovl, (int)sizeof(ovl) - 5);
        if(m <= 0) m = kern_fs_read("/apps.overlay.json", ovl, (int)sizeof(ovl) - 5);
        if(m > 0 && m < (int)sizeof(ovl) - 5){
            /* Shift the payload right by 4 to make room for the sentinel. */
            for(int i = m; i >= 0; i--) ovl[i + 4] = ovl[i];
            ovl[0] = 'M'; ovl[1] = 'E'; ovl[2] = 'R'; ovl[3] = 'G';
            parse_apps_json((const char*)ovl);
        }
    }

    g_ai_gateway.initialized = 1;
    return g_app_registry_count;
}

int get_mode(void){ return g_ai_gateway.system_mode; }

int set_mode(int m){
    if(m<AI_FAST) m=AI_FAST; if(m>AI_HEAVY) m=AI_HEAVY;
    g_ai_gateway.system_mode = m;
    ai_set_mode(m);
    return 0;
}

int quota_remaining(void){ return g_ai_gateway.quota_remaining; }

int ai_request(const char* prompt, int mode, char* out, int outsize){
    if(!g_ai_gateway.initialized) ai_gateway_init();
    static char reason[1024];
    static char httpbuf[2048];
    int code = 0; const char* text; int used_mode = mode;
    if(mode <= AI_FAST){
        ai_reason(prompt ? prompt : "", reason, (int)sizeof(reason)-1);
        text = reason;
    } else {
        static char url[128];
        my_snprintf(url, (int)sizeof(url), "http://127.0.0.1:%d/ai?mode=%d",
                g_ai_gateway.gateway_port, mode);
        int r = net_http_get(url, httpbuf, (int)sizeof(httpbuf)-1);
        if(r <= 0){ code = MCP_ERR_DISPATCH; text = ""; }
        else { httpbuf[sizeof(httpbuf)-1]=0; text = httpbuf; }
        if(g_ai_gateway.quota_remaining > 0) g_ai_gateway.quota_remaining--;
    }
    if(out && outsize > 0){
        int j = my_snprintf(out, outsize, "{\"code\":%d,\"text\":\"", code);
        json_escape_into(out, &j, text, outsize);
        my_snprintf(out + j, outsize - j, "\",\"mode\":%d,\"quotaLeft\":%d}",
                   used_mode, g_ai_gateway.quota_remaining);
    }
    return 0;
}

/* Wrap a tool result in the bus reply envelope.  Escapes inner_json directly
 * into g_env after the prefix, so no third buffer is needed. */
static const char* build_mcp_result(int code, const char* inner_json){
    int j = my_snprintf(g_env, (int)sizeof(g_env), "{\"code\":%d,\"json\":\"", code);
    json_escape_into(g_env, &j, inner_json ? inner_json : "null", (int)sizeof(g_env));
    my_snprintf(g_env + j, (int)sizeof(g_env) - j, "\"}");
    return g_env;
}

const char* mcp_dispatch_request(const char* source_app_id,
                                  const char* target_app_id,
                                  const char* tool_name,
                                  const char* json_args){
    if(!g_ai_gateway.initialized) ai_gateway_init();

    struct AppRegistry* src = app_registry_find(source_app_id);
    if(!src) return build_mcp_result(MCP_ERR_NO_SRC, "null");

    /* ENDORSEMENT: the caller must be a .mex the CLR actually loaded.  The
       manifest alone cannot grant an identity, so a managed app that lies
       about who it is still fails here. */
    if(!app_is_resident(source_app_id))
        return build_mcp_result(MCP_ERR_FORBID,
                                "{\"error\":\"caller is not a resident .mex\"}");

    struct AppRegistry* tgt = app_registry_find(target_app_id);
    if(!tgt) return build_mcp_result(MCP_ERR_NO_TGT, "null");

    /* ACL (outbound allowlist): the CALLER's mcp_client_access must list the
       target app it wants to call.  This is the only authz the bus performs. */
    int allowed = 0;
    for(int i=0;i<src->mcp_client_access_count;i++){
        if(my_strcmp(src->mcp_client_access[i], target_app_id)==0){ allowed=1; break; }
    }
    if(!allowed) return build_mcp_result(MCP_ERR_FORBID, "null");

    const struct McpTool* tool = 0;
    for(int i=0;i<tgt->mcp_server_count;i++){
        if(my_strcmp(tgt->mcp_server[i]->tool_name, tool_name)==0){ tool=tgt->mcp_server[i]; break; }
    }
    if(!tool) return build_mcp_result(MCP_ERR_NO_TGT, "null");

    const char* res = tool->dispatch
        ? tool->dispatch(source_app_id, target_app_id, tool_name, json_args)
        : "null";
    if(!res) return build_mcp_result(MCP_ERR_DISPATCH, "null");
    return build_mcp_result(MCP_OK, res);
}