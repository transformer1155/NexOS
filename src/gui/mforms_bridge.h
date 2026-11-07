// =====================================================================
//  mforms_bridge.h  -  NexOS.Forms host API: real in 32-bit, no-ops in 64-bit
// ---------------------------------------------------------------------
//  Extracted from gui_legacy.cpp so that EVERY translation unit that needs
//  the host API (gui_legacy.cpp, gfx_ime.cpp, ...) sees the same switch.
//  Without this, the 64-bit stubs would stay file-private inside
//  gui_legacy.cpp and a new TU would link in 32-bit and fail in 64-bit.
// =====================================================================
#ifndef NEXOS_GUI_MFORMS_BRIDGE_H
#define NEXOS_GUI_MFORMS_BRIDGE_H

// ---------------------------------------------------------------------
//  NexOS.Forms managed (C#) GUI shell bridge.
//  The shell is driven by MiniCLR, which only ships in the 32-bit kernel;
//  gui.cpp is shared with the 64-bit build (gui64.o), so under -m64 we
//  swap the host API for no-op stubs and the native drawers take over.
// ---------------------------------------------------------------------
#if defined(__i386__) || defined(NexOS_HAVE_MFORMS)
#  include "mforms.h"     // NexOS.Forms host: managed (C#) GUI shell bridge
#  ifndef NexOS_HAVE_MFORMS
#    define NexOS_HAVE_MFORMS 1
#  endif
#else
#  define NexOS_HAVE_MFORMS 0
static inline int         mforms_ready(void) { return 0; }
static inline int         mforms_open(int) { return -1; }
static inline void        mforms_close(int) {}
static inline const char* mforms_title(int) { return ""; }
static inline void        mforms_paint(int, int, int, int, int) {}
static inline int         mforms_click(int, int, int, int, int, int, int) { return 0; }
static inline int         mforms_drag(int, int, int, int, int, int, int) { return 0; }
static inline int         mforms_key(int, int) { return 0; }
static inline void        mforms_set_mouse(int, int) {}
static inline int         mforms_has_desktop(void) { return 0; }
static inline void        mforms_paint_desktop(int, int) {}
static inline void        mforms_paint_wall(int, int) {}
static inline void        mforms_paint_desk_icons(int, int) {}
static inline int         g_mforms_anim = 0;   // 64-bit build: no managed shell
static inline void        mforms_paint_overlay(int, int) {}
static inline int         mforms_desktop_click(int, int) { return -2; }
static inline int         mforms_desktop_drag(int, int, int) { return 0; }
static inline int         mforms_desktop_rclick(int, int) { return -2; }
static inline int         mforms_rclick(int, int, int, int, int, int, int) { return 0; }
static inline int         mforms_desktop_menu_open(void) { return 0; }
static inline int         mforms_desktop_key(int) { return 0; }
static inline int         mforms_desktop_menu(int) { return -1; }
static inline void        mforms_set_running(uint32_t) {}
static inline void        mforms_set_ime(int) {}
static inline int         mforms_paint_failed(int) { return 0; }
#endif

#endif // NEXOS_GUI_MFORMS_BRIDGE_H
