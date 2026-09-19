/* theme_default plugin - Phase 2 facade + genuinely decoupled colour table.
 * Owns the window colour palette (mirrors gui.cpp's WIN* constants).  gui.cpp
 * is unchanged by default; when it consults svc_lookup("theme.colors") at draw
 * time it picks up this table, enabling live reload in Phase 5.  The palette is
 * held in a MUTABLE runtime array (g_theme_rt) so `plugin pm reload theme_default`
 * can swap colours without recompiling the kernel.
 */
#include "plugin_manager.h"
#include "nexos_api.h"
#include "skill_vm.h"

/* Default palette (mirrors gui.cpp's WIN* constants). */
static const uint32_t g_theme_default_colors[16] = {
    0x1F1F1F, 0x2B2B2B, 0x323232, 0x0078D4, 0x0E639C, 0x1E1E1E, 0xF3F3F3, 0xFFFFFF,
    0xCCCCCC, 0x888888, 0x2D2D2D, 0x3A3A3A, 0x005A9E, 0x2D7D46, 0xC50F1F, 0xE8A33D
};
/* Mutable runtime palette (what gui.cpp actually reads). */
static uint32_t g_theme_rt[16];

static int theme_colors(void* args, void* out, int outcap) {
    (void)args;
    if (out && outcap >= (int)sizeof(const uint32_t*)) {
        *(const uint32_t**)out = g_theme_rt;
        return 16; /* count */
    }
    return 0;
}

/* Phase 5 reload: the .bc exposes the 16 palette entries as its first 16
 * globals.  Load it, run main() (which materialises the global initialisers),
 * then copy globals[0..15] into g_theme_rt so gui.cpp picks up the new colours
 * on its next frame. */
static int theme_reload(const uint8_t* bc, int len) {
    SkillVM vm;
    if (skill_load(&vm, bc, len) != 0) {
        pm_serial("[THEME] reload: bad .bc\n"); return -1;
    }
    int fid = skill_find(&vm, "main");
    int out = 0;
    if (skill_run(&vm, fid >= 0 ? fid : 0, &out) != 0) {
        pm_serial("[THEME] reload: run failed\n"); return -1;
    }
    for (int i = 0; i < 16; i++) g_theme_rt[i] = (uint32_t)vm.globals[i];
    pm_serial("[THEME] reload: palette updated from .bc\n");
    return 0;
}

static int theme_default_init(Plugin* self) {
    (void)self;
    for (int i = 0; i < 16; i++) g_theme_rt[i] = g_theme_default_colors[i];
    svc_register("theme.colors", theme_colors);
    pm_set_reload("theme_default", theme_reload);
    return 0;
}
static void theme_default_exit(Plugin* self) {
    (void)self;
    svc_unregister("theme.colors");
}
static int theme_default_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self;
    if (pm_strcmp(method, "colors") != 0) return -1;
    int n = theme_colors(args, out, outcap);
    pm_serial("[THEME] colors ok (");
    char cb[8]; int ci=0, t=n; if(t==0) cb[ci++]='0'; else {char tmp[4];int tn=0;while(t){tmp[tn++]=('0'+(t%10));t/=10;}while(tn)cb[ci++]=tmp[--tn];} cb[ci++]=0;
    pm_serial(cb);
    pm_serial(" entries)\n");
    return n;
}
extern const Plugin g_theme_default = {
    "theme_default", 0x0100,
    "theme.colors",
    "",
    theme_default_init, theme_default_exit, theme_default_call
};
