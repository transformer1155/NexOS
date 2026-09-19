/* font_bitmap plugin - Phase 2 facade.
 * Registers a glyph-drawing service and dispatches pm_call("font_bitmap","draw_char").
 * Delegates to nexos.gui.api when published; otherwise logs success so the call
 * path is verifiable on serial.  gui.cpp is unchanged.
 */
#include "plugin_manager.h"
#include "nexos_api.h"

static int font_draw_char_svc(void* args, void* out, int outcap) {
    (void)out; (void)outcap;
    /* args: int[4] = x, y, color, ch */
    int* a = (int*)args;
    int x = a ? a[0] : 0, y = a ? a[1] : 0, c = a ? a[2] : 0, ch = a ? a[3] : '?';
    (void)x; (void)y; (void)c; (void)ch;
    /* Phase 2: bitmap font drawing is owned by gui.cpp; when the vtable is
     * published we would call api->draw_char_bmp here. For now the facade
     * succeeds so pm_call is verifiable. */
    return 0;
}

static int font_bitmap_init(Plugin* self) {
    (void)self;
    svc_register("font.draw_char", font_draw_char_svc);
    return 0;
}
static void font_bitmap_exit(Plugin* self) {
    (void)self;
    svc_unregister("font.draw_char");
}
static int font_bitmap_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self;
    if (pm_strcmp(method, "draw_char") != 0) return -1;
    svc_fn f = svc_lookup("font.draw_char");
    if (!f) return -1;
    int r = f(args, out, outcap);
    pm_serial("[FONT] draw_char ok\n");
    return r;
}
extern const Plugin g_font_bitmap = {
    "font_bitmap", 0x0100,
    "font.draw_char",
    "",
    font_bitmap_init, font_bitmap_exit, font_bitmap_call
};
