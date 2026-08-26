// =====================================================================
//  mforms.cpp  -  native host for NexOS.Forms (the managed GUI shell)
// ---------------------------------------------------------------------
//  Two responsibilities, and deliberately nothing else:
//
//    * publish NexOS.Forms.Gfx / NexOS.Forms.Host as internal calls so
//      C# can draw and read machine state;
//    * expose a small C API so gui.cpp can paint a managed window and
//      route input into it.
//
//  Coordinate contract: managed code always works in *client* pixels
//  with the origin at the top-left of the window's content area.  This
//  file adds the window origin and clips to the client rectangle, so a
//  buggy C# app can smear inside its own window but never over the title
//  bar, the taskbar or a neighbouring window.
// =====================================================================
#include "mforms.h"
#include "clr.h"

// Clipboard is shared with the kernel (terminal copy/paste) so that text
// copied in a managed app can be pasted into the text terminal and vice
// versa.
extern char g_clipboard[256];
extern void clipboard_set(const char* text, int len);

// COM1 debug log.  Each freestanding TU keeps its own copy (there is no
// shared serial object to link against), so define one here too.
static inline void mf_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static void serial_puts(const char* s) {
    while (s && *s) mf_outb(0x3F8, (uint8_t)*s++);
}
static void serial_putdec(int v) {
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { int t = v; if (v < 0){ b[i++]='-'; t=-t; } while(t){ b[i++]=(char)('0'+t%10); t/=10; } }
    for (int j = i-1; j >= 0; j--) mf_outb(0x3F8, (uint8_t)b[j]);
}

// =====================================================================
//  local string helpers (freestanding: no libc)
// =====================================================================
namespace {

int slen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

void scpy(char* d, const char* s, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void scat(char* d, const char* s, int cap) {
    int i = slen(d);
    int j = 0;
    while (s && s[j] && i < cap - 1) d[i++] = s[j++];
    d[i] = 0;
}

// =====================================================================
//  host table + paint context
// =====================================================================
MFormsHost g_h;
bool       g_have_host = false;
bool       g_ready     = false;
char       g_report[128] = "mforms: not started";

// Current client origin and clip rectangle, in screen pixels.
// Right/bottom are exclusive.
int g_ox = 0, g_oy = 0;
int g_cl = 0, g_ct = 0, g_cr = 0, g_cbot = 0;
int g_cw = 0, g_ch = 0;          // client size handed to the managed app

// Pointer position in *screen* coordinates.  Kept in screen space so a
// single update per frame stays valid for every window painted after
// it; Gfx::MouseX/Y subtract the client origin on the way out.
int g_msx = -1, g_msy = -1;

void set_context(int ox, int oy, int w, int h) {
    g_ox = ox; g_oy = oy;
    g_cw = w;  g_ch = h;
    g_cl = ox; g_ct = oy;
    g_cr = ox + w; g_cbot = oy + h;
    if (g_have_host) {
        if (g_cl < 0) g_cl = 0;
        if (g_ct < 0) g_ct = 0;
        if (g_cr > g_h.screen_w) g_cr = g_h.screen_w;
        if (g_cbot > g_h.screen_h) g_cbot = g_h.screen_h;
    }
}

// Intersect a screen-space rectangle with the clip box.
// Returns false when nothing survives.
bool clip_rect(int& x, int& y, int& w, int& h) {
    int x1 = x + w, y1 = y + h;
    if (x < g_cl) x = g_cl;
    if (y < g_ct) y = g_ct;
    if (x1 > g_cr) x1 = g_cr;
    if (y1 > g_cbot) y1 = g_cbot;
    w = x1 - x; h = y1 - y;
    return w > 0 && h > 0;
}

bool box_visible(int x, int y, int w, int h) {
    return x < g_cr && y < g_cbot && x + w > g_cl && y + h > g_ct;
}

bool box_inside(int x, int y, int w, int h) {
    return x >= g_cl && y >= g_ct && x + w <= g_cr && y + h <= g_cbot;
}

// =====================================================================
//  text clipping
// ---------------------------------------------------------------------
//  gui.cpp's text routines have no clip box, so a long label would run
//  straight out of the window.  Rather than teach Graphics about clips,
//  trim the string here: drop glyphs that fall left of the clip, stop at
//  the first glyph that would cross the right edge.  ASCII advances 8px,
//  3-byte UTF-8 (CJK) advances 16px, which matches draw_text_utf8.
// =====================================================================
constexpr int TEXTBUF = 512;
char g_textbuf[TEXTBUF];

// Returns the x at which g_textbuf should be drawn, or -1 to skip.
int clip_text(int sx, int sy, const char* s) {
    if (!s || !*s) return -1;
    if (sy + 16 <= g_ct || sy >= g_cbot) return -1;   // fully above/below

    int out = 0;
    int x   = sx;
    int startx = -1;

    while (*s) {
        unsigned char c = (unsigned char)*s;
        int adv, blen;
        if (c < 0x80) {
            if (c == '\n') break;                   // callers draw one line
            adv = 8; blen = 1;
        } else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
                                          (s[2] & 0xC0) == 0x80) {
            adv = 16; blen = 3;
        } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            s += 2; continue;                       // unsupported, skip
        } else { s++; continue; }

        if (x + adv > g_cr) break;                  // would cross right edge
        if (x >= g_cl) {                            // fully visible glyph
            if (startx < 0) startx = x;
            if (out + blen >= TEXTBUF - 1) break;
            for (int i = 0; i < blen; i++) g_textbuf[out++] = s[i];
        }
        x += adv;
        s += blen;
    }
    g_textbuf[out] = 0;
    return out ? startx : -1;
}

// =====================================================================
//  NexOS.Forms.Gfx  --  drawing
//  Arguments arrive in client coordinates.
// =====================================================================
int32_t g_fill_rect(int32_t* a) {
    int x = g_ox + a[0], y = g_oy + a[1], w = a[2], h = a[3];
    if (clip_rect(x, y, w, h)) g_h.fill_rect(x, y, w, h, (uint32_t)a[4]);
    return 0;
}

int32_t g_fill_round(int32_t* a) {
    int x = g_ox + a[0], y = g_oy + a[1], w = a[2], h = a[3];
    if (!box_visible(x, y, w, h)) return 0;
    if (box_inside(x, y, w, h)) {
        g_h.fill_round(x, y, w, h, a[4], (uint32_t)a[5]);
    } else {
        // Partially off-screen: keep the pixels, lose the corners.
        if (clip_rect(x, y, w, h)) g_h.fill_rect(x, y, w, h, (uint32_t)a[5]);
    }
    return 0;
}

int32_t g_draw_round(int32_t* a) {
    int x = g_ox + a[0], y = g_oy + a[1], w = a[2], h = a[3];
    if (box_inside(x, y, w, h)) g_h.draw_round(x, y, w, h, a[4], (uint32_t)a[5]);
    return 0;
}

int32_t g_draw_rect(int32_t* a) {
    int x = g_ox + a[0], y = g_oy + a[1], w = a[2], h = a[3];
    if (box_inside(x, y, w, h)) g_h.draw_rect(x, y, w, h, (uint32_t)a[4]);
    return 0;
}

int32_t g_draw_line(int32_t* a) {
    int x0 = g_ox + a[0], y0 = g_oy + a[1];
    int x1 = g_ox + a[2], y1 = g_oy + a[3];
    // Only axis-aligned segments are clipped precisely; those are the
    // ones UI code actually draws (separators, rules, grid lines).
    if (y0 == y1) {
        if (y0 < g_ct || y0 >= g_cbot) return 0;
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        if (x0 < g_cl) x0 = g_cl;
        if (x1 >= g_cr) x1 = g_cr - 1;
        if (x0 > x1) return 0;
    } else if (x0 == x1) {
        if (x0 < g_cl || x0 >= g_cr) return 0;
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        if (y0 < g_ct) y0 = g_ct;
        if (y1 >= g_cbot) y1 = g_cbot - 1;
        if (y0 > y1) return 0;
    } else if (!box_inside(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1,
                           (x0 < x1 ? x1 - x0 : x0 - x1) + 1,
                           (y0 < y1 ? y1 - y0 : y0 - y1) + 1)) {
        return 0;
    }
    g_h.draw_line(x0, y0, x1, y1, (uint32_t)a[4]);
    return 0;
}

int32_t g_fill_grad(int32_t* a) {
    int x = g_ox + a[0], y = g_oy + a[1], w = a[2], h = a[3];
    if (box_inside(x, y, w, h))
        g_h.fill_grad(x, y, w, h, (uint32_t)a[4], (uint32_t)a[5]);
    return 0;
}

int32_t g_text(int32_t* a) {
    int sx = g_ox + a[0], sy = g_oy + a[1];
    int dx = clip_text(sx, sy, clr_str(a[2]));
    if (dx >= 0) g_h.text(dx, sy, g_textbuf, (uint32_t)a[3]);
    return 0;
}

int32_t g_text_bg(int32_t* a) {
    int sx = g_ox + a[0], sy = g_oy + a[1];
    int dx = clip_text(sx, sy, clr_str(a[2]));
    if (dx >= 0) g_h.text_bg(dx, sy, g_textbuf, (uint32_t)a[3], (uint32_t)a[4]);
    return 0;
}

// Text horizontally centred inside a client-space box.
int32_t g_text_center(int32_t* a) {
    const char* s = clr_str(a[3]);
    int tw = g_h.measure(s);
    int sx = g_ox + a[0] + (a[2] - tw) / 2;
    if (sx < g_ox + a[0]) sx = g_ox + a[0];
    int sy = g_oy + a[1];
    int dx = clip_text(sx, sy, s);
    if (dx >= 0) g_h.text(dx, sy, g_textbuf, (uint32_t)a[4]);
    return 0;
}

int32_t g_fill_circle(int32_t* a) {
    int cx = g_ox + a[0], cy = g_oy + a[1], r = a[2];
    if (box_inside(cx - r, cy - r, 2 * r + 1, 2 * r + 1))
        g_h.fill_circle(cx, cy, r, (uint32_t)a[3]);
    return 0;
}

int32_t g_draw_circle(int32_t* a) {
    int cx = g_ox + a[0], cy = g_oy + a[1], r = a[2];
    if (box_inside(cx - r, cy - r, 2 * r + 1, 2 * r + 1))
        g_h.draw_circle(cx, cy, r, (uint32_t)a[3]);
    return 0;
}

int32_t g_icon(int32_t* a) {
    int x = g_ox + a[0], y = g_oy + a[1], sz = a[2];
    if (box_inside(x, y, sz, sz))
        g_h.icon(x, y, sz, (uint32_t)a[3], (char)a[4], (uint32_t)a[5]);
    return 0;
}

int32_t g_progress(int32_t* a) {
    int x = g_ox + a[0], y = g_oy + a[1], w = a[2], h = a[3];
    if (box_inside(x, y, w, h))
        g_h.progress(x, y, w, h, a[4], (uint32_t)a[5]);
    return 0;
}

int32_t g_measure(int32_t* a)  { return g_h.measure(clr_str(a[0])); }

// SFS texture cache (id -> .tex, see tools/tex_pack.py).  Coordinates are
// client-relative like every other Gfx call, so offset by the window origin
// and clip to the window box.
int32_t g_has_image(int32_t* a) {
    if (!g_h.has_image) return 0;
    return g_h.has_image(a[0]);
}
int32_t g_image(int32_t* a) {
    if (!g_h.image) return 0;
    // Native draw_image clips to the framebuffer, so draw whenever the
    // rect intersects the window box (partial overlap included).
    g_h.image(a[0], g_ox + a[1], g_oy + a[2], a[3], a[4]);
    return 0;
}

int32_t g_width(int32_t*)      { return g_cw; }
int32_t g_height(int32_t*)     { return g_ch; }
int32_t g_screen_w(int32_t*)   { return (int32_t)g_h.screen_w; }
int32_t g_screen_h(int32_t*)   { return (int32_t)g_h.screen_h; }
int32_t g_mouse_x(int32_t*)    { return g_msx < 0 ? -1 : g_msx - g_ox; }
int32_t g_mouse_y(int32_t*)    { return g_msy < 0 ? -1 : g_msy - g_oy; }
int32_t g_origin_x(int32_t*)   { return g_ox; }
int32_t g_origin_y(int32_t*)   { return g_oy; }

// Arm the shared button press animation (screen coords).  The managed
// Btn class turns this into a shrink-to-half-and-restore on whichever
// W.Button/Primary/Key sits under the point.
static void press_screen(int sx, int sy) {
    int32_t a[2] = { sx, sy }, r = 0;
    clr_call("NexOS.Forms.Btn::PressScreen", a, 2, &r);
}
// Synthetic pointer position for voice / automation clicks: set the native
// cursor to (x,y) in the CURRENT client context so Gfx.MouseX/Y report it.
int32_t g_set_mouse(int32_t* a) { g_msx = a[0] + g_ox; g_msy = a[1] + g_oy; return 0; }

// =====================================================================
//  NexOS.Forms.Host  --  machine state
// =====================================================================
// NOTE: these forward to function pointers in g_h. The Makefile compiles
// this file with -fno-optimize-sibling-calls so GCC emits `call *ptr`
// instead of a tail `jmp *ptr` (the latter faults in this build).
int32_t h_mem_total(int32_t*)  { return (int32_t)g_h.mem_total_kb(); }
int32_t h_pages_free(int32_t*) { return (int32_t)g_h.mem_free_pages(); }
int32_t h_pages_used(int32_t*) { return (int32_t)g_h.mem_used_pages(); }
int32_t h_pages_all(int32_t*)  { return (int32_t)g_h.mem_total_pages(); }
int32_t h_heap_alloc(int32_t*) { return (int32_t)g_h.heap_alloc_bytes(); }
int32_t h_heap_free(int32_t*)  { return (int32_t)g_h.heap_free_bytes(); }
int32_t h_heap_ac(int32_t*)    { return (int32_t)g_h.heap_alloc_count(); }
int32_t h_heap_fc(int32_t*)    { return (int32_t)g_h.heap_free_count(); }
int32_t h_optimize(int32_t*)   { if (g_h.optimize_memory) g_h.optimize_memory(); return 0; }

int32_t h_time_h(int32_t*) { int h=0,m=0,s=0; g_h.get_time(&h,&m,&s); return h; }
int32_t h_time_m(int32_t*) { int h=0,m=0,s=0; g_h.get_time(&h,&m,&s); return m; }
int32_t h_time_s(int32_t*) { int h=0,m=0,s=0; g_h.get_time(&h,&m,&s); return s; }

int32_t h_os_name(int32_t*)    { return clr_new_str(g_h.os_name ? g_h.os_name() : "NexOS"); }
int32_t h_cpu_vendor(int32_t*) { return clr_new_str(g_h.cpu_vendor ? g_h.cpu_vendor() : "unknown"); }
int32_t h_disk_model(int32_t*) { return clr_new_str(g_h.disk_model ? g_h.disk_model() : "unknown"); }
int32_t h_disk_mb(int32_t*)    { return g_h.disk_size_mb ? (int32_t)g_h.disk_size_mb() : 0; }
int32_t h_is64(int32_t*)       { return g_h.is_64bit ? g_h.is_64bit() : 0; }
int32_t h_pci(int32_t*)        { return g_h.pci_count ? g_h.pci_count() : 0; }
int32_t h_nic(int32_t*)        { return g_h.nic_present ? g_h.nic_present() : 0; }

int32_t h_ticks(int32_t*) {
    uint32_t t;
    __asm__ volatile("rdtsc" : "=a"(t) :: "edx");
    return (int32_t)(t >> 10);
}

// Managed code opens an app (e.g. Notepad from the File Explorer).
int32_t h_open_app(int32_t* a) {
    if (g_h.open_app) g_h.open_app(a ? a[0] : -1);
    return 0;
}

int32_t h_close_app(int32_t* a) {
    if (g_h.close_app) g_h.close_app(a ? a[0] : -1);
    return 0;
}

int32_t h_exit_gui(int32_t*) {
    if (g_h.exit_gui) g_h.exit_gui();
    return 0;
}

// ---- sign-in -------------------------------------------------------
// The lock screen is managed code; the password hashes are not.  These
// four thunks are the only way Login.cs can reach the account database.
// h_login_check returns the uid on success (and the kernel has committed
// the session by then) or -1 when the credentials are rejected.
int32_t h_login_check(int32_t* a) {
    if (!g_h.login_check) return -1;
    return (int32_t)g_h.login_check(clr_str(a[0]), clr_str(a[1]));
}

int32_t h_login_uid(int32_t*) {
    return g_h.login_uid ? (int32_t)g_h.login_uid() : -1;
}

int32_t h_user_count(int32_t*) {
    return g_h.user_count ? (int32_t)g_h.user_count() : 0;
}

int32_t h_user_name(int32_t* a) {
    if (!g_h.user_name) return clr_new_str("");
    const char* s = g_h.user_name(a ? (int)a[0] : -1);
    return clr_new_str(s ? s : "");
}

// Monotonic milliseconds (PIT-calibrated by gui.cpp).  Used by managed
// double-click detection; unlike h_ticks its rate is known.
int32_t h_tick_ms(int32_t*) { return g_h.tick_ms ? (int32_t)g_h.tick_ms() : 0; }

// Which application kinds currently have a window open.  gui.cpp owns
// the window list, so it pushes a bitmask in before each frame and the
// managed taskbar reads it back here.
uint32_t g_run_mask = 0;
int32_t h_run_mask(int32_t*) { return (int32_t)g_run_mask; }

// ---- file system -----------------------------------------------------
//  list_files() hands back one entry per line, directories prefixed with
//  "[D] ".  Parsing that in C# would need a string API we do not have,
//  so the listing is cached here and exposed as count/name/isdir.
constexpr int FLIST_MAX = 4096;
char g_flist[FLIST_MAX];
int  g_flist_fs = -1;
int  g_flist_n  = 0;
int  g_flist_off[128];           // byte offset of each line

void flist_load(int fs) {
    if (g_flist_fs == fs) return;      // already snapshotted this frame
    g_flist[0] = 0;
    g_flist_n  = 0;
    g_flist_fs = fs;
    if (!g_h.list_files) return;
    int n = g_h.list_files(fs, g_flist, FLIST_MAX - 1);
    if (n < 0) n = 0;
    g_flist[n] = 0;
    // Split in place: record each line start, terminate at newline.
    int i = 0;
    while (g_flist[i] && g_flist_n < 128) {
        g_flist_off[g_flist_n++] = i;
        while (g_flist[i] && g_flist[i] != '\n') i++;
        if (g_flist[i] == '\n') { g_flist[i] = 0; i++; }
    }
    // Trailing empty line from a final '\n'.
    while (g_flist_n > 0 && g_flist[g_flist_off[g_flist_n - 1]] == 0) g_flist_n--;
}

const char* flist_line(int fs, int idx) {
    flist_load(fs);
    if (idx < 0 || idx >= g_flist_n) return "";
    return g_flist + g_flist_off[idx];
}

int32_t h_file_count(int32_t* a) { flist_load(a[0]); return g_flist_n; }

int32_t h_file_is_dir(int32_t* a) {
    const char* l = flist_line(a[0], a[1]);
    return (l[0] == '[' && l[1] == 'D' && l[2] == ']') ? 1 : 0;
}

int32_t h_file_name(int32_t* a) {
    const char* l = flist_line(a[0], a[1]);
    if (l[0] == '[' && l[1] == 'D' && l[2] == ']') l += 3;
    while (*l == ' ') l++;
    // File names may contain spaces (e.g. desktop shortcuts "This PC.lnk",
    // "AI Agent.lnk").  The ONLY reliable name boundary is the SFS size
    // annotation " (digitsB)" the kernel appends after the name; MKFS and
    // the Desktop folder emit a bare "<name>\n" with no suffix, so for those
    // the name runs to end of line.  Stopping at the first space (the old
    // behaviour) made every space-containing file un-deletable / un-openable
    // / un-renamable, because FileDelete/Open/Rename then looked up a
    // truncated name ("This", "AI") and silently no-op'd.
    const char* end = l;
    while (end[0]) end++;                       // end -> NUL terminator
    for (const char* p = l; *p; p++) {
        if (*p == '(' && p > l && p[-1] == ' ' &&
            p[1] >= '0' && p[1] <= '9') {
            const char* q = p + 1;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == 'B' && q[1] == ')') { end = p; break; }
        }
    }
    while (end > l && (end[-1] == '\n' || end[-1] == ' ')) end--;
    int len = (int)(end - l);
    char buf[64];
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    for (int i = 0; i < len; i++) buf[i] = l[i];
    buf[len] = 0;
    return clr_new_str(buf);
}

// Invalidate the listing so the next query re-reads the medium.
int32_t h_file_refresh(int32_t*) { g_flist_fs = -1; return 0; }

// File-system mutations, backed by the kernel SFS (mkfs).  These let the
// kernel-native context menus create / delete / rename files the same
// way the terminal's mkdir / rm / copy do.
int32_t h_file_mkdir(int32_t* a) {
    if (!g_h.mkdir) return -1;
    return g_h.mkdir(a[0], clr_str(a[1]));   // a[0]=fs, a[1]=name
}
int32_t h_file_delete(int32_t* a) {
    if (!g_h.remove) return -1;
    int r = g_h.remove(a[0], clr_str(a[1])); // a[0]=fs, a[1]=name
    g_flist_fs = -1;                 // listing changed; force re-read
    return r;
}
int32_t h_file_rename(int32_t* a) {
    if (!g_h.rename) return -1;
    int r = g_h.rename(a[0], clr_str(a[1]), clr_str(a[2])); // a[0]=fs
    g_flist_fs = -1;
    return r;
}

constexpr int TEXT_MAX = 3072;
char g_textread[TEXT_MAX + 1];

// Write buffer for Host.WriteText (settings + Notepad docs).  Large enough
// for a modest document; the kernel mkfs.create path truncates at the volume
// limit anyway, and the managed side keeps its own copy of the text.
constexpr int WRITE_MAX = 65536;
char g_textwrite[WRITE_MAX + 1];

int32_t h_read_text(int32_t* a) {
    if (!g_h.read_file) return clr_new_str("");
    int n = g_h.read_file(a[0], clr_str(a[1]),
                          (unsigned char*)g_textread, TEXT_MAX);
    if (n < 0) n = 0;
    // Managed strings are UTF-8 and NUL-terminated; scrub embedded NULs
    // and control bytes so a binary file cannot truncate the result.
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)g_textread[i];
        if (c == 0) g_textread[i] = ' ';
        else if (c < 0x20 && c != '\n' && c != '\t') g_textread[i] = '.';
    }
    g_textread[n] = 0;
    return clr_new_str(g_textread);
}

// Host.WriteText(fs, name, text): persist a UTF-8 text body to stable
// storage.  Returns bytes written (>=0) or -1 on error.  Used by the
// managed shell to save personalization settings ("nexos.cfg") and Notepad
// documents.  Matches the read side's clr_str/a[] indexing convention.
int32_t h_write_text(int32_t* a) {
    if (!g_h.write_file) return -1;
    const char* s = clr_str(a[2]);
    int len = 0; while (s && s[len]) len++;          // local strlen (no libc in freestanding 64-bit)
    if (len > WRITE_MAX) len = WRITE_MAX;
    for (int i = 0; i < len; i++) g_textwrite[i] = s[i];
    int n = g_h.write_file(a[0], clr_str(a[1]), (const unsigned char*)g_textwrite, len);
    return (int32_t)n;
}

constexpr int EXEC_MAX = 4096;
char g_execbuf[EXEC_MAX];

int32_t h_exec(int32_t* a) {
    if (!g_h.exec_command) return clr_new_str("");
    g_execbuf[0] = 0;
    g_h.exec_command(clr_str(a[0]), g_execbuf, EXEC_MAX - 1);
    g_execbuf[EXEC_MAX - 1] = 0;
    return clr_new_str(g_execbuf);
}

// Host.RunExe(name): execute a native Windows PE image through the
// win32/win64 loader and surface its windows.  This is what a double-click
// on a .exe in the File Explorer calls, so programs actually RUN instead of
// being dumped into Notepad.  Returns the window count (>=0) or a negative
// loader error code.
int32_t h_run_exe(int32_t* a) {
    if (!g_h.run_exe) return -1;
    return (int32_t)g_h.run_exe(clr_str(a[0]));
}

int32_t h_shutdown(int32_t*) { if (g_h.shutdown) g_h.shutdown(); return 0; }
int32_t h_reboot(int32_t*)   { if (g_h.reboot)   g_h.reboot();   return 0; }

int32_t h_log(int32_t* a) { serial_puts(clr_str(a[0])); serial_puts("\n"); return 0; }

int32_t h_http_get(int32_t* a) {
    if (!g_h.http_get) return clr_new_str("");
    const char* r = g_h.http_get(clr_str(a[0]));
    return clr_new_str(r ? r : "");
}

// A one-character managed string.  Managed code has no char[]->string
// constructor, so text entry (terminal, notepad) builds strings a glyph
// at a time through this.  The argument is a Unicode codepoint, encoded
// here as UTF-8 so CJK input survives the C# <-> runtime boundary.
int32_t h_charstr(int32_t* a) {
    uint32_t cp = (uint32_t)a[0];
    char b[8];
    int n = 0;
    if (cp < 0x80) {
        b[n++] = (char)cp;
    } else if (cp < 0x800) {
        b[n++] = (char)(0xC0 | (cp >> 6));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        b[n++] = (char)(0xE0 | (cp >> 12));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        b[n++] = (char)(0xF0 | (cp >> 18));
        b[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[n++] = (char)(0x80 | (cp & 0x3F));
    }
    b[n] = 0;
    return clr_new_str(b);
}

// Host.GetClipboard(): return the shared kernel clipboard as a managed string.
int32_t h_clip_get(int32_t*) {
    return clr_new_str(g_clipboard);
}

// Host.SetAnim(on): the managed shell asks the host to keep repainting so
// its animations (AI desktop thinking dots, typewriter reveal) can progress.
// The GUI main loop polls g_mforms_anim and throttles render_all() to ~30 fps
// while set, because between input events nothing else triggers a repaint.
extern "C" { int g_mforms_anim = 0; }
int32_t h_set_anim(int32_t* a) { g_mforms_anim = (a && a[0]) ? 1 : 0; return 0; }

// Host.SetPixel(mode, scale, scan): the managed shell (Theme) pushes its
// retro "pixel / CRT monitor" settings down to the kernel so the single
// framebuffer post-process in gui.cpp::pixelate_framebuffer() can read them.
//   mode  : 0 off, 1 on
//   scale : block size in pixels (1 = full detail, >1 = chunkier pixels)
//   scan  : 0 off, 1 on (CRT scanline darkening)
extern "C" { int g_pixel_mode = 0; int g_pixel_scale = 1; int g_pixel_scan = 0; }
int32_t h_set_pixel(int32_t* a) {
    g_pixel_mode  = (a && a[0]) ? 1 : 0;
    g_pixel_scale = (a && a[1] > 1) ? a[1] : 1;
    g_pixel_scan  = (a && a[2]) ? 1 : 0;
    return 0;
}

// Host.SetClipboard(text): copy a managed string into the shared clipboard.
int32_t h_clip_set(int32_t* a) {
    const char* s = clr_str(a[0]);
    int len = slen(s);
    clipboard_set(s, len);
    return 0;
}

// =====================================================================
//  registration
// =====================================================================
struct Reg { const char* name; clr_icall_fn fn; };

const Reg g_regs[] = {
    // ---- Gfx ----
    { "NexOS.Forms.Gfx::FillRect",    g_fill_rect    },
    { "NexOS.Forms.Gfx::FillRound",   g_fill_round   },
    { "NexOS.Forms.Gfx::DrawRound",   g_draw_round   },
    { "NexOS.Forms.Gfx::DrawRect",    g_draw_rect    },
    { "NexOS.Forms.Gfx::DrawLine",    g_draw_line    },
    { "NexOS.Forms.Gfx::Gradient",    g_fill_grad    },
    { "NexOS.Forms.Gfx::Text",        g_text         },
    { "NexOS.Forms.Gfx::TextBg",      g_text_bg      },
    { "NexOS.Forms.Gfx::TextCenter",  g_text_center  },
    { "NexOS.Forms.Gfx::FillCircle",  g_fill_circle  },
    { "NexOS.Forms.Gfx::DrawCircle",  g_draw_circle  },
    { "NexOS.Forms.Gfx::Icon",        g_icon         },
    { "NexOS.Forms.Gfx::Progress",    g_progress     },
    { "NexOS.Forms.Gfx::HasImage",    g_has_image    },
    { "NexOS.Forms.Gfx::Image",       g_image        },
    { "NexOS.Forms.Gfx::Measure",     g_measure      },
    { "NexOS.Forms.Gfx::Width",       g_width        },
    { "NexOS.Forms.Gfx::Height",      g_height       },
    { "NexOS.Forms.Gfx::ScreenW",     g_screen_w     },
    { "NexOS.Forms.Gfx::ScreenH",     g_screen_h     },
    { "NexOS.Forms.Gfx::MouseX",      g_mouse_x      },
    { "NexOS.Forms.Gfx::MouseY",      g_mouse_y      },
    { "NexOS.Forms.Gfx::SetMouse",    g_set_mouse    },
    { "NexOS.Forms.Gfx::OriginX",     g_origin_x     },
    { "NexOS.Forms.Gfx::OriginY",     g_origin_y     },
    // ---- Host ----
    { "NexOS.Forms.Host::MemTotalKb",   h_mem_total   },
    { "NexOS.Forms.Host::PagesFree",    h_pages_free  },
    { "NexOS.Forms.Host::PagesUsed",    h_pages_used  },
    { "NexOS.Forms.Host::PagesTotal",   h_pages_all   },
    { "NexOS.Forms.Host::HeapAlloc",    h_heap_alloc  },
    { "NexOS.Forms.Host::HeapFree",     h_heap_free   },
    { "NexOS.Forms.Host::HeapAllocCnt", h_heap_ac     },
    { "NexOS.Forms.Host::HeapFreeCnt",  h_heap_fc     },
    { "NexOS.Forms.Host::Optimize",     h_optimize    },
    { "NexOS.Forms.Host::Hour",         h_time_h      },
    { "NexOS.Forms.Host::Minute",       h_time_m      },
    { "NexOS.Forms.Host::Second",       h_time_s      },
    { "NexOS.Forms.Host::OsName",       h_os_name     },
    { "NexOS.Forms.Host::CpuVendor",    h_cpu_vendor  },
    { "NexOS.Forms.Host::DiskModel",    h_disk_model  },
    { "NexOS.Forms.Host::DiskSizeMb",   h_disk_mb     },
    { "NexOS.Forms.Host::Is64Bit",      h_is64        },
    { "NexOS.Forms.Host::PciCount",     h_pci         },
    { "NexOS.Forms.Host::NicPresent",   h_nic         },
    { "NexOS.Forms.Host::Ticks",        h_ticks       },
    { "NexOS.Forms.Host::TickMs",       h_tick_ms     },
    { "NexOS.Forms.Host::SetAnim",      h_set_anim    },
    { "NexOS.Forms.Host::RunningMask",  h_run_mask    },
    { "NexOS.Forms.Host::FileCount",    h_file_count  },
    { "NexOS.Forms.Host::FileName",     h_file_name   },
    { "NexOS.Forms.Host::FileIsDir",    h_file_is_dir },
    { "NexOS.Forms.Host::FileRefresh",  h_file_refresh},
    { "NexOS.Forms.Host::FileMkDir",    h_file_mkdir  },
    { "NexOS.Forms.Host::FileDelete",   h_file_delete },
    { "NexOS.Forms.Host::FileRename",   h_file_rename },
    { "NexOS.Forms.Host::ReadText",     h_read_text   },
    { "NexOS.Forms.Host::WriteText",    h_write_text  },
    { "NexOS.Forms.Host::Exec",         h_exec        },
    { "NexOS.Forms.Host::RunExe",       h_run_exe     },
    { "NexOS.Forms.Host::Shutdown",     h_shutdown    },
    { "NexOS.Forms.Host::Reboot",       h_reboot      },
    { "NexOS.Forms.Host::Log",          h_log         },
    { "NexOS.Forms.Host::CharStr",      h_charstr     },
    { "NexOS.Forms.Host::HttpGet",      h_http_get    },
    { "NexOS.Forms.Host::GetClipboard", h_clip_get    },
    { "NexOS.Forms.Host::SetClipboard", h_clip_set    },
    { "NexOS.Forms.Host::OpenApp",      h_open_app    },
    { "NexOS.Forms.Host::CloseApp",     h_close_app   },
    { "NexOS.Forms.Host::ExitGui",      h_exit_gui    },
    { "NexOS.Forms.Host::LoginCheck",   h_login_check },
    { "NexOS.Forms.Host::LoginUid",     h_login_uid   },
    { "NexOS.Forms.Host::UserCount",    h_user_count  },
    { "NexOS.Forms.Host::UserName",     h_user_name   },
    { "NexOS.Forms.Host::SetPixel",     h_set_pixel   },
};
const int G_REG_COUNT = (int)(sizeof(g_regs) / sizeof(g_regs[0]));

// ---------------------------------------------------------------------
//  Managed heap discipline
// ---------------------------------------------------------------------
//  The CLR heap is a bump allocator.  Painting allocates strings, so a
//  frame that did not rewind would exhaust it in seconds.  Persistent
//  state (created by Init/Open and mutated by Click/Key) must survive,
//  so the watermark is re-baselined after every event and rewound only
//  around a paint.
constexpr uint32_t CLR_HEAP_BYTES = 512u * 1024u;   // must match clr.cpp
uint32_t g_persist = 0;
bool     g_heap_warned = false;

void rebaseline() {
    g_persist = clr_heap_mark();
    if (!g_heap_warned && g_persist > CLR_HEAP_BYTES / 4 * 3) {
        g_heap_warned = true;
        serial_puts("[MFORMS] warning: persistent managed state above 75% of heap\n");
    }
}

// A managed event handler that faulted (typically by exhausting the bump
// heap) leaves the watermark at its high point.  Every later call would then
// fault immediately on its first allocation, so rewind to the last
// known-good persistent watermark and let the next frame try again.
// Without this the shell was one bad event away from being dead for good.
void heap_recover() {
    clr_heap_reset(g_persist);
    serial_puts("[MFORMS] managed heap rewound after fault\n");
}

char g_title[64];

} // namespace

// =====================================================================
//  public API
// =====================================================================
// diag_step (defined in kernel64.cpp for 64-bit, no-op stub in kernel.cpp for
// 32-bit) replaces colour beacons for progress tracing -- logs step name to
// serial, never paints the whole screen.
extern "C" void diag_step(uint32_t id, const char* name);

extern "C" void mforms_init(const MFormsHost* host) {
    if (!host) return;
    g_h = *host;
    g_have_host = true;
    for (int i = 0; i < G_REG_COUNT; i++)
        clr_register_icall(g_regs[i].name, g_regs[i].fn);
    set_context(0, 0, g_h.screen_w, g_h.screen_h);
}

extern "C" int mforms_start(void) {
    g_ready = false;
    if (!g_have_host) { scpy(g_report, "mforms: no host table", sizeof(g_report)); return -1; }
    if (!clr_ready())  { scpy(g_report, "mforms: CLR not initialised", sizeof(g_report)); return -1; }

    if (clr_load("shell.mex") != 0) {
        scpy(g_report, "mforms: ", sizeof(g_report));
        scat(g_report, clr_last_report(), sizeof(g_report));
        return -2;
    }

    int32_t r = 0;
    if (clr_call("NexOS.Forms.Shell::Init", nullptr, 0, &r) != 0) {
        scpy(g_report, "mforms: ", sizeof(g_report));
        scat(g_report, clr_last_report(), sizeof(g_report));
        return -3;
    }

    rebaseline();
    g_ready = true;
    scpy(g_report, "mforms: shell.mex ready", sizeof(g_report));
    serial_puts("[MFORMS] managed shell ready\n");
    return 0;
}

extern "C" int mforms_ready(void) { return g_ready && clr_loaded(); }
extern "C" const char* mforms_report(void) { return g_report; }

extern "C" int mforms_open(int kind) {
    if (!mforms_ready()) return -1;
    int32_t a = kind, r = -1;
    if (clr_call("NexOS.Forms.Shell::Open", &a, 1, &r) != 0) {
        scpy(g_report, clr_last_report(), sizeof(g_report));
        heap_recover();
        return -1;
    }
    rebaseline();                    // the new app's state must persist
    return (int)r;
}

extern "C" void mforms_close(int id) {
    if (!mforms_ready() || id < 0) return;
    int32_t a = id, r = 0;
    // Only re-baseline on success: baselining after a fault would bake the
    // blown watermark into the persistent floor and never give it back.
    if (clr_call("NexOS.Forms.Shell::Close", &a, 1, &r) == 0) rebaseline();
    else heap_recover();
}

extern "C" const char* mforms_title(int id) {
    g_title[0] = 0;
    if (!mforms_ready() || id < 0) return g_title;
    uint32_t mark = clr_heap_mark();
    int32_t a = id, r = 0;
    if (clr_call("NexOS.Forms.Shell::Title", &a, 1, &r) == 0 && r)
        scpy(g_title, clr_str(r), sizeof(g_title));
    clr_heap_reset(mark);
    return g_title;
}

extern "C" void mforms_paint(int id, int ox, int oy, int w, int h) {
    if (!mforms_ready() || id < 0) return;
    set_context(ox, oy, w, h);
    g_flist_fs = -1;                 // listings are per-frame snapshots
    uint32_t mark = clr_heap_mark();
    int32_t a[3] = { id, w, h }, r = 0;
    clr_call("NexOS.Forms.Shell::Paint", a, 3, &r);
    clr_heap_reset(mark);
}

extern "C" int mforms_click(int id, int ox, int oy, int w, int h,
                            int mx, int my) {
    if (!mforms_ready() || id < 0) return 0;
    set_context(ox, oy, w, h);
    g_msx = mx; g_msy = my;
    g_flist_fs = -1;
    press_screen(mx, my);            // arm button press animation (screen)
    int32_t a[3] = { id, mx - ox, my - oy }, r = 0;
    if (clr_call("NexOS.Forms.Shell::Click", a, 3, &r) != 0) { heap_recover(); return 0; }
    rebaseline();                    // a click may create durable state
    return (int)r;
}

extern "C" int mforms_key(int id, int ch) {
    if (!mforms_ready() || id < 0) return 0;
    int32_t a[2] = { id, ch }, r = 0;
    int rc = clr_call("NexOS.Forms.Shell::Key", a, 2, &r);
    if (rc != 0) { heap_recover(); return 0; }
    rebaseline();
    return (int)r;
}

extern "C" void mforms_set_mouse(int mx, int my) { g_msx = mx; g_msy = my; }

// Deliver a keystroke to the desktop surface (inline rename editor).
extern "C" int mforms_desktop_key(int ch) {
    if (!mforms_ready()) return 0;
    int32_t a[1] = { ch }, r = 0;
    if (clr_call("NexOS.Forms.Desktop::Key", a, 1, &r) != 0) { heap_recover(); return 0; }
    rebaseline();
    return (int)r;
}

extern "C" int mforms_has_desktop(void) {
    if (!mforms_ready()) return 0;
    // The managed Win11 shell always owns the desktop surface once it is
    // up, so simply mirror the ready state.  (Probing the C# Shell::
    // HasDesktop via clr_call was fragile: it cached a 0 on a transient
    // first-call failure and then disabled all desktop input forever.)
    return 1;
}

extern "C" void mforms_paint_desktop(int w, int h) {
    if (!mforms_ready()) return;
    diag_step(200, "mforms_paint_desktop enter");
    extern int g_clr_trace;
    g_clr_trace = 1;
    set_context(0, 0, w, h);
    g_flist_fs = -1;
    uint32_t mark = clr_heap_mark();
    int32_t a[2] = { w, h }, r = 0;
    clr_call("NexOS.Forms.Shell::PaintDesktop", a, 2, &r);
    clr_heap_reset(mark);
    diag_step(201, "mforms_paint_desktop returned -> DeferredRun");
    // AI desktop deferred work (e.g. agent run) must happen AFTER the paint
    // heap is reset so its allocations survive as persistent state.
    if (clr_call("NexOS.Forms.Desktop::DeferredRun", nullptr, 0, &r) == 0)
        rebaseline();
    else
        heap_recover();
    // g_clr_trace left ON intentionally for diagnosis (every frame traced).
}

// Taskbar and Start menu, painted after the windows so the shell chrome
// is never covered by one.
extern "C" void mforms_paint_overlay(int w, int h) {
    if (!mforms_ready()) return;
    set_context(0, 0, w, h);
    g_flist_fs = -1;
    uint32_t mark = clr_heap_mark();
    int32_t a[2] = { w, h }, r = 0;
    clr_call("NexOS.Forms.Shell::PaintOverlay", a, 2, &r);
    clr_heap_reset(mark);
    // Voice: execute deferred synthetic clicks now that the paint heap is
    // reset, so any window/app a handler opens survives as persistent
    // state (mirrors the Desktop::DeferredRun pattern above).
    if (clr_call("NexOS.Forms.Voice::Dispatch", nullptr, 0, &r) != 0)
        heap_recover();
    else
        rebaseline();
}

// -2 == "not mine, hit-test your windows".  Note the failure path also
// returns -2: 0 is a valid Kind (ControlPanel) and must never be
// synthesised by an error.
extern "C" int mforms_desktop_click(int mx, int my) {
    if (!mforms_ready()) return -2;
    set_context(0, 0, g_h.screen_w, g_h.screen_h);
    g_msx = mx; g_msy = my;
    press_screen(mx, my);            // arm button press animation (screen)
    int32_t a[2] = { mx, my }, r = 0;
    if (clr_call("NexOS.Forms.Shell::DesktopClick", a, 2, &r) != 0) { heap_recover(); return -2; }
    rebaseline();
    return (int)r;
}

// Right-click on the desktop surface (taskbar / tray / wallpaper).
extern "C" int mforms_desktop_rclick(int mx, int my) {
    if (!mforms_ready()) return -2;
    set_context(0, 0, g_h.screen_w, g_h.screen_h);
    g_msx = mx; g_msy = my;
    int32_t a[2] = { mx, my }, r = 0;
    if (clr_call("NexOS.Forms.Shell::DesktopRClick", a, 2, &r) != 0) { heap_recover(); return -2; }
    rebaseline();
    return (int)r;
}

// Right-click inside a managed window (e.g. the File Explorer).
// mx,my are screen coordinates; the managed handler gets them local
// plus the window origin so it can position a popup.
extern "C" int mforms_rclick(int id, int ox, int oy, int w, int h,
                             int mx, int my) {
    if (!mforms_ready() || id < 0) return 0;
    set_context(ox, oy, w, h);
    g_msx = mx; g_msy = my;
    int32_t a[5] = { id, mx - ox, my - oy, ox, oy }, r = 0;
    if (clr_call("NexOS.Forms.Shell::RightClick", a, 5, &r) != 0) { heap_recover(); return 0; }
    rebaseline();
    return (int)r;
}

// True while the Start menu is up; the host then gives it every click.
extern "C" int mforms_desktop_menu_open(void) {
    if (!mforms_ready()) return 0;
    int32_t r = 0;
    if (clr_call("NexOS.Forms.Shell::DesktopMenuOpen", nullptr, 0, &r) != 0) return 0;
    return r ? 1 : 0;
}

extern "C" void mforms_set_running(uint32_t mask) { g_run_mask = mask; }

extern "C" int mforms_heap_pct(void) {
    // CLR_HEAP_SIZE is private to clr.cpp; the ratio is what callers
    // actually want and CLR_HEAP_BYTES mirrors the value both were built to.
    return (int)((clr_heap_used() * 100u) / CLR_HEAP_BYTES);
}
