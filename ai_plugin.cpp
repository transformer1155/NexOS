// =====================================================================
//  ai_plugin.cpp  -  NexOS plugin catalogue / manager
// =====================================================================
#include "ai_plugin.h"

// Freestanding string helpers (no <string.h> in this build).
static int ap_strlen(const char* s){ int n=0; while(s[n]) n++; return n; }
static int ap_strcmp(const char* a,const char* b){ while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }

// State -> short label, indexed by AiPlugin.state.
static const char* g_state_name[] = { "planned", "basic", "available" };

// The 23 plugins from the NexOS manifest.  `state` tracks how far each
// is implemented; `loaded` is the runtime flag managed by load/unload.
struct AiPlugin g_plugins[] = {
    { "nexos.filemanager",   "File Explorer",        "MKFS/SFS",         8192, 0, 0 },
    { "nexos.terminal",      "Terminal",             "Shell",            4096, 1, 0 },
    { "nexos.taskmanager",   "Task Manager",         "Proc",             6144, 0, 0 },
    { "nexos.settings",      "Control Panel",        "Config",           5120, 0, 0 },
    { "nexos.appinstaller",  "App Installer",        "FS/Net",          10240, 0, 0 },
    { "nexos.ai.inference",  "AI Inference Engine",  "GGUF loader",     20480, 1, 0 },
    { "nexos.knowledge",     "Knowledge Base / RAG", "AI engine",       15360, 0, 0 },
    { "nexos.skills",        "AI Skills",            "AI / KBase",       8192, 0, 0 },
    { "nexos.distributed",   "Distributed Compute",  "Net",             10240, 0, 0 },
    { "nexos.physical",      "Physical Machine Drv", "GPIO/PWM",         4096, 0, 0 },
    { "nexos.production",    "Production Line",      "Physical/AI",     12288, 0, 0 },
    { "nexos.editor",        "Code Editor",          "FS",              10240, 0, 0 },
    { "nexos.git",           "Git Integration",      "FS/Net",           6144, 0, 0 },
    { "nexos.logviewer",     "Log Viewer",           "Log",              5120, 0, 0 },
    { "nexos.firewall",      "Firewall",             "Net",              3072, 0, 0 },
    { "nexos.messaging",     "Enterprise Messaging", "Net/User",         8192, 0, 0 },
    { "nexos.roles",         "Role System",          "User/Perm",        4096, 0, 0 },
    { "nexos.audit",         "Audit Log",            "Log",              5120, 0, 0 },
    { "nexos.monitor",       "Performance Monitor",  "Stats",            4096, 0, 0 },
    { "nexos.aidesk",        "AI Desktop Assistant", "AI/Skills",        5120, 1, 0 },
    { "nexos.ai",            "AI Plugin",            "AI/KBase/Skills", 15360, 1, 0 },
    { "nexos.ai_interface",  "AI Interface",         "AI Plugin",        3072, 0, 0 },
    { "nexos.virtualdesktop","Virtual Desktop",      "WM",               3072, 1, 0 },
};

int g_plugin_count = (int)(sizeof(g_plugins) / sizeof(g_plugins[0]));

void ai_plugin_init(void) {
    for (int i = 0; i < g_plugin_count; i++)
        g_plugins[i].loaded = (g_plugins[i].state >= 1) ? 1 : 0;
}

int ai_plugin_find(const char* id) {
    for (int i = 0; i < g_plugin_count; i++)
        if (ap_strcmp(g_plugins[i].id, id) == 0) return i;
    return -1;
}

int ai_plugin_set(const char* id, int loaded) {
    int i = ai_plugin_find(id);
    if (i < 0) return -1;
    g_plugins[i].loaded = loaded ? 1 : 0;
    return g_plugins[i].loaded;
}

int ai_plugin_toggle(const char* id) {
    int i = ai_plugin_find(id);
    if (i < 0) return -1;
    g_plugins[i].loaded = g_plugins[i].loaded ? 0 : 1;
    return g_plugins[i].loaded;
}

// ---- tiny append helpers (no sprintf in this environment) ----
static void ap_cat(char* b, int* p, int max, const char* s) {
    while (*s && *p < max - 1) b[(*p)++] = *s++;
}
static void ap_int(char* b, int* p, int max, int v) {
    if (v == 0) { if (*p < max - 1) b[(*p)++] = '0'; return; }
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    char tmp[12]; int n = 0;
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg && *p < max - 1) b[(*p)++] = '-';
    while (n > 0 && *p < max - 1) b[(*p)++] = tmp[--n];
}
static void ap_pad(char* b, int* p, int max, int have, int want) {
    for (int k = have; k < want && *p < max - 1; k++) b[(*p)++] = ' ';
}

int ai_plugin_list(char* buf, int bufsize) {
    int p = 0;
    ap_cat(buf, &p, bufsize, "NexOS plugin catalogue (");
    ap_int(buf, &p, bufsize, g_plugin_count);
    ap_cat(buf, &p, bufsize, " plugins):\n");
    ap_cat(buf, &p, bufsize, "  ID                     NAME                 STATE      MB\n");
    for (int i = 0; i < g_plugin_count; i++) {
        AiPlugin* pl = &g_plugins[i];
        ap_cat(buf, &p, bufsize, "  ");
        ap_cat(buf, &p, bufsize, pl->id);
        ap_pad(buf, &p, bufsize, (int)ap_strlen(pl->id), 22);
        ap_cat(buf, &p, bufsize, pl->name);
        ap_pad(buf, &p, bufsize, (int)ap_strlen(pl->name), 20);
        const char* st = (pl->state >= 0 && pl->state <= 2) ? g_state_name[pl->state] : "?";
        ap_cat(buf, &p, bufsize, st);
        ap_pad(buf, &p, bufsize, (int)ap_strlen(st), 10);
        ap_int(buf, &p, bufsize, pl->mem_kb / 1024);
        ap_cat(buf, &p, bufsize, pl->loaded ? "   [loaded]\n" : "\n");
    }
    buf[p < bufsize ? p : bufsize - 1] = 0;
    return p;
}

int ai_plugin_serialize(char* buf, int bufsize) {
    int p = 0;
    for (int i = 0; i < g_plugin_count; i++) {
        AiPlugin* pl = &g_plugins[i];
        ap_cat(buf, &p, bufsize, pl->id);
        ap_cat(buf, &p, bufsize, "|");
        ap_cat(buf, &p, bufsize, pl->name);
        ap_cat(buf, &p, bufsize, "|");
        ap_int(buf, &p, bufsize, pl->state);
        ap_cat(buf, &p, bufsize, "|");
        ap_int(buf, &p, bufsize, pl->loaded);
        ap_cat(buf, &p, bufsize, "\n");
    }
    buf[p < bufsize ? p : bufsize - 1] = 0;
    return p;
}
