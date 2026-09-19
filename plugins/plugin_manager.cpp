/* plugin_manager.cpp - NexOS plugin manager implementation.
 * Freestanding: only <stdint.h>, kmalloc/kfree (extern "C"), and a private
 * COM1 serial helper. No C++ stdlib, no exceptions, no RTTI, no virtual.
 */
#include "plugin_manager.h"
#include "skill_vm.h"   /* NBC1 bytecode VM (header-only, freestanding) */

extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);
extern "C" int   kern_fs_read(const char* name, unsigned char* buf, int bufsize);

/* ---- private serial (each freestanding TU owns its own; kernel's is static)*/
static inline void pm_outb(uint16_t p, uint8_t v) {
    __asm__ __volatile__("outb %%al, %%dx" :: "a"(v), "d"(p));
}
void pm_serial(const char* s) {
    while (*s) pm_outb(0x3F8, (uint8_t)*s++);
}
void pm_serial_byte(uint8_t b) { pm_outb(0x3F8, b); }

int pm_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* ---- service registry: FNV-1a hash + open addressing ---------------------- */
typedef struct {
    const char* name;   /* NULL == empty slot */
    svc_fn      fn;
} ServiceEntry;

static ServiceEntry g_svc[PM_SVC_SLOTS];

static uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static int svc_probe(const char* name, uint32_t* out_idx) {
    uint32_t h = fnv1a(name);
    uint32_t i = h & (PM_SVC_SLOTS - 1);
    uint32_t start = i;
    do {
        if (g_svc[i].name == nullptr) { *out_idx = i; return 0; }       /* free */
        if (g_svc[i].name && pm_strcmp(g_svc[i].name, name) == 0) {
            *out_idx = i; return 1;                                    /* found */
        }
        i = (i + 1) & (PM_SVC_SLOTS - 1);
    } while (i != start);
    return -1; /* table full */
}

int svc_register(const char* name, svc_fn fn) {
    uint32_t idx;
    int r = svc_probe(name, &idx);
    if (r == -1) { pm_serial("[PM] svc table full\n"); return -1; }
    g_svc[idx].name = name;
    g_svc[idx].fn   = fn;
    return 0;
}
svc_fn svc_lookup(const char* name) {
    uint32_t idx;
    if (svc_probe(name, &idx) == 1) return g_svc[idx].fn;
    return nullptr;
}
int svc_unregister(const char* name) {
    uint32_t idx;
    if (svc_probe(name, &idx) == 1) { g_svc[idx].name = nullptr; g_svc[idx].fn = nullptr; return 0; }
    return -1;
}
void svc_clear(void) {
    for (int i = 0; i < PM_SVC_SLOTS; i++) { g_svc[i].name = nullptr; g_svc[i].fn = nullptr; }
}

/* ---- plugin table + ordering state ---------------------------------------- */
static const Plugin* g_plugins[PM_MAX_PLUGINS];
static int           g_nplugins;

static int  g_color[PM_MAX_PLUGINS];   /* 0 white, 1 gray, 2 black */
static int  g_order[PM_MAX_PLUGINS];   /* init order (deps first)   */
static int  g_order_n;

/* loaded stack for rollback */
static int  g_loaded[PM_MAX_PLUGINS];
static int  g_loaded_n;

/* path stack for cycle printing */
static int  g_path[PM_MAX_PLUGINS];
static int  g_path_n;

static int find_index(const char* name) {
    for (int i = 0; i < g_nplugins; i++)
        if (g_plugins[i] && pm_strcmp(g_plugins[i]->name, name) == 0)
            return i;
    return -1;
}

/* iterate comma-separated requires/provides list */
static const char* tok_next(const char** pp) {
    const char* p = *pp;
    while (*p == ' ' || *p == ',') p++;
    if (*p == 0) { *pp = p; return nullptr; }
    const char* s = p;
    while (*p && *p != ',') p++;
    /* temporarily not NUL-terminated; caller compares with bounded length */
    *pp = p;
    return s;
}
/* compare a token [s, s+len) with a NUL string */
static int tok_eq(const char* s, int len, const char* str) {
    for (int i = 0; i < len; i++) {
        if (str[i] == 0 || str[i] != s[i]) return 0;
    }
    return str[len] == 0;
}

int pm_init(void) {
    g_nplugins = 0;
    g_order_n  = 0;
    g_loaded_n = 0;
    for (int i = 0; i < PM_MAX_PLUGINS; i++) { g_plugins[i] = nullptr; g_color[i] = 0; }
    svc_clear();
    return 0;
}

int pm_register(const Plugin* p) {
    if (!p || !p->name) return -1;
    if (find_index(p->name) >= 0) { pm_serial("[PM] duplicate plugin: "); pm_serial(p->name); pm_serial("\n"); return -1; }
    if (g_nplugins >= PM_MAX_PLUGINS) { pm_serial("[PM] plugin table full\n"); return -1; }
    g_plugins[g_nplugins++] = p;
    return 0;
}

const Plugin* pm_find(const char* name) {
    int i = find_index(name);
    return i >= 0 ? g_plugins[i] : nullptr;
}

/* Introspection for the shell's `plugin pm list`: enumerate what is actually
 * registered instead of printing a number that goes stale every time a plugin
 * is added.  Both return 0/nullptr for an out-of-range index. */
int pm_count(void) {
    return g_nplugins;
}

const char* pm_name_at(int idx) {
    if (idx < 0 || idx >= g_nplugins) return nullptr;
    return g_plugins[idx] ? g_plugins[idx]->name : nullptr;
}

/* DFS topological sort: dependencies first. Detects cycles (gray back-edge). */
static int dfs(int u) {
    g_color[u] = 1;                 /* gray: in progress */
    g_path[g_path_n++] = u;
    const char* req = g_plugins[u]->deps;
    const char* p = req ? req : "";
    const char* tok;
    while ((tok = tok_next(&p)) != nullptr) {
        /* find length of token (until ',' or end) */
        int len = 0; const char* e = tok;
        while (e[0] && e[0] != ',') { len++; e++; }
        if (len == 0) continue;
        int v = -1;
        for (int i = 0; i < g_nplugins; i++)
            if (g_plugins[i] && tok_eq(tok, len, g_plugins[i]->name)) { v = i; break; }
        if (v < 0) {
            pm_serial("[PM] missing dependency '");
            /* bounded print */
            for (int k = 0; k < len; k++) pm_outb(0x3F8, (uint8_t)tok[k]);
            pm_serial("' for plugin "); pm_serial(g_plugins[u]->name); pm_serial("\n");
            g_path_n--; return -1;
        }
        if (g_color[v] == 1) {
            /* cycle: print path from the repeat */
            pm_serial("[PM] circular dependency: ");
            int start = 0;
            while (start < g_path_n && g_path[start] != v) start++;
            for (int k = start; k < g_path_n; k++) {
                pm_serial(g_plugins[g_path[k]]->name);
                pm_serial(k + 1 < g_path_n ? " -> " : "");
            }
            pm_serial(" -> "); pm_serial(g_plugins[v]->name); pm_serial("\n");
            g_path_n--; return -1;
        }
        if (g_color[v] == 0) { if (dfs(v) != 0) { g_path_n--; return -1; } }
    }
    g_color[u] = 2;                 /* black: done */
    g_order[g_order_n++] = u;
    g_path_n--;
    return 0;
}

int pm_resolve(void) {
    g_order_n = 0;
    for (int i = 0; i < g_nplugins; i++) g_color[i] = 0;
    for (int i = 0; i < g_nplugins; i++) {
        if (g_color[i] == 0) {
            g_path_n = 0;
            if (dfs(i) != 0) return -1;
        }
    }
    return 0;
}

int pm_load_all(void) {
    g_loaded_n = 0;
    for (int i = 0; i < g_order_n; i++) {
        const Plugin* p = g_plugins[g_order[i]];
        int r = p->init((Plugin*)p);
        if (r != 0) {
            pm_serial("[PM] init failed for "); pm_serial(p->name); pm_serial("\n");
            /* rollback: reverse-call exit() for already-loaded, then clear svc */
            for (int k = g_loaded_n - 1; k >= 0; k--)
                g_plugins[g_loaded[k]]->exit((Plugin*)g_plugins[g_loaded[k]]);
            g_loaded_n = 0;
            svc_clear();
            return -1;
        }
        g_loaded[g_loaded_n++] = g_order[i];
        /* Phase 1 log: [PLUGIN] <name> v<version> loaded */
        pm_serial("[PLUGIN] ");
        pm_serial(p->name);
        pm_serial(" v");
        /* print version as major.minor */
        {
            char vb[8]; int vi = 0;
            int maj = (p->version >> 8) & 0xFF, min = p->version & 0xFF;
            if (maj == 0) vb[vi++] = '0'; else { int t = maj; char tmp[4]; int tn=0; while(t){tmp[tn++]=('0'+(t%10));t/=10;} while(tn) vb[vi++]=tmp[--tn]; }
            vb[vi++] = '.';
            if (min == 0) vb[vi++] = '0'; else { int t = min; char tmp[4]; int tn=0; while(t){tmp[tn++]=('0'+(t%10));t/=10;} while(tn) vb[vi++]=tmp[--tn]; }
            vb[vi++] = 0;
            pm_serial(vb);
        }
        pm_serial(" loaded");
        /* list the services this plugin provides */
        if (p->provides && p->provides[0]) {
            pm_serial("  services: ");
            pm_serial(p->provides);
        }
        pm_serial("\n");
    }
    return 0;
}

void pm_unload_all(void) {
    for (int k = g_loaded_n - 1; k >= 0; k--)
        g_plugins[g_loaded[k]]->exit((Plugin*)g_plugins[g_loaded[k]]);
    g_loaded_n = 0;
    svc_clear();
}

int pm_call(const char* plugin, const char* method, void* args, void* out, int outcap) {
    const Plugin* p = pm_find(plugin);
    if (!p) { pm_serial("[PM] pm_call: no such plugin: "); pm_serial(plugin); pm_serial("\n"); return -1; }
    return p->call((Plugin*)p, method, args, out, outcap);
}

/* ---- boot hook ------------------------------------------------------------ */
extern const Plugin g_helloworld;

void plugin_manager_boot(void) {
    pm_serial("[PM] plugin manager starting\n");
    pm_init();
    if (pm_register(&g_helloworld) != 0) { pm_serial("[PM] register failed\n"); return; }
    extern void plugins_boot(void);
    plugins_boot();
    if (pm_resolve() != 0) { pm_serial("[PM] dependency resolve failed\n"); return; }
    if (pm_load_all() != 0) { pm_serial("[PM] load failed\n"); return; }
    pm_serial("[PM] all plugins loaded\n");
    /* Phase 3 smoke test: view a .skill source baked into MKFS */
    svc_fn viewer = svc_lookup("src.view");
    if (viewer) viewer((void*)"src_gfx_core.skill", 0, 0);
    else pm_serial("[PM] src.view service missing\n");
}

/* ---- runtime reload hooks + hot reload (Phase 5) -------------------------- */
static plugin_reload_fn g_reload_hooks[PM_MAX_PLUGINS];

int pm_set_reload(const char* name, plugin_reload_fn fn) {
    int i = find_index(name);
    if (i < 0) { pm_serial("[PM] pm_set_reload: unknown plugin "); pm_serial(name); pm_serial("\n"); return -1; }
    g_reload_hooks[i] = fn;
    return 0;
}

int pm_reload(const char* name) {
    int i = find_index(name);
    if (i < 0) { pm_serial("[PM] reload: no such plugin: "); pm_serial(name); pm_serial("\n"); return -1; }
    if (!g_reload_hooks[i]) { pm_serial("[PM] reload: plugin has no reload hook: "); pm_serial(name); pm_serial("\n"); return -1; }

    /* Prefer a precompiled .bc (NBC1); fall back to the .skill source text.
     * SFS (and MKFS) store these flat as "src_<name>.bc" / "src_<name>.skill"
     * (sfs_gen.py has no subdirectory support, 19-char name cap). */
    /* Candidate base names, in priority order.  SFS/MKFS names are capped at 19
     * chars (name[20]), so we try short aliases for the long plugin names.
     * e.g. app_calculator -> "calc", theme_default -> "theme_default" (fits). */
    const char* aliases[4];
    int nalias = 0;
    aliases[nalias++] = name;                       /* <name> */
    if (!pm_strcmp(name, "app_calculator")) aliases[nalias++] = "calc";
    if (!pm_strcmp(name, "theme_default"))   aliases[nalias++] = "theme_default";
    aliases[nalias++] = "src";                        /* src_<name> prefix below */

    static uint8_t buf[65536];
    int len = -1;
    for (int c = 0; c < nalias && len < 0; c++) {
        for (int ext = 0; ext < 2 && len < 0; ext++) {  /* 0=.bc, 1=.skill */
            char path[64];
            int k = 0;
            const char* base = aliases[c];
            if (base[0] == 's' && base[1] == 'r' && base[2] == 'c') {
                const char* q = "src_"; while (*q) path[k++] = *q++;
                const char* nn = name; while (*nn && k < 56) path[k++] = *nn++;
            } else {
                const char* nn = base; while (*nn && k < 56) path[k++] = *nn++;
            }
            path[k++] = '.';
            path[k++] = (ext == 0) ? 'b' : 's';
            path[k++] = (ext == 0) ? 'c' : 'k';
            if (ext == 1) { path[k++] = 'i'; path[k++] = 'l'; }
            path[k++] = 0;
            len = kern_fs_read(path, buf, (int)sizeof(buf));
        }
    }
    if (len < 0) {
        pm_serial("[PM] reload: source not found: "); pm_serial(name); pm_serial("\n");
        return -1;
    }
    pm_serial("[PM] reload: loaded "); pm_serial(name); pm_serial(" (");
    { char nb[8]; int ni=0,t=len; if(t==0)nb[ni++]='0'; else{char tmp[6];int tn=0;while(t){tmp[tn++]=('0'+(t%10));t/=10;}while(tn)nb[ni++]=tmp[--tn];} nb[ni++]=0; pm_serial(nb); }
    pm_serial(" bytes)\n");
    if (0) { pm_serial("[PM] reload: source not found for "); pm_serial(name); pm_serial("\n"); return -1; }

    if (0) pm_serial("[PM] reload: loading "); pm_serial(name);
    pm_serial(" ("); { char nb[8]; int ni=0,t=len; if(t==0)nb[ni++]='0'; else{char tmp[6];int tn=0;while(t){tmp[tn++]=(char)('0'+(t%10));t/=10;}while(tn)nb[ni++]=tmp[--tn];} nb[ni++]=0; pm_serial(nb); } pm_serial(" bytes)\n");

    int r = g_reload_hooks[i](buf, len);
    if (r == 0) { pm_serial("[PM] reload: "); pm_serial(name); pm_serial(" rebound ok\n"); }
    else { pm_serial("[PM] reload: "); pm_serial(name); pm_serial(" rebind failed\n"); }
    return r;
}
