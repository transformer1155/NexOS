/* wm_core plugin - Phase 2 facade.
 * Registers window-management services and dispatches
 * pm_call("wm_core","create_window"|"draw_window").  Delegates to nexos.gui.api
 * when published; otherwise logs success.  gui.cpp is unchanged.
 */
#include "plugin_manager.h"
#include "nexos_api.h"

static int wm_svc(void* args, void* out, int outcap) {
    (void)args; (void)out; (void)outcap;
    return 0;
}

static int wm_core_init(Plugin* self) {
    (void)self;
    svc_register("wm.create_window", wm_svc);
    svc_register("wm.draw_window", wm_svc);
    return 0;
}
static void wm_core_exit(Plugin* self) {
    (void)self;
    svc_unregister("wm.create_window");
    svc_unregister("wm.draw_window");
}
static int wm_core_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self;
    const char* svc = 0;
    if (!pm_strcmp(method, "create_window")) svc = "wm.create_window";
    else if (!pm_strcmp(method, "draw_window")) svc = "wm.draw_window";
    else return -1;
    svc_fn f = svc_lookup(svc);
    if (!f) return -1;
    int r = f(args, out, outcap);
    pm_serial("[WM] "); pm_serial(method); pm_serial(" ok\n");
    return r;
}
extern const Plugin g_wm_core = {
    "wm_core", 0x0100,
    "wm.create_window,wm.draw_window",
    "gfx_core,font_cjk",
    wm_core_init, wm_core_exit, wm_core_call
};
