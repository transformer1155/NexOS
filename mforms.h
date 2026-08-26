#ifndef NexOS_MFORMS_H
#define NexOS_MFORMS_H
// =====================================================================
//  mforms.h  -  native host for the managed (C#) GUI shell
// ---------------------------------------------------------------------
//  NexOS.Forms is a Windows-Forms-shaped UI framework written in C# and
//  executed by MiniCLR.  Every window, control and application lives on
//  the managed side; this module is the only native code involved.  It
//  does two things:
//
//    1. Publishes an internal-call surface (NexOS.Forms.Gfx / Host) so
//       managed code can draw and read machine state.
//    2. Gives gui.cpp a handful of C entry points to paint a window,
//       route a click, or deliver a key.
//
//  gui.cpp owns the framebuffer but its Graphics type lives in an
//  anonymous namespace, so the drawing primitives arrive here as a
//  function table (MFormsHost) filled in at GUI start-up.  That keeps
//  this file free of any dependency on the window manager's internals.
// =====================================================================
#include <stdint.h>

extern "C" {

// ---------------------------------------------------------------------
//  Services gui.cpp lends to the managed shell.
//  All coordinates are absolute screen pixels; clipping to the client
//  area is mforms' job, not the caller's.
// ---------------------------------------------------------------------
struct MFormsHost {
    // ---- drawing ----------------------------------------------------
    void (*fill_rect)   (int x, int y, int w, int h, uint32_t c);
    void (*fill_round)  (int x, int y, int w, int h, int r, uint32_t c);
    void (*draw_round)  (int x, int y, int w, int h, int r, uint32_t c);
    void (*draw_rect)   (int x, int y, int w, int h, uint32_t c);
    void (*draw_line)   (int x0, int y0, int x1, int y1, uint32_t c);
    void (*fill_grad)   (int x, int y, int w, int h, uint32_t top, uint32_t bot);
    void (*text)        (int x, int y, const char* s, uint32_t fg);
    void (*text_bg)     (int x, int y, const char* s, uint32_t fg, uint32_t bg);
    void (*fill_circle) (int cx, int cy, int r, uint32_t c);
    void (*draw_circle) (int cx, int cy, int r, uint32_t c);
    void (*icon)        (int x, int y, int sz, uint32_t bg, char letter, uint32_t lc);
    void (*progress)    (int x, int y, int w, int h, int pct, uint32_t c);
    int  (*measure)     (const char* s);      // pixel width of a UTF-8 string
    // ---- texture cache (SFS-backed .tex files, see tools/tex_pack.py) --
    int  (*has_image)   (int id);
    void (*image)       (int id, int x, int y, int w, int h);  // stretch-blit
    int  screen_w;
    int  screen_h;

    // ---- machine state ----------------------------------------------
    uint32_t (*mem_total_kb)(void);
    uint32_t (*mem_free_pages)(void);
    uint32_t (*mem_used_pages)(void);
    uint32_t (*mem_total_pages)(void);
    uint32_t (*heap_alloc_bytes)(void);
    uint32_t (*heap_free_bytes)(void);
    uint32_t (*heap_alloc_count)(void);
    uint32_t (*heap_free_count)(void);
    void     (*optimize_memory)(void);
    // Millisecond counter (monotonic, wraps after 49 days).  Drives
    // double-click detection in managed code (File Explorer etc.).
    uint32_t (*tick_ms)(void);

    int  (*list_files)(int fs, char* buf, int bufsize);
    int  (*read_file) (int fs, const char* name, unsigned char* buf, int bufsize);
    // Write a file body to durable storage (MKFS data disk).  Used by the
    // managed shell to persist personalization settings ("nexos.cfg") and to
    // save Notepad documents.  fs selects the volume (0=MKFS). Returns bytes
    // written, or -1 on error.
    int  (*write_file)(int fs, const char* name, const unsigned char* buf, int size);
    int  (*mkdir)(int fs, const char* name);
    int  (*remove)(int fs, const char* name);
    int  (*rename)(int fs, const char* old_name, const char* new_name);
    // Synchronous HTTP GET for the managed Browser control.  Returns the
    // response body (or an empty string on error).  Caller copies it.
    const char* (*http_get)(const char* url);
    void (*get_time)  (int* h, int* m, int* s);

    const char* (*os_name)(void);
    const char* (*cpu_vendor)(void);
    const char* (*disk_model)(void);
    uint32_t    (*disk_size_mb)(void);
    int         (*is_64bit)(void);
    int         (*pci_count)(void);
    int         (*nic_present)(void);

    void (*exec_command)(const char* cmd, char* out, int outsize);
    void (*shutdown)(void);
    void (*reboot)(void);
    // Managed code asks the kernel to open (or focus) an application of
    // this managed Kind (e.g. Notepad from the File Explorer).
    void (*open_app)(int kind);
    // Managed code asks the kernel to close every window of a Kind
    // (taskbar right-click "Close window" / "End process").
    void (*close_app)(int kind);
    // Managed code asks the kernel to EXECUTE a native Windows PE image
    // (a .exe in SFS / mkfs) through the win32_run / win64_run loader and
    // surface the windows it creates on the desktop.  This is what makes
    // a double-click on foo.exe in the File Explorer actually *run* the
    // program instead of opening it in Notepad.  Returns the number of
    // desktop windows created, 0 when the image ran but drew nothing,
    // or a negative win32_run() error code when the load failed.
    int  (*run_exe)(const char* filename);
    // Managed code asks the kernel to leave GUI mode (text terminal).
    void (*exit_gui)(void);

    // ---- sign-in ------------------------------------------------------
    // The lock screen / sign-in page is managed code (Login.cs), but the
    // account database and the password hashes live in the kernel.  These
    // four slots are the whole contract:
    //   login_check  verify a username/password pair.  On success the
    //                kernel commits the session (current uid, euid, sudo
    //                state) and returns the uid; -1 means "rejected".
    //   login_uid    uid of the session already signed in, or -1 when the
    //                machine is still locked.  Lets the shell skip the
    //                login page when the desktop is re-entered from the
    //                text terminal.
    //   user_count / user_name  enumerate accounts so the page can show a
    //                Windows-style user picker instead of a blank field.
    int         (*login_check)(const char* user, const char* pass);
    int         (*login_uid)(void);
    int         (*user_count)(void);
    const char* (*user_name)(int idx);
};

// ---------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------
// Publish the internal calls.  Safe to call more than once; the host
// table is copied.  Must run before mforms_start().
void mforms_init(const MFormsHost* host);

// Load shell.mex and run NexOS.Forms.Shell::Init.  0 on success.
//   -1 CLR not ready   -2 image load failed   -3 Init faulted
int  mforms_start(void);

// True once mforms_start() has succeeded.
int  mforms_ready(void);

// Human-readable reason the last operation failed.
const char* mforms_report(void);

// ---------------------------------------------------------------------
//  Applications
// ---------------------------------------------------------------------
// Ask the shell to instantiate application `kind` (indices agree with
// NexOS.Forms.AppKind on the managed side).  Returns an app id >= 0.
int  mforms_open(int kind);
void mforms_close(int id);

// Window title chosen by the managed app; "" when unavailable.
const char* mforms_title(int id);

// Paint app `id` into the client rectangle (screen coordinates).
void mforms_paint(int id, int ox, int oy, int w, int h);

// Deliver a click at screen point (mx,my).  Returns non-zero when the
// app consumed it.  Client rect is passed so layout matches the paint.
int  mforms_click(int id, int ox, int oy, int w, int h, int mx, int my);

// Deliver a key.  `ch` is an ASCII code, or a negative virtual key
// (-1 backspace, -2 enter, -3 up, -4 down, -5 left, -6 right, -7 esc).
int  mforms_key(int id, int ch);

// Publish the pointer position (screen coordinates) so managed code can
// draw hover states.  Call once per frame before painting.
void mforms_set_mouse(int mx, int my);

// ---------------------------------------------------------------------
//  Desktop / shell surface
// ---------------------------------------------------------------------
// Non-zero when shell.mex provides a managed desktop.
int  mforms_has_desktop(void);

// Managed shell requests continuous repaints (AI desktop thinking dots /
// typewriter reveal).  The GUI main loop polls this and throttles
// render_all() to ~30 fps while it is set, because nothing else triggers
// repaints between input events.
extern int g_mforms_anim;

// Layer 1: wallpaper + desktop icons, painted *behind* the windows.
void mforms_paint_desktop(int w, int h);

// Layer 2: taskbar + Start menu, painted *above* the windows.
void mforms_paint_overlay(int w, int h);

// Route a click at screen (mx,my) into the shell.  Returns the app Kind
// to launch, -1 when the shell consumed the click, or -2 when the point
// belongs to no shell element and the caller should try its windows.
int  mforms_desktop_click(int mx, int my);

// Deliver a keystroke to the desktop surface when no window is focused
// (e.g. the desktop inline-rename editor).  Returns non-zero if consumed.
int  mforms_desktop_key(int ch);

// Right-click on the desktop surface (taskbar / tray / wallpaper).  Opens
// the kernel-native context menu; returns 0 (the menu is a popup, not a
// launch target).
int  mforms_desktop_rclick(int mx, int my);

// Right-click inside a managed window (e.g. the File Explorer).  mx,my are
// screen coordinates; ox,oy,w,h are the window's client rect.
int  mforms_rclick(int id, int ox, int oy, int w, int h, int mx, int my);

// Non-zero while the Start menu is open (it is modal: give it the click).
int  mforms_desktop_menu_open(void);

// Bit i set == a window of Kind i is open.  Drives the taskbar's
// running-app indicators; call once per frame before painting.
void mforms_set_running(uint32_t mask);

// Managed heap pressure, 0..100, for the task manager and diagnostics.
int  mforms_heap_pct(void);

} // extern "C"

#endif // NexOS_MFORMS_H
