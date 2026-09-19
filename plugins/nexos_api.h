/* nexos_api.h - live implementation vtable published by gui.cpp (Phase 2b).
 * Plugins look up "nexos.gui.api" via svc_lookup() to call the real renderer.
 * Declared here so plugin modules can type the pointer.  Phase 2 only defines
 * the shape; gui.cpp fills it in during its init (deferred to Phase 2b).
 */
#ifndef NEXOS_API_H
#define NEXOS_API_H
#include <stdint.h>

typedef uint32_t Color;

/* Function-pointer shape of the services the GUI plugins will own.  Filled by
 * gui.cpp at boot; plugins never call gui.cpp directly, only through this. */
typedef struct NexosGuiAPI {
    void (*put_pixel)(int x, int y, Color c);
    void (*fill_rect)(int x, int y, int w, int h, Color c);
    void (*draw_rect)(int x, int y, int w, int h, Color c);
    void (*draw_line)(int x0, int y0, int x1, int y1, Color c);
    void (*blend_pixel)(int x, int y, Color c, int alpha);
    int  (*ease_out_cubic)(int p);
    int  (*ease_in_out_cubic)(int p);
} NexosGuiAPI;

/* Service name carrying the above vtable. */
#define NEXOS_GUI_API_SVC "nexos.gui.api"

#endif /* NEXOS_API_H */
