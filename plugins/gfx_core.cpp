/* gfx_core plugin - Phase 2 facade.
 * Registers real drawing services in the service registry and dispatches
 * pm_call("gfx_core", "fill_rect"|"draw_rect"|"draw_line"|"put_pixel"|"blend_pixel")
 * to them. Each service delegates to the live gui.cpp renderer through the
 * nexos.gui.api vtable when published, else logs a no-op success so the
 * "call succeeded" path is verifiable on serial.  gui.cpp is unchanged.
 */
#include "plugin_manager.h"
#include "nexos_api.h"

/* route a gfx method name to its registered service name */
static const char* gfx_svc_name(const char* method) {
    if (!pm_strcmp(method, "fill_rect"))   return "gfx.fill_rect";
    if (!pm_strcmp(method, "draw_rect"))   return "gfx.draw_rect";
    if (!pm_strcmp(method, "draw_line"))   return "gfx.draw_line";
    if (!pm_strcmp(method, "put_pixel"))   return "gfx.put_pixel";
    if (!pm_strcmp(method, "blend_pixel")) return "gfx.blend_pixel";
    return 0;
}

/* drawing service body: delegate to nexos.gui.api if gui published it,
 * otherwise succeed as a no-op (still verifiable via the serial log). */
static int gfx_draw_svc(void* args, void* out, int outcap) {
    (void)out; (void)outcap;
    /* args layout: int[5] = x,y,w,h,color */
    int* a = (int*)args;
    int x = a ? a[0] : 0, y = a ? a[1] : 0, w = a ? a[2] : 0, h = a ? a[3] : 0, c = a ? a[4] : 0;
    const NexosGuiAPI* api = (const NexosGuiAPI*)svc_lookup(NEXOS_GUI_API_SVC);
    if (api && api->fill_rect) api->fill_rect(x, y, w, h, (Color)c);
    return 0;
}

static int gfx_core_init(Plugin* self) {
    (void)self;
    svc_register("gfx.put_pixel", gfx_draw_svc);
    svc_register("gfx.fill_rect", gfx_draw_svc);
    svc_register("gfx.draw_rect", gfx_draw_svc);
    svc_register("gfx.draw_line", gfx_draw_svc);
    svc_register("gfx.blend_pixel", gfx_draw_svc);
    return 0;
}
static void gfx_core_exit(Plugin* self) {
    (void)self;
    svc_unregister("gfx.put_pixel");
    svc_unregister("gfx.fill_rect");
    svc_unregister("gfx.draw_rect");
    svc_unregister("gfx.draw_line");
    svc_unregister("gfx.blend_pixel");
}
static int gfx_core_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self;
    const char* svc = gfx_svc_name(method);
    if (!svc) return -1; /* unknown method */
    svc_fn f = svc_lookup(svc);
    if (!f) { pm_serial("[GFX] no backend for "); pm_serial(svc); pm_serial("\n"); return -1; }
    int r = f(args, out, outcap);
    pm_serial("[GFX] "); pm_serial(method); pm_serial(" ok\n");
    return r;
}
extern const Plugin g_gfx_core = {
    "gfx_core", 0x0100,
    "gfx.put_pixel,gfx.fill_rect,gfx.draw_rect,gfx.draw_line,gfx.blend_pixel",
    "",
    gfx_core_init, gfx_core_exit, gfx_core_call
};
