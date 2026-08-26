// =====================================================================
//  win32.h  -  NexOS Win32 subsystem: shared types between win32.cpp,
//              kernel.cpp and gui.cpp
// ---------------------------------------------------------------------
//  The Win32 subsystem provides:
//    * A simulated Windows registry (HKLM / HKCU / HKCR / HKU / HKCC)
//    * A PE32 image loader (sections, base relocations, imports)
//    * Emulated kernel32 / user32 / gdi32 / advapi32 / msvcrt exports
//    * A GDI display list so real Win32 windows can be rendered by the
//      NexOS GUI window manager.
// =====================================================================
#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------
//  GDI display list  -  produced by the app during WM_PAINT, consumed by
//  the NexOS GUI compositor (gui.cpp).
// ---------------------------------------------------------------------
enum W32CmdKind : uint8_t {
    W32_CMD_FILLRECT = 0,   // solid rectangle
    W32_CMD_FRAMERECT,      // 1px outline
    W32_CMD_TEXT,           // text string
    W32_CMD_LINE,           // line from (x,y) to (x+w, y+h)
    W32_CMD_ELLIPSE,        // ellipse bounded by the rectangle
    W32_CMD_BUTTON,         // pseudo control: 3D button with caption
    W32_CMD_PIXEL,          // single pixel
};

struct W32DrawCmd {
    uint8_t  kind;
    int16_t  x, y, w, h;
    uint32_t color;         // 0x00RRGGBB
    uint32_t bkcolor;       // 0x00RRGGBB (text background, 0xFFFFFFFF = transparent)
    uint16_t id;            // child-control id (control's menu/resource id)
    char     text[48];
};

// A window's whole frame is one display list, so the cap is really "how
// complex may a Win32 app's client area be".  72 was enough for a static
// splash but not for a browser that paints chrome, a document and a
// clickable link list, so the budget is 320 entries -- about 21 KiB per
// window, 85 KiB for the four slots, which lives in .bss.
constexpr int W32_MAX_CMDS    = 320;
constexpr int W32_MAX_WINDOWS = 4;

struct W32WinInfo {
    uint32_t hwnd;
    int      x, y, w, h;
    char     title[48];
    char     cls[32];
    uint8_t  is_msgbox;
    uint8_t  visible;
};

extern "C" {

// ---- lifecycle -------------------------------------------------------
// reader(name, buf, bufsize) -> bytes read or <0; writer(text) -> console
void win32_init(int (*reader)(const char*, uint8_t*, int),
                void (*writer)(const char*));

// Load + execute a PE32 image.  Returns:
//   0  success            -1 file not found      -2 not a PE32 image
//  -3  unsupported PE     -4 out of memory       -5 unresolved imports
int  win32_run(const char* filename, const char* args, int info_only);

// Human readable text of the last load (imports, sections, errors).
const char* win32_last_report(void);

// ---- registry --------------------------------------------------------
int  win32_reg_query(const char* path, const char* value, char* out, int outsz);
int  win32_reg_set(const char* path, const char* value, const char* type,
                   const char* data);
int  win32_reg_delete(const char* path, const char* value);
int  win32_reg_list(const char* path, char* out, int outsz);
int  win32_reg_tree(const char* path, char* out, int outsz, int max_depth);
int  win32_reg_key_count(void);
int  win32_reg_value_count(void);

// ---- simulated environment ------------------------------------------
const char* win32_env_get(const char* name);
int  win32_env_list(char* out, int outsz);

// ---- GUI bridge ------------------------------------------------------
int  win32_window_count(void);
int  win32_window_info(int idx, W32WinInfo* out);   // 1 = live window, 0 = empty slot
int  win32_window_cmds(int idx, const W32DrawCmd** out);   // returns count
void win32_window_repaint(int idx);                        // re-run WM_PAINT
int  win32_window_dispatch(int idx, uint32_t msg, uint32_t wp, uint32_t lp);
// Hit-test a click (client-local coords lx,ly) against this window's BUTTON
// controls.  Returns the control id, or 0 if nothing was hit.
int  win32_window_button_hit(int idx, int lx, int ly);
void win32_window_close(int idx);
void win32_reset(void);          // free all images / windows

// ---- popup menus (user32 TrackPopupMenu bridge) ----------------------
// TrackPopupMenu() registers a menu session that gui.cpp renders in its
// main loop; choosing an item posts WM_COMMAND(id) to the owner window.
// Return 1 while a session is active (also outputs its origin + owner).
int  win32_menu_active(int* x, int* y, uint32_t* hwnd);
int  win32_menu_item_count(void);            // items of the active menu
const char* win32_menu_item_text(int i);     // label of item i
int  win32_menu_item_flags(int i);           // MF_* flags of item i
void win32_menu_choose(int i);               // pick item i -> WM_COMMAND
void win32_menu_dismiss(void);               // close without choosing

} // extern "C"
