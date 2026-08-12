// =====================================================================
//  gui.cpp  -  Win11-style Graphical User Interface for NexOS
// ---------------------------------------------------------------------
//  Features:
//  - Top status bar (Win11 style, not bottom)
//  - Desktop icons (Control Panel, File Explorer, Task Manager, etc.)
//  - Draggable windows with title bars and close buttons
//  - Start menu with app launcher
//  - Control Panel: system info, display settings, memory optimization
//  - File Explorer: browse MKFS/SFS/FAT32 file systems
//  - Task Manager: memory stats, page usage, heap info
//  - Memory optimization: coalesce heap, display before/after stats
//  - System tray with clock
// =====================================================================

#include <stdint.h>
#include "zfont_data.h"   // embedded GB2312 16x16 CJK font (387 glyphs)
#include "ime_dict.h"     // pinyin -> Hanzi dictionary for IME
#include "win32.h"        // Win32 subsystem: registry, PE32 loader, GDI display list

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
static inline int         mforms_key(int, int) { return 0; }
static inline void        mforms_set_mouse(int, int) {}
static inline int         mforms_has_desktop(void) { return 0; }
static inline void        mforms_paint_desktop(int, int) {}
static inline void        mforms_paint_overlay(int, int) {}
static inline int         mforms_desktop_click(int, int) { return -2; }
static inline int         mforms_desktop_rclick(int, int) { return -2; }
static inline int         mforms_rclick(int, int, int, int, int, int, int) { return 0; }
static inline int         mforms_desktop_menu_open(void) { return 0; }
static inline int         mforms_desktop_key(int) { return 0; }
static inline void        mforms_set_running(uint32_t) {}
#endif

// Height of the managed taskbar.  Mirrors NexOS.Forms.Desktop.TaskH:
// the strip is reserved so a click on the bar is never eaten by a window
// that reaches the bottom of the screen.
constexpr int MANAGED_TASKBAR_H = 48;

// Clipboard is owned by the kernel but shared with the GUI (terminal /
// browser URL copy-paste) and the C# host bridge.
extern char g_clipboard[256];
extern int  g_clipboard_len;
extern void clipboard_set(const char* text, int len);

// ---- VBE info structure (at physical address 0x5000, set by stage2/UEFI) ----
// Extended for real-hardware UEFI GOP support
struct VbeInfo {
    uint32_t framebuffer_phys;   // 0x5000 - low 32 bits of framebuffer (or shadow)
    uint16_t width;              // 0x5004
    uint16_t height;             // 0x5006
    uint8_t  bpp;                // 0x5008
    uint16_t pitch;              // 0x5009
    uint16_t mode_number;        // 0x500B
    uint8_t  vbe_ok;             // 0x500D
    uint8_t  vbe_mode_set;       // 0x500E: 1=mode already set by BIOS/UEFI
    uint8_t  pixel_format;       // 0x500F: 0=BGRX32, 1=RGBX32, 2=RGB24, 3=RGB565, 4=BltOnly
    uint64_t framebuffer_phys64; // 0x5010: full 64-bit framebuffer address
    uint32_t shadow_buffer;      // 0x5018: shadow buffer addr if framebuffer > 4GB
    uint8_t  reserved[4];        // 0x501C-0x501F: padding
} __attribute__((packed));

// Pixel format constants (must match bootuefi.c)
#define PXF_BGRX32   0  // Blue-Green-Red-Reserved (most common UEFI)
#define PXF_RGBX32   1  // Red-Green-Blue-Reserved
#define PXF_RGB24    2  // 24-bit packed RGB
#define PXF_RGB565   3  // 16-bit RGB565
#define PXF_BLT_ONLY 4  // No linear framebuffer

// ---- Port I/O helpers ----
static inline void outb(uint16_t p, uint8_t v) {
    __asm__ __volatile__("outb %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint8_t inb(uint16_t p) {
    uint8_t v;
    __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void outw(uint16_t p, uint16_t v) {
    __asm__ __volatile__("outw %0,%1" :: "a"(v), "Nd"(p));
}
static inline uint16_t inw(uint16_t p) {
    uint16_t v;
    __asm__ __volatile__("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

// ---- Bochs VBE I/O port interface ----
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF
#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40

static void vbe_write_reg(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}
static void bochs_vbe_set_mode(uint16_t width, uint16_t height, uint8_t bpp) {
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vbe_write_reg(VBE_DISPI_INDEX_XRES, width);
    vbe_write_reg(VBE_DISPI_INDEX_YRES, height);
    vbe_write_reg(VBE_DISPI_INDEX_BPP, bpp);
    vbe_write_reg(VBE_DISPI_INDEX_VIRT_WIDTH, width);
    vbe_write_reg(VBE_DISPI_INDEX_VIRT_HEIGHT, height);
    vbe_write_reg(VBE_DISPI_INDEX_X_OFFSET, 0);
    vbe_write_reg(VBE_DISPI_INDEX_Y_OFFSET, 0);
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
}
static void bochs_vbe_disable(void) {
    vbe_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
}

// ---- BGA (Bochs Graphics Adapter) detection ----
// BGA ports (0x1CE/0x1CF) exist only in QEMU/Bochs/VirtualBox.
// Real hardware uses INT 10h (handled by stage2.asm) or UEFI GOP.
static bool bga_detect(void) {
    outw(VBE_DISPI_IOPORT_INDEX, VBE_DISPI_INDEX_ID);
    uint16_t id = inw(VBE_DISPI_IOPORT_DATA);
    // Valid BGA IDs: 0xB0C0 through 0xB0C6
    return (id >= 0xB0C0 && id <= 0xB0C6);
}

static bool g_bga_available = false;  // true if Bochs VBE ports detected

// =====================================================================
//  Pinyin IME (中文输入法)
//  Global state bound to the active Terminal window. Typing a-z composes
//  pinyin; digits pick a candidate; space picks #1; ESC cancels.
// =====================================================================
static char   g_ime_py[16];         // composing pinyin
static int    g_ime_len = 0;
static bool   g_ime_active = false;
static int    g_ime_cands[9];       // candidate unicode codepoints
static int    g_ime_cand_count = 0;
static bool   g_ime_cn = false;     // language mode: true=中文, false=英文

static void ime_reset(){
    g_ime_len = 0;
    g_ime_active = false;
    g_ime_cand_count = 0;
    g_ime_py[0] = 0;
}

// Forward declaration: defined just below (see "Encode a Unicode codepoint").
static void utf8_encode(uint32_t cp, char out[4]);
// Forward declarations for the serial debug helpers (defined later in this file)
// so ime_commit() can log without moving the definitions.
static void serial_puts(const char* s);
static void serial_putdec(int v);

// Inject one committed Unicode codepoint into whichever input buffer is
// currently focused.  Returns the number of bytes appended.  `target_kind`
// selects where the glyph lands:
//   0 = native buffer (term_input / browser_url) passed via out_buf
//   1 = managed (C#) app, delivered through mforms_key(codepoint)
static int ime_commit(uint32_t cp, int target_kind, int app_id,
                      char* out_buf, int* out_len, int out_max){
    char u8[4];
    utf8_encode(cp, u8);
    if (target_kind == 1) {
        // Managed (C#) app: deliver the FULL Unicode codepoint as a single
        // key event.  Sending the raw UTF-8 bytes would split one glyph into
        // 2-3 byte events that the managed text box discards (its OnKey only
        // accepts ASCII).  The C# side re-inserts it via Host.CharStr(cp).
        mforms_key(app_id, (int)cp);
        serial_puts("[IME] commit U+");
        serial_putdec((int)cp);
        serial_puts(" mode=");
        serial_puts(g_ime_cn ? "CN" : "EN");
        serial_puts(" -> managed\n");
        return 1;
    }
    // Native buffer (terminal input line / browser URL): append the UTF-8
    // bytes directly.  Log once so a headless test can prove the CJK glyph
    // actually reached a native input box.
    serial_puts("[IME] commit U+");
    serial_putdec((int)cp);
    serial_puts(" -> native\n");
    int n = 0;
    for (int i = 0; u8[i] && *out_len < out_max - 1; i++)
        out_buf[(*out_len)++] = u8[i], n++;
    out_buf[*out_len] = 0;
    return n;
}

// Prefix match against the sorted pinyin dictionary.
static void ime_lookup(){
    g_ime_cand_count = 0;
    if (g_ime_len == 0) return;
    for (int i = 0; i < ime_count && g_ime_cand_count < 9; i++) {
        // prefix match: ime_pinyin[i] starts with g_ime_py
        const char* a = ime_pinyin[i];
        const char* b = g_ime_py;
        int n = g_ime_len;
        bool match = true;
        while (n > 0 && *a) { if (*a != *b) { match = false; break; } a++; b++; n--; }
        if (match && n == 0)
            g_ime_cands[g_ime_cand_count++] = ime_unicode[i];
    }
}

// Encode a Unicode codepoint as UTF-8 (up to 3 bytes).
static void utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp; out[1] = 0;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
    }
}

// Display width in pixels of a mixed UTF-8 string (ASCII 8px, CJK 16px).
static int utf8_display_width(const char* s) {
    int w = 0;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) { w += 8; s++; }
        else if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) { w += 16; s += 3; }
        else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { w += 8; s += 2; }
        else s++;
    }
    return w;
}
static bool g_vbe_mode_set_by_bios = false;  // true if mode set by BIOS/UEFI

// VGA text buffer pointer, defined in kernel.cpp.  In VBE/fb_console mode it
// points to a shadow buffer; in real VGA text mode it aliases 0xB8000.
extern volatile uint16_t* VGA_MEMORY;

// ---- Serial debug ----
static void serial_puts(const char* s) {
    while (*s) outb(0x3F8, (uint8_t)*s++);
}
static void serial_putc(char c) {
    outb(0x3F8, (uint8_t)c);
}
static void serial_putdec(int v) {
    char buf[12]; int n = 0;
    if (v < 0) { serial_putc('-'); v = -v; }
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) serial_putc(buf[--n]);
}
static void serial_puthex32(uint32_t v) {
    const char* h = "0123456789ABCDEF";
    for (int s = 28; s >= 0; s -= 4) serial_putc(h[(v >> s) & 0xF]);
}

// ---- Memory helpers ----
static void* memset_(void* d, int v, int n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)v;
    return d;
}
static void* memcpy_(void* d, const void* s, int n) {
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    while (n--) *dp++ = *sp++;
    return d;
}
static int strlen_(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static int strcmp_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static int strncmp_(const char* a, const char* b, int n) {
    while (n > 0 && *a && *a == *b) { a++; b++; n--; }
    return n == 0 ? 0 : (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void strcpy_(char* d, const char* s) {
    while ((*d++ = *s++));
}
static void int_to_str(int val, char* buf) {
    if (val < 0) { *buf++ = '-'; val = -val; }
    char tmp[12]; int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) *buf++ = tmp[--i];
    *buf = 0;
}
static void uint_to_str(uint32_t val, char* buf) {
    char tmp[12]; int i = 0;
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    while (i > 0) *buf++ = tmp[--i];
    *buf = 0;
}

// =====================================================================
//  Win11-style Color Palette
// =====================================================================
typedef uint32_t Color;

// Win11 accent colors
constexpr Color C_ACCENT       = 0x0067C0;  // Win11 blue
constexpr Color C_ACCENT_LIGHT = 0x4CC2FF;  // Light blue hover
constexpr Color C_ACCENT_DARK  = 0x003E92;  // Dark blue pressed
constexpr Color C_ACCENT_ORANGE = 0xFF8C00; // Orange accent for start menu icon

// Desktop wallpaper (gradient from dark blue to lighter blue)
constexpr Color C_WALLPAPER_TOP    = 0x0A2540;  // Deep navy
constexpr Color C_WALLPAPER_BOT    = 0x1B4965;  // Medium blue

// Top status bar
constexpr Color C_TOPBAR_BG        = 0x1F1F1F;  // Near-black with transparency feel
constexpr Color C_TOPBAR_BORDER    = 0x2D2D2D;
constexpr Color C_TOPBAR_TEXT      = 0xFFFFFF;
constexpr Color C_TOPBAR_HOVER     = 0x323232;

// Start menu
constexpr Color C_STARTMENU_BG     = 0x202020;
constexpr Color C_STARTMENU_BORDER = 0x3D3D3D;
constexpr Color C_STARTMENU_HOVER  = 0x383838;

// Windows
constexpr Color C_WIN_BG           = 0xF3F3F3;  // Win11 light gray window bg
constexpr Color C_WIN_TITLEBAR     = 0xFAFAFA;  // Win11 light titlebar
constexpr Color C_WIN_TITLEBAR_ACT = 0xFFFFFF;  // Active titlebar
constexpr Color C_WIN_BORDER       = 0xE5E5E5;
constexpr Color C_WIN_BORDER_ACT   = 0x0067C0;  // Active border = accent
constexpr Color C_WIN_SHADOW       = 0x80000000;
constexpr Color C_WIN_TEXT         = 0x1A1A1A;
constexpr Color C_WIN_TEXT_SEC     = 0x616161;

// Buttons
constexpr Color C_BTN_BG           = 0xF5F5F5;
constexpr Color C_BTN_HOVER        = 0xE5E5E5;
constexpr Color C_BTN_PRESSED      = 0xD0D0D0;
constexpr Color C_BTN_BORDER       = 0xCCCCCC;
constexpr Color C_BTN_TEXT         = 0x1A1A1A;

// Close button
constexpr Color C_CLOSE_HOVER      = 0xE81123;  // Red
constexpr Color C_CLOSE_TEXT       = 0xC9C9C9;

// Desktop icons
constexpr Color C_ICON_BG          = 0x3D5A80;
constexpr Color C_ICON_TEXT        = 0xFFFFFF;
constexpr Color C_ICON_SELECTED    = 0x4477AA;

// Status / progress
constexpr Color C_PROGRESS_BG      = 0xD0D0D0;
constexpr Color C_PROGRESS_FILL    = 0x0067C0;
constexpr Color C_MEM_GOOD         = 0x107C10;  // Green
constexpr Color C_MEM_WARN         = 0xCA5010;  // Orange
constexpr Color C_MEM_BAD          = 0xC50F1F;  // Red

// ---- Portal / Card UI colors (new) ----
constexpr Color C_PORTAL_BG       = 0xF0F2F5;  // Light gray page background
constexpr Color C_CARD_BG         = 0xFFFFFF;  // White card background
constexpr Color C_CARD_SHADOW     = 0x20000000; // Subtle shadow
constexpr Color C_CARD_HOVER      = 0xF5F7FA;  // Card hover background
constexpr Color C_CARD_BORDER     = 0xE8E8E8;  // Card border

// Category icon colors (Control Panel)
constexpr Color C_CAT_SYSTEM      = 0x0078D7;  // Blue - System & Security
constexpr Color C_CAT_NETWORK     = 0x0099BC;  // Teal - Network & Internet
constexpr Color C_CAT_HARDWARE    = 0x5C6BC0;  // Indigo - Hardware & Sound
constexpr Color C_CAT_PROGRAMS    = 0x66BB6A;  // Green - Programs
constexpr Color C_CAT_ACCOUNT     = 0x26A69A;  // Teal-green - User Accounts
constexpr Color C_CAT_APPEARANCE  = 0xEC407A;  // Pink - Appearance
constexpr Color C_CAT_TIME        = 0x7E57C2;  // Purple - Time & Region
constexpr Color C_CAT_ACCESS      = 0x42A5F5;  // Light blue - Ease of Access

// Task Manager table colors
constexpr Color C_TM_HEADER_BG    = 0xF0F0F0;  // Table header background
constexpr Color C_TM_HEADER_TEXT  = 0x424242;  // Table header text
constexpr Color C_TM_ROW_HOVER    = 0xF5F9FF;  // Row hover
constexpr Color C_TM_ROW_SELECTED = 0xE5F3FF;  // Selected row
constexpr Color C_TM_SEARCH_BG    = 0xFFFFFF;  // Search bar background
constexpr Color C_TM_SEARCH_BORDER= 0xD0D0D0;  // Search bar border

// Portal desktop colors
constexpr Color C_PORTAL_SEARCH_BG = 0xFFFFFF;
constexpr Color C_PORTAL_SEARCH_BORDER = 0xD0D0D0;
constexpr Color C_PORTAL_TAB_BG    = 0xE8E8E8;
constexpr Color C_PORTAL_TAB_SEL   = 0x0067C0;
constexpr Color C_PORTAL_TAB_TEXT  = 0x333333;
constexpr Color C_PORTAL_TAB_TEXT_SEL = 0xFFFFFF;
constexpr Color C_PORTAL_WEATHER_BG = 0xE3F2FD;

// Quick access shortcut colors
constexpr Color C_SHORTCUT_ICON_BG = 0xF0F0F0;
constexpr Color C_SHORTCUT_TEXT    = 0x333333;

// Legacy compatibility
constexpr Color COLOR_BLACK   = 0x000000;
constexpr Color COLOR_WHITE   = 0xFFFFFF;
constexpr Color COLOR_RED     = 0xE81123;
constexpr Color COLOR_GREEN   = 0x107C10;
constexpr Color COLOR_BLUE    = 0x0067C0;
constexpr Color COLOR_YELLOW  = 0xFFFF00;
constexpr Color COLOR_GREY    = 0x808080;
constexpr Color COLOR_DARK_GREY  = 0x404040;
constexpr Color COLOR_LIGHT_GREY = 0xC0C0C0;
constexpr Color COLOR_CYAN    = 0x00AAAA;

// =====================================================================
//  8x16 BIOS-style bitmap font (code page 437)
// =====================================================================
const uint8_t font8x16[256][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x81,0xA5,0x81,0x81,0xBD,0x99,0x81,0x81,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0xFF,0xDB,0xFF,0xFF,0xC3,0xE7,0xFF,0xFF,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x6C,0xFE,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x18,0x3C,0x3C,0xE7,0xE7,0xE7,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x18,0x3C,0x7E,0xFF,0xFF,0x7E,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xE7,0xC3,0xC3,0xE7,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    {0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x42,0x42,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xC3,0x99,0xBD,0xBD,0x99,0xC3,0xFF,0xFF,0xFF,0xFF,0xFF},
    {0x00,0x00,0x1E,0x0E,0x1A,0x32,0x78,0xCC,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x3C,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3F,0x33,0x3F,0x30,0x30,0x30,0x30,0x70,0xF0,0xE0,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7F,0x63,0x7F,0x63,0x63,0x63,0x63,0x67,0xE7,0xE6,0xC0,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x18,0x18,0xDB,0x3C,0xE7,0x3C,0xDB,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x80,0xC0,0xE0,0xF0,0xF8,0xFE,0xF8,0xF0,0xE0,0xC0,0x80,0x00,0x00,0x00,0x00},
    {0x00,0x02,0x06,0x0E,0x1E,0x3E,0xFE,0x3E,0x1E,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x3C,0x7E,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x66,0x66,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7F,0xDB,0xDB,0xDB,0x7B,0x1B,0x1B,0x1B,0x1B,0x1B,0x00,0x00,0x00,0x00},
    {0x00,0x7C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0xF0,0xF0,0xF0,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0F,0x0F,0x0F,0x0F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xF0,0xF0,0xF0,0xF0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0,0xF0},
    {0x00,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    // 0x20 - 0x3F
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, // '!'
    {0x00,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, // '#'
    {0x18,0x18,0x18,0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x18,0x18,0x18,0x00,0x00,0x00}, // '$'
    {0x00,0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x63,0xC6,0x00,0x00,0x00,0x00}, // '%'
    {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, // '&'
    {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '\''
    {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '('
    {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // ')'
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00}, // '*'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00}, // ','
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, // '.'
    {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00}, // '/'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // '0'
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00}, // '1'
    {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00}, // '2'
    {0x00,0x00,0x7C,0xC6,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // '3'
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00}, // '4'
    {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // '5'
    {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // '6'
    {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, // '7'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // '8'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00,0x00}, // '9'
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, // ':'
    {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // ';'
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00}, // '<'
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '='
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00}, // '>'
    {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, // '?'
    // 0x40 - 0x5F
    {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0x70,0x00,0x00,0x00,0x00,0x00}, // '@'
    {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'A'
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00,0x00}, // 'B'
    {0x00,0x00,0x3C,0x66,0xC6,0xC0,0xC0,0xC0,0xC6,0x66,0x3C,0x00,0x00,0x00,0x00,0x00}, // 'C'
    {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00,0x00}, // 'D'
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x62,0x66,0xFE,0x00,0x00,0x00,0x00,0x00}, // 'E'
    {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00}, // 'F'
    {0x00,0x00,0x3C,0x66,0xC6,0xC0,0xC0,0xDE,0xC6,0x66,0x3C,0x00,0x00,0x00,0x00,0x00}, // 'G'
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'H'
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00}, // 'I'
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00,0x00}, // 'J'
    {0x00,0x00,0xE6,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00}, // 'K'
    {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x66,0xFE,0x00,0x00,0x00,0x00,0x00}, // 'L'
    {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'M'
    {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'N'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // 'O'
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00}, // 'P'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00,0x00,0x00}, // 'Q'
    {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00}, // 'R'
    {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // 'S'
    {0x00,0x00,0xFF,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00}, // 'T'
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // 'U'
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00,0x00}, // 'V'
    {0x00,0x00,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'W'
    {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'X'
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00}, // 'Y'
    {0x00,0x00,0xFE,0xC6,0x8C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00}, // 'Z'
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00,0x00}, // '['
    {0x00,0x00,0x00,0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00,0x00,0x00,0x00,0x00}, // '\'
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00}, // '_'
    // 0x60 - 0x7F
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, // 'a'
    {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00}, // 'b'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // 'c'
    {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, // 'd'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // 'e'
    {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00,0x00}, // 'f'
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00,0x00,0x00}, // 'g'
    {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00,0x00}, // 'h'
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00}, // 'i'
    {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,0x00,0x00}, // 'j'
    {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00,0x00}, // 'k'
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00}, // 'l'
    {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'm'
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00}, // 'n'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // 'o'
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00,0x00,0x00}, // 'p'
    {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00,0x00,0x00}, // 'q'
    {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00,0x00}, // 'r'
    {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x70,0x1C,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, // 's'
    {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00,0x00}, // 't'
    {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, // 'u'
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00,0x00}, // 'v'
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00,0x00}, // 'w'
    {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00}, // 'x'
    {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00,0x00,0x00}, // 'y'
    {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x66,0xFE,0x00,0x00,0x00,0x00,0x00}, // 'z'
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,0x00}, // '{'
    {0x00,0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, // '|'
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00}, // '}'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x72,0x9C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
    {0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00}, // 0x7F
};

#define F {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}

// =====================================================================
//  GUI Callbacks - registered by kernel for data access
// =====================================================================
struct GuiCallbacks {
    uint32_t (*get_total_mem_kb)(void);
    uint32_t (*get_free_pages)(void);
    uint32_t (*get_used_pages)(void);
    uint32_t (*get_total_pages)(void);
    uint32_t (*get_heap_alloc_bytes)(void);
    uint32_t (*get_heap_free_bytes)(void);
    uint32_t (*get_heap_alloc_count)(void);
    uint32_t (*get_heap_free_count)(void);
    void     (*optimize_memory)(void);        // force heap coalescing + page reclaim
    int      (*list_files)(int fs_type, char* buf, int bufsize);  // 0=mkfs,1=sfs,2=fat32
    int      (*read_file)(int fs_type, const char* name, uint8_t* buf, int bufsize); // returns bytes read or -1
    void     (*get_time)(int* h, int* m, int* s);
    // File-mutation (context-menu actions: new folder / delete / rename).
    int      (*mkdir)(int fs, const char* name);
    int      (*remove)(int fs, const char* name);
    int      (*rename)(int fs, const char* old_name, const char* new_name);
    const char* (*get_os_name)(void);
    bool     (*is_64bit)(void);
    // Browser callbacks
    int      (*browser_navigate)(const char* url);  // start navigation, returns 0 on success
    int      (*browser_status)(void);               // 0=idle,1=connecting,2=loading,3=done,-1=error
    int      (*browser_get_page)(char* buf, int bufsize); // get page body
    void     (*browser_reset)(void);                // reset browser state
    // Terminal command execution callback
    void     (*exec_command)(const char* cmd, char* output, int outsize); // execute shell command
    void     (*shutdown)(void);   // power off
    void     (*reboot)(void);     // restart system
    // Hardware info callbacks (for device adaptation display)
    const char* (*get_cpu_vendor)(void);
    const char* (*get_disk_model)(void);
    uint32_t (*get_disk_size_mb)(void);
    int      (*get_nic_present)(void);
    int      (*get_mouse_present)(void);
    int      (*get_keyboard_present)(void);
    uint32_t (*get_pci_count)(void);
    int      (*get_bga_available)(void);
    int      (*get_vbe_mode_set)(void);
    int      (*get_cpu_64bit_capable)(void);  // CPU supports 64-bit (vs currently running in 64-bit)
    // Synchronous HTTP GET for the managed Browser control.
    const char* (*http_get)(const char* url);
    // Session persistence: save/load a named blob to durable storage (the
    // MKFS data disk).  Used to keep the running-app list across a reboot.
    int (*session_save)(const char* name, const void* data, int size);
    int (*session_load)(const char* name, void* buf, int bufsize); // returns bytes or -1
    int (*session_clear)(const char* name);
    // Sign-in: the lock screen is drawn by managed code, but the account
    // database lives in the kernel.  login_check() verifies a credential
    // pair and, on success, commits the session (returns the uid, or -1);
    // login_uid() reports the uid already signed in (-1 while locked);
    // user_count()/user_name() enumerate accounts for the user picker.
    int         (*login_check)(const char* user, const char* pass);
    int         (*login_uid)(void);
    int         (*user_count)(void);
    const char* (*user_name)(int idx);
};

static GuiCallbacks g_cb;
static int g_startup_app_id = -1;   // set by kernel before gui_enter()

// Defined at the bottom of this file inside the extern "C" thunk block.
// Forward-declared here so Win11Desktop::launch_app() can drop straight back
// to the text-mode shell when the "Terminal" shortcut is opened, instead of
// painting a C# terminal window.
extern "C" void gui_exit(void);

// Defined in the extern "C" block at the bottom (session persistence).
extern "C" int gui_session_save(void);
extern "C" int gui_session_restore(void);

// =====================================================================
//  Graphics Driver
// =====================================================================
namespace {

// Heap allocation (defined in kernel.cpp / kernel64.cpp)
extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);
extern "C" int   winloader_capture_run(const char* filename, const char* args,
                                       char* out, int outsize);

constexpr int FONT_W = 8;
constexpr int FONT_H = 16;
constexpr int TOPBAR_H = 32;

// ---- SFS-backed texture cache (tools/tex_pack.py produces the .tex files) --
constexpr int TEX_MAX    = 128;
constexpr int TEX_WALL   = 0;    // full-screen wallpaper
constexpr int TEX_TASK   = 1;    // taskbar tile
constexpr int TEX_MENU   = 2;    // start/context menu tile
constexpr int TEX_CHROME = 3;    // title-bar tile
constexpr int TEX_WINBG  = 4;    // window client-area tile
constexpr int TEX_ICON   = 100;  // + AppKind -> app icon

struct TexRec {
    uint16_t w, h;
    uint8_t  fmt;            // 0 = RGB565, 1 = ARGB32
    bool     loaded;
    uint8_t* data;
};
static TexRec g_tex[TEX_MAX];

// Extract an 8-bit colour channel from an RGB565 texel.
static inline int tex_channel(uint16_t c, int ch) {
    if (ch == 0) { int v = (c >> 11) & 0x1F; return (v << 3) | (v >> 2); }
    if (ch == 1) { int v = (c >> 5) & 0x3F;  return (v << 2) | (v >> 4); }
    int v = c & 0x1F; return (v << 3) | (v >> 2);
}
// Bilinear blend of one channel across 4 RGB565 texels (xf/yf are 0..65535).
static inline int tex_bilerp(int a0, int a1, int b0, int b1, int xf, int yf) {
    int top = a0 + (((a1 - a0) * xf) >> 16);
    int bot = b0 + (((b1 - b0) * xf) >> 16);
    return top + (((bot - top) * yf) >> 16);
}

struct Graphics {
    volatile uint32_t* lfb;      // frontbuffer (VBE LFB)
    uint32_t*          backbuffer; // off-screen buffer (regular RAM)
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint8_t  bpp;
    uint8_t  pixel_format;       // 0=BGRX32, 1=RGBX32, 2=RGB24, 3=RGB565
    bool     initialized;

    void init() {
        volatile VbeInfo* info = (volatile VbeInfo*)0x5000;
        if (info->vbe_ok != 1) {
            // No VBE info from BIOS INT 10h or UEFI GOP. Try BGA ports
            // (QEMU/VirtualBox/Bochs always provide Bochs VBE ports even when
            // SeaBIOS/VBoxVGA don't expose classic VBE BIOS).
            if (!bga_detect()) {
                initialized = false;
                serial_puts("[GUI] VBE not available (no BIOS VBE, no BGA)\n");
                return;
            }
            // Synthesize a default VBE info so the rest of init works.
            // enable_vbe_mode() will actually set the mode via BGA ports.
            info->vbe_ok         = 1;
            info->vbe_mode_set   = 1;            // will be set via BGA
            info->width          = 1024;
            info->height         = 768;
            info->bpp            = 32;
            info->pitch          = 1024 * 4;
            info->mode_number    = 0x117;        // standard 1024x768x32
            info->pixel_format   = PXF_BGRX32;
            info->framebuffer_phys64 = 0xFD000000ULL;  // QEMU stdvga (bochs-display) BAR0 LFB
            info->framebuffer_phys   = 0xFD000000;
            info->shadow_buffer  = 1;   // VMM maps BGA LFB; do not heap-shadow it
            g_bga_available = true;
            serial_puts("[GUI] BGA fallback: synthesized VBE info (1024x768x32, BGRX32)\n");
        }

        // Check for BltOnly (no linear framebuffer)
        if (info->pixel_format == PXF_BLT_ONLY) {
            initialized = false;
            serial_puts("[GUI] GOP is BltOnly - no linear framebuffer\n");
            return;
        }

        lfb = (volatile uint32_t*)info->framebuffer_phys;
        width = info->width;
        height = info->height;
        pitch = info->pitch;
        bpp = info->bpp;
        pixel_format = info->pixel_format;

        // [DGOP] Dump the actual GOP/UEFI framebuffer parameters so real-hardware
        // mismatches (PixelsPerScanLine != HorizontalResolution, or FB above 4GB)
        // are visible in the serial log.  On Intel Iris Xe the pitch is usually
        // wider than width*bpp; present() now honors it.
        {
            uint32_t nominal = (uint32_t)width * (bpp / 8u);
            serial_puts("[DGOP] fb_base=0x");
            serial_puthex32(info->framebuffer_phys);
            serial_puts(" fb64=0x");
            serial_puthex32((uint32_t)(info->framebuffer_phys64 & 0xFFFFFFFFULL));
            serial_puts((info->framebuffer_phys64 >> 32) ? "HIGH " : " ");
            serial_puts("w=");
            serial_putdec((int)width);
            serial_puts(" h=");
            serial_putdec((int)height);
            serial_puts(" bpp=");
            serial_putdec((int)bpp);
            serial_puts(" pitch=");
            serial_putdec((int)pitch);
            serial_puts(" nominal=");
            serial_putdec((int)nominal);
            serial_puts(pitch == nominal ? " MATCH\n" : " MISMATCH(pitch>width)\n");
        }

        // For a framebuffer above 4 GB, kernel.cpp vmm_init() has already built
        // 4-level page tables that map the real high framebuffer into a <4GB
        // virtual window (0xF0000000) and rewritten framebuffer_phys to that
        // window address, with shadow_buffer set to 1 as the "mapped" sentinel.
        // So writing to framebuffer_phys is always safe here.  We only abort
        // when the full 64-bit address is above 4 GB AND shadow_buffer == 0
        // (meaning the kernel could not set up the mapping).

        // Validate framebuffer address is accessible (below 4GB for 32-bit kernel)
        if (info->framebuffer_phys64 > 0xFFFFFFFFULL && info->shadow_buffer == 0) {
            serial_puts("[GUI] WARNING: Framebuffer above 4GB without shadow buffer\n");
            initialized = false;
            return;
        }

        serial_puts("[GUI] Pixel format: ");
        serial_puts(pixel_format == PXF_BGRX32 ? "BGRX32" :
                    pixel_format == PXF_RGBX32 ? "RGBX32" :
                    pixel_format == PXF_RGB24  ? "RGB24"  :
                    pixel_format == PXF_RGB565 ? "RGB565" : "unknown");
        serial_puts("\n");

        // Detect BGA (Bochs VBE ports) for dynamic mode switching
        g_bga_available = bga_detect();
        // Check if mode was already set by BIOS (INT 10h) or UEFI (GOP)
        g_vbe_mode_set_by_bios = (info->vbe_mode_set == 1);

        serial_puts("[GUI] BGA: ");
        serial_puts(g_bga_available ? "yes" : "no");
        serial_puts(", BIOS mode set: ");
        serial_puts(g_vbe_mode_set_by_bios ? "yes" : "no");
        serial_puts("\n");

        // Allocate backbuffer (off-screen buffer for double buffering)
        backbuffer = (uint32_t*)kmalloc(width * height * 4);
        if (!backbuffer) {
            serial_puts("[GUI] Failed to allocate backbuffer\n");
            initialized = false;
            return;
        }
        // Clear backbuffer to black
        for (int i = 0; i < width * height; i++) backbuffer[i] = 0;

        initialized = true;
        serial_puts("[GUI] Framebuffer initialized with double-buffering\n");
    }

    void present() {
        // Copy backbuffer to LFB (frontbuffer), converting pixel format if needed.
        // The backbuffer stores colors as 0x00RRGGBB (R in bits 16-23) and is
        // row-major with stride = width pixels.  The LFB, however, has a row
        // PITCH that is frequently LARGER than width*bpp: real GOP hardware
        // (Intel Iris Xe, etc.) aligns each scanline to 16/32/256 bytes, so
        // PixelsPerScanLine != HorizontalResolution.  We MUST stride the LFB by
        // `pitch` per row, otherwise the whole framebuffer is written into the
        // first display row(s) and the screen collapses into a single garbled
        // strip (e.g. a white line at the top-left).  present_rect() already
        // honors pitch; this full-screen flip must too.
        if (pixel_format == PXF_RGBX32) {
            // RGBX32: swap R and B bytes
            for (int ry = 0; ry < height; ry++) {
                volatile uint8_t* dst8 = (volatile uint8_t*)lfb + (uint32_t)ry * pitch;
                uint8_t* src8 = (uint8_t*)backbuffer + (uint32_t)ry * width * 4u;
                for (int i = 0; i < width; i++) {
                    dst8[i*4]   = src8[i*4+2];  // R <- B (swap)
                    dst8[i*4+1] = src8[i*4+1];  // G
                    dst8[i*4+2] = src8[i*4];    // B <- R (swap)
                    dst8[i*4+3] = 0;            // X
                }
            }
        } else if (pixel_format == PXF_RGB24 || bpp == 24) {
            // 24-bit packed RGB (BGR in memory on x86)
            for (int ry = 0; ry < height; ry++) {
                volatile uint8_t* dst = (volatile uint8_t*)lfb + (uint32_t)ry * pitch;
                uint8_t* src = (uint8_t*)backbuffer + (uint32_t)ry * width * 4u;
                for (int i = 0; i < width; i++) {
                    dst[i*3]   = src[i*4];      // B
                    dst[i*3+1] = src[i*4+1];    // G
                    dst[i*3+2] = src[i*4+2];    // R
                }
            }
        } else if (pixel_format == PXF_RGB565 || bpp == 16) {
            // 16-bit RGB565
            for (int ry = 0; ry < height; ry++) {
                volatile uint16_t* dst = (volatile uint16_t*)((volatile uint8_t*)lfb + (uint32_t)ry * pitch);
                uint32_t* src = backbuffer + (uint32_t)ry * width;
                for (int i = 0; i < width; i++) {
                    uint32_t c = src[i];  // 0x00RRGGBB
                    uint16_t r = (c >> 19) & 0x1F;
                    uint16_t g = (c >> 10) & 0x3F;
                    uint16_t b = (c >> 3) & 0x1F;
                    dst[i] = (r << 11) | (g << 5) | b;
                }
            }
        } else {
            // BGRX32 (most common UEFI): direct copy is correct on x86 LE
            // Also used as fallback for unknown formats
            for (int ry = 0; ry < height; ry++) {
                volatile uint32_t* dst = (volatile uint32_t*)((volatile uint8_t*)lfb + (uint32_t)ry * pitch);
                uint32_t* src = backbuffer + (uint32_t)ry * width;
                for (int i = 0; i < width; i++) dst[i] = src[i];
            }
        }
    }

    // Partial blit of a sub-rectangle from backbuffer to LFB.
    // x,y,w,h are logical pixel coordinates. Honors the LFB row pitch
    // (which may exceed width*4 on real hardware) so it is safe to call
    // for small regions. Used by the clock tick and cursor moves to avoid
    // the tearing and CPU cost of a full-screen flip.
    void present_rect(int x, int y, int w, int h) {
        if (!initialized || !lfb || !backbuffer) return;
        // Clip to screen bounds
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width  - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;

        if (pixel_format == PXF_RGBX32) {
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                volatile uint8_t* dst = (volatile uint8_t*)lfb + (uint32_t)by * pitch + (uint32_t)x * 4u;
                uint8_t* src = (uint8_t*)(backbuffer + by * width + x);
                for (int i = 0; i < w; i++) {
                    dst[i*4]   = src[i*4+2];
                    dst[i*4+1] = src[i*4+1];
                    dst[i*4+2] = src[i*4];
                    dst[i*4+3] = 0;
                }
            }
        } else if (pixel_format == PXF_RGB24 || bpp == 24) {
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                volatile uint8_t* dst = (volatile uint8_t*)lfb + (uint32_t)by * pitch + (uint32_t)x * 3u;
                uint8_t* src = (uint8_t*)(backbuffer + by * width + x);
                for (int i = 0; i < w; i++) {
                    dst[i*3]   = src[i*4];
                    dst[i*3+1] = src[i*4+1];
                    dst[i*3+2] = src[i*4+2];
                }
            }
        } else if (pixel_format == PXF_RGB565 || bpp == 16) {
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                volatile uint16_t* dst = (volatile uint16_t*)((volatile uint8_t*)lfb + (uint32_t)by * pitch);
                uint32_t* src = backbuffer + by * width + x;
                for (int i = 0; i < w; i++) {
                    uint32_t c = src[i];
                    uint16_t r = (c >> 19) & 0x1F;
                    uint16_t g = (c >> 10) & 0x3F;
                    uint16_t b = (c >> 3)  & 0x1F;
                    dst[x + i] = (uint16_t)((r << 11) | (g << 5) | b);
                }
            }
        } else {
            // BGRX32 (most common) and fallback
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                volatile uint32_t* dst = (volatile uint32_t*)((volatile uint8_t*)lfb + (uint32_t)by * pitch);
                uint32_t* src = backbuffer + by * width + x;
                for (int i = 0; i < w; i++) dst[x + i] = src[i];
            }
        }
    }

    void enable_vbe_mode() {
        // If BGA ports available (QEMU/VirtualBox), set mode dynamically
        if (g_bga_available) {
            bochs_vbe_set_mode(width, height, bpp);
            serial_puts("[GUI] VBE mode set via BGA ports\n");
        } else if (g_vbe_mode_set_by_bios) {
            // Mode already set by BIOS INT 10h or UEFI GOP - nothing to do
            serial_puts("[GUI] VBE mode already set by BIOS/UEFI\n");
        } else {
            // No BGA and mode not set by BIOS - can't set mode in PM
            serial_puts("[GUI] WARNING: No way to set VBE mode (no BGA, no BIOS mode set)\n");
        }
    }
    void disable_vbe_mode() {
        // Only disable if we set it via BGA (can't switch back to text mode without INT 10h)
        if (g_bga_available) {
            bochs_vbe_disable();
            serial_puts("[GUI] VBE mode disabled (BGA)\n");
        } else {
            // Can't switch back to text mode without INT 10h (we're in PM)
            // Just clear the framebuffer instead
            serial_puts("[GUI] VBE mode stays active (no BGA to disable)\n");
        }
    }

    inline void put_pixel(int x, int y, Color c) {
        if ((uint32_t)x >= width || (uint32_t)y >= height) return;
        backbuffer[y * width + x] = c;
    }

    inline Color get_pixel(int x, int y) {
        if ((uint32_t)x >= width || (uint32_t)y >= height) return 0;
        return backbuffer[y * width + x];
    }

    void fill_rect(int x, int y, int w, int h, Color c) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;
        for (int row = 0; row < h; row++) {
            uint32_t* p = backbuffer + (y + row) * width + x;
            for (int col = 0; col < w; col++) p[col] = c;
        }
    }

    // Alpha-blend a solid colour over a rectangle (used for fade animations)
    void blend_rect(int x, int y, int w, int h, Color c, int alpha) {
        if (alpha <= 0) return;
        if (alpha > 255) alpha = 255;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;
        int cr = (int)((c >> 16) & 0xFF), cg = (int)((c >> 8) & 0xFF), cb = (int)(c & 0xFF);
        for (int row = 0; row < h; row++) {
            uint32_t* p = backbuffer + (y + row) * width + x;
            for (int col = 0; col < w; col++) {
                uint32_t d = p[col];
                int dr = (int)((d >> 16) & 0xFF), dg = (int)((d >> 8) & 0xFF), db = (int)(d & 0xFF);
                int r = (dr * (255 - alpha) + cr * alpha) / 255;
                int g = (dg * (255 - alpha) + cg * alpha) / 255;
                int b = (db * (255 - alpha) + cb * alpha) / 255;
                p[col] = (uint32_t)((r << 16) | (g << 8) | b);
            }
        }
    }

    void draw_rect(int x, int y, int w, int h, Color c) {        for (int i = x; i < x + w; i++) { put_pixel(i, y, c); put_pixel(i, y + h - 1, c); }
        for (int i = y; i < y + h; i++) { put_pixel(x, i, c); put_pixel(x + w - 1, i, c); }
    }

    void draw_line(int x0, int y0, int x1, int y1, Color c) {
        int dx = x1 - x0; if (dx < 0) dx = -dx;
        int dy = y1 - y0; if (dy < 0) dy = -dy;
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;
        for (;;) {
            put_pixel(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
    }

    // Filled rounded rectangle (Win11 style corners)
    void fill_rounded_rect(int x, int y, int w, int h, int r, Color c) {
        if (r > h / 2) r = h / 2;
        if (r > w / 2) r = w / 2;
        if (r < 0) r = 0;
        // Center rectangle
        fill_rect(x + r, y, w - 2 * r, h, c);
        // Top and bottom strips
        fill_rect(x, y + r, r, h - 2 * r, c);
        fill_rect(x + w - r, y + r, r, h - 2 * r, c);
        // Corners (approximate with quarter circles)
        for (int dy = 0; dy < r; dy++) {
            int dx = (int)(r * 0.7 - dy * 0.3);
            if (dx < 0) dx = 0;
            // Top-left
            for (int i = 0; i <= dx; i++) put_pixel(x + r - 1 - i, y + r - 1 - dy, c);
            // Top-right
            for (int i = 0; i <= dx; i++) put_pixel(x + w - r + i, y + r - 1 - dy, c);
            // Bottom-left
            for (int i = 0; i <= dx; i++) put_pixel(x + r - 1 - i, y + h - r + dy, c);
            // Bottom-right
            for (int i = 0; i <= dx; i++) put_pixel(x + w - r + i, y + h - r + dy, c);
        }
    }

    // Draw rounded rectangle outline
    void draw_rounded_rect(int x, int y, int w, int h, int r, Color c) {
        if (r > h / 2) r = h / 2;
        if (r > w / 2) r = w / 2;
        if (r < 0) r = 0;
        // Top and bottom edges
        draw_line(x + r, y, x + w - r - 1, y, c);
        draw_line(x + r, y + h - 1, x + w - r - 1, y + h - 1, c);
        // Left and right edges
        draw_line(x, y + r, x, y + h - r - 1, c);
        draw_line(x + w - 1, y + r, x + w - 1, y + h - r - 1, c);
        // Corner arcs
        for (int dy = 0; dy < r; dy++) {
            int dx = (int)(r * 0.7 - dy * 0.3);
            if (dx < 0) dx = 0;
            put_pixel(x + r - 1 - dx, y + r - 1 - dy, c);
            put_pixel(x + w - r + dx, y + r - 1 - dy, c);
            put_pixel(x + r - 1 - dx, y + h - r + dy, c);
            put_pixel(x + w - r + dx, y + h - r + dy, c);
        }
    }

    void draw_char(int x, int y, char ch, Color fg, Color bg) {
        const uint8_t* glyph = font8x16[(uint8_t)ch];
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                Color c = (bits & (0x80 >> col)) ? fg : bg;
                put_pixel(x + col, y + row, c);
            }
        }
    }

    void draw_text(int x, int y, const char* s, Color fg, Color bg) {
        int cx = x;
        while (*s) {
            if (*s == '\n') { cx = x; y += 16; }
            else { draw_char(cx, y, *s, fg, bg); cx += 8; }
            s++;
        }
    }

    void draw_text_transparent(int x, int y, const char* s, Color fg) {
        int cx = x;
        while (*s) {
            if (*s == '\n') { cx = x; y += 16; }
            else {
                const uint8_t* glyph = font8x16[(uint8_t)*s];
                for (int row = 0; row < 16; row++) {
                    uint8_t bits = glyph[row];
                    for (int col = 0; col < 8; col++) {
                        if (bits & (0x80 >> col))
                            put_pixel(cx + col, y + row, fg);
                    }
                }
                cx += 8;
            }
            s++;
        }
    }

    // ================= CJK (GB2312 16x16) rendering =================
    static int zfont_find_unicode(uint32_t cp) {
        int lo = 0, hi = (int)zfont_count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (zfont_unicode[mid] == cp) return mid;
            if (zfont_unicode[mid] < cp) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }

    // Draw one 16x16 CJK glyph by Unicode codepoint (with background).
    void draw_cjk(int x, int y, uint32_t cp, Color fg, Color bg) {
        int idx = zfont_find_unicode(cp);
        if (idx < 0) {                      // glyph not in embedded set: box
            fill_rect(x, y, 16, 16, bg);
            fill_rect(x, y, 15, 1, fg);
            fill_rect(x, y + 15, 16, 1, fg);
            fill_rect(x, y, 1, 16, fg);
            fill_rect(x + 15, y, 1, 16, fg);
            return;
        }
        const uint8_t* g = zfont_glyphs + idx * 32;
        for (int row = 0; row < 16; row++) {
            for (int half = 0; half < 2; half++) {
                uint8_t bits = g[row * 2 + half];
                for (int b = 0; b < 8; b++) {
                    int col = half * 8 + b;
                    Color c = (bits & (0x80 >> b)) ? fg : bg;
                    put_pixel(x + col, y + row, c);
                }
            }
        }
    }

    // Draw CJK glyph transparent (only fg pixels).
    void draw_cjk_transparent(int x, int y, uint32_t cp, Color fg) {
        int idx = zfont_find_unicode(cp);
        if (idx < 0) return;
        const uint8_t* g = zfont_glyphs + idx * 32;
        for (int row = 0; row < 16; row++) {
            for (int half = 0; half < 2; half++) {
                uint8_t bits = g[row * 2 + half];
                for (int b = 0; b < 8; b++) {
                    if (bits & (0x80 >> b))
                        put_pixel(x + half * 8 + b, y + row, fg);
                }
            }
        }
    }

    // Draw a mixed UTF-8 string (ASCII 8x16 + CJK 16x16) with background.
    void draw_text_utf8(int x, int y, const char* s, Color fg, Color bg) {
        int cx = x;
        while (*s) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) {
                if (c == '\n') { cx = x; y += 16; }
                else { draw_char(cx, y, (char)c, fg, bg); cx += 8; }
                s++;
            } else if ((c & 0xF0) == 0xE0 &&
                       (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
                uint32_t cp = ((c & 0x0F) << 12) |
                              (((unsigned char)s[1] & 0x3F) << 6) |
                              ((unsigned char)s[2] & 0x3F);
                s += 3;
                draw_cjk(cx, y, cp, fg, bg);
                cx += 16;
            } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
                s += 2;                    // 2-byte UTF-8 (accent, etc.) - skip
            } else s++;
        }
    }

    // Same but transparent (CJK glyphs only draw fg pixels).
    void draw_text_utf8_transparent(int x, int y, const char* s, Color fg) {
        int cx = x;
        while (*s) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) {
                if (c == '\n') { cx = x; y += 16; }
                else {
                    const uint8_t* glyph = font8x16[(uint8_t)c];
                    for (int row = 0; row < 16; row++) {
                        uint8_t bits = glyph[row];
                        for (int col = 0; col < 8; col++)
                            if (bits & (0x80 >> col))
                                put_pixel(cx + col, y + row, fg);
                    }
                    cx += 8;
                }
                s++;
            } else if ((c & 0xF0) == 0xE0 &&
                       (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
                uint32_t cp = ((c & 0x0F) << 12) |
                              (((unsigned char)s[1] & 0x3F) << 6) |
                              ((unsigned char)s[2] & 0x3F);
                s += 3;
                draw_cjk_transparent(cx, y, cp, fg);
                cx += 16;
            } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
                s += 2;
            } else s++;
        }
    }

    void clear_screen(Color c) { fill_rect(0, 0, width, height, c); }

    // Force-zero the entire front-buffer (LFB). Used on GUI entry to wipe
    // any bootuefi/OVMF residue before the GUI draws over it.
    void force_clear_lfb(){
        if(!lfb || !width || !height || !pitch) return;
        uint8_t* fb = (uint8_t*)lfb;
        uint32_t bytes = (uint32_t)pitch * (uint32_t)height;
        for(uint32_t i = 0; i < bytes; i += 4){
            fb[i] = 0; fb[i+1] = 0; fb[i+2] = 0; fb[i+3] = 0;
        }
    }

    // Gradient fill (top to bottom)
    void fill_gradient(int x, int y, int w, int h, Color top, Color bot) {
        if (h <= 0) return;
        int tr = (int)((top >> 16) & 0xFF), tg = (int)((top >> 8) & 0xFF), tb = (int)(top & 0xFF);
        int br = (int)((bot >> 16) & 0xFF), bg = (int)((bot >> 8) & 0xFF), bb = (int)(bot & 0xFF);
        for (int row = 0; row < h; row++) {
            int r = tr + (br - tr) * row / h;
            int g = tg + (bg - tg) * row / h;
            int b = tb + (bb - tb) * row / h;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            Color c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            fill_rect(x, y + row, w, 1, c);
        }
    }

    // 16.16 fixed-point sample coordinate for the bilinear stretch, computed
    // with 32-bit math only.  The naive form ((idx*src << 16) + (src << 15))/dst
    // needs a 64-bit divide (__divdi3), which a freestanding 32-bit kernel does
    // not link against.  Split into quotient + remainder so every intermediate
    // stays inside int32 -- bit-exact with the 64-bit expression:
    //     N = idx*src*2 + src
    //     result = ((N/dst) << 15) + (((N%dst) << 15) / dst) - 0x8000
    static inline int bilinear_fp(int idx, int src, int dst) {
        int n = idx * src * 2 + src;
        int q = n / dst;
        int r = n % dst;
        return (q << 15) + ((r << 15) / dst) - 0x8000;
    }

    // Stretch-blit a SFS texture into the backbuffer.
    //   fmt 0 = RGB565 (bilinear), fmt 1 = ARGB32 (nearest + alpha blend).
    void draw_image(const TexRec& t, int x, int y, int w, int h) {
        if (!t.loaded || !t.data || w <= 0 || h <= 0) return;
        int sx = x, sy = y, sw = w, sh = h;
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > (int)width)  sw = width - sx;
        if (sy + sh > (int)height) sh = height - sy;
        if (sw <= 0 || sh <= 0) return;
        const int tw = t.w, th = t.h;
        if (tw <= 0 || th <= 0) return;

        if (t.fmt == 1) {
            const uint8_t* src = t.data;
            for (int row = 0; row < sh; row++) {
                int tyi = (row * th) / sh;
                if (tyi >= th) tyi = th - 1;
                uint32_t* p = backbuffer + (sy + row) * width + sx;
                for (int col = 0; col < sw; col++) {
                    int txi = (col * tw) / sw;
                    if (txi >= tw) txi = tw - 1;
                    const uint8_t* q = src + (tyi * tw + txi) * 4;
                    uint32_t px = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                                  ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
                    int a = (int)((px >> 24) & 0xFF);
                    if (a == 0) continue;
                    uint32_t d = p[col];
                    int dr = (int)((d >> 16) & 0xFF), dg = (int)((d >> 8) & 0xFF), db = (int)(d & 0xFF);
                    int sr = (int)((px >> 16) & 0xFF), sg = (int)((px >> 8) & 0xFF), sb = (int)(px & 0xFF);
                    if (a == 255) { p[col] = px & 0x00FFFFFF; continue; }
                    p[col] = ((uint32_t)((sr * a + dr * (255 - a)) / 255) << 16) |
                             ((uint32_t)((sg * a + dg * (255 - a)) / 255) << 8) |
                             ((uint32_t)((sb * a + db * (255 - a)) / 255));
                }
            }
            return;
        }

        const uint16_t* src = (const uint16_t*)t.data;
        for (int row = 0; row < sh; row++) {
            int fy = bilinear_fp(row, th, sh);
            int y0 = fy >> 16; if (y0 < 0) y0 = 0;
            int y1 = y0 + 1; if (y1 >= th) y1 = th - 1;
            int yf = fy & 0xFFFF;
            const uint16_t* r0 = src + (uint32_t)y0 * tw;
            const uint16_t* r1 = src + (uint32_t)y1 * tw;
            uint32_t* p = backbuffer + (sy + row) * width + sx;
            for (int col = 0; col < sw; col++) {
                int fx = bilinear_fp(col, tw, sw);
                int x0 = fx >> 16; if (x0 < 0) x0 = 0;
                int x1 = x0 + 1; if (x1 >= tw) x1 = tw - 1;
                int xf = fx & 0xFFFF;
                uint16_t c00 = r0[x0], c01 = r0[x1], c10 = r1[x0], c11 = r1[x1];
                int r = tex_bilerp(tex_channel(c00, 0), tex_channel(c01, 0),
                                   tex_channel(c10, 0), tex_channel(c11, 0), xf, yf);
                int g = tex_bilerp(tex_channel(c00, 1), tex_channel(c01, 1),
                                   tex_channel(c10, 1), tex_channel(c11, 1), xf, yf);
                int b = tex_bilerp(tex_channel(c00, 2), tex_channel(c01, 2),
                                   tex_channel(c10, 2), tex_channel(c11, 2), xf, yf);
                p[col] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    // Draw a simple icon (colored square with letter)
    void draw_icon(int x, int y, int sz, Color bg, char letter, Color lc) {
        fill_rounded_rect(x, y, sz, sz, 4, bg);
        // Draw letter centered
        int lx = x + (sz - FONT_W) / 2;
        int ly = y + (sz - FONT_H) / 2;
        draw_char(lx, ly, letter, lc, bg);
    }

    // Draw a progress bar
    void draw_progress(int x, int y, int w, int h, int pct, Color fill) {
        fill_rounded_rect(x, y, w, h, h/2, C_PROGRESS_BG);
        int fw = w * pct / 100;
        if (fw > 0) fill_rounded_rect(x, y, fw, h, h/2, fill);
    }

    // ---- New helper methods for UI refactoring ----

    // Fill a circle (for circular icons) - no math.h, uses integer math
    void fill_circle(int cx, int cy, int r, Color c) {
        if (r <= 0) return;
        int r2 = r * r;
        for (int dy = -r; dy <= r; dy++) {
            int rem = r2 - dy * dy;
            if (rem < 0) continue;
            // Integer sqrt: find largest dx where dx*dx <= rem
            int dx = 0;
            int s = 1;
            while (s * s <= rem) { dx = s; s++; }
            for (int x = cx - dx; x <= cx + dx; x++) {
                put_pixel(x, cy + dy, c);
            }
        }
    }

    // Draw circle outline - uses midpoint circle algorithm (no math.h)
    void draw_circle(int cx, int cy, int r, Color c) {
        if (r <= 0) return;
        int x = r, y = 0;
        int err = 0;
        while (x >= y) {
            put_pixel(cx + x, cy + y, c);
            put_pixel(cx + y, cy + x, c);
            put_pixel(cx - y, cy + x, c);
            put_pixel(cx - x, cy + y, c);
            put_pixel(cx - x, cy - y, c);
            put_pixel(cx - y, cy - x, c);
            put_pixel(cx + y, cy - x, c);
            put_pixel(cx + x, cy - y, c);
            if (err <= 0) { y++; err += 2*y + 1; }
            if (err > 0) { x--; err -= 2*x + 1; }
        }
    }

    // Draw text centered in a region
    void draw_text_centered(int x, int y, int w, const char* s, Color fg) {
        int tw = strlen_(s) * FONT_W;
        int sx = x + (w - tw) / 2;
        if (sx < x) sx = x;
        draw_text_transparent(sx, y, s, fg);
    }

    // Draw text centered with background
    void draw_text_centered_bg(int x, int y, int w, const char* s, Color fg, Color bg) {
        int tw = strlen_(s) * FONT_W;
        int sx = x + (w - tw) / 2;
        if (sx < x) sx = x;
        draw_text(sx, y, s, fg, bg);
    }

    // Draw a card with shadow and rounded corners
    void draw_card(int x, int y, int w, int h, Color bg) {
        // Shadow
        for (int i = 0; i < 3; i++) {
            fill_rect(x + 2 + i, y + 2 + i, w, h, 0x10000000);
        }
        // Card background
        fill_rounded_rect(x, y, w, h, 6, bg);
    }

    // Draw a circular category icon with letter
    void draw_category_icon(int cx, int cy, int r, Color bg, char letter) {
        fill_circle(cx, cy, r, bg);
        // Draw letter centered
        int lx = cx - FONT_W / 2;
        int ly = cy - FONT_H / 2;
        draw_char(lx, ly, letter, COLOR_WHITE, bg);
    }

    // Draw a search bar (input field with magnifying glass icon)
    void draw_search_bar(int x, int y, int w, int h, const char* placeholder,
                         const char* text, int text_len, bool focused) {
        Color border = focused ? C_ACCENT : C_TM_SEARCH_BORDER;
        fill_rounded_rect(x, y, w, h, h/2, C_TM_SEARCH_BG);
        draw_rounded_rect(x, y, w, h, h/2, border);
        // Magnifying glass icon (simple circle + handle)
        int ix = x + 10, iy = y + h/2;
        draw_circle(ix, iy, 4, C_WIN_TEXT_SEC);
        draw_line(ix + 3, iy + 3, ix + 7, iy + 7, C_WIN_TEXT_SEC);
        // Text or placeholder
        int tx = x + 22;
        if (text_len > 0) {
            char tmp[128];
            int copy_len = text_len < 127 ? text_len : 127;
            memcpy_(tmp, text, copy_len);
            tmp[copy_len] = 0;
            draw_text_transparent(tx, y + (h - FONT_H) / 2, tmp, C_WIN_TEXT);
        } else if (placeholder) {
            draw_text_transparent(tx, y + (h - FONT_H) / 2, placeholder, C_WIN_TEXT_SEC);
        }
        // Cursor if focused
        if (focused) {
            int cx = tx + text_len * FONT_W;
            fill_rect(cx, y + 4, 1, h - 8, C_ACCENT);
        }
    }

    // Draw a table header row
    void draw_table_header(int x, int y, int w, int h, const char* headers[],
                           int col_widths[], int ncols) {
        fill_rect(x, y, w, h, C_TM_HEADER_BG);
        draw_line(x, y + h - 1, x + w, y + h - 1, C_WIN_BORDER);
        int cx = x + 4;
        for (int i = 0; i < ncols; i++) {
            draw_text_transparent(cx, y + (h - FONT_H) / 2, headers[i], C_TM_HEADER_TEXT);
            cx += col_widths[i];
        }
    }

    // Draw a shortcut tile (icon circle + label below)
    void draw_shortcut_tile(int x, int y, int w, int h, Color icon_color,
                            char letter, const char* label, bool hover) {
        Color bg = hover ? C_CARD_HOVER : C_CARD_BG;
        fill_rounded_rect(x, y, w, h, 8, bg);
        if (hover) draw_rounded_rect(x, y, w, h, 8, C_CARD_BORDER);
        // Icon circle
        int icon_r = 16;
        int icx = x + w / 2;
        int icy = y + 18;
        fill_circle(icx, icy, icon_r, icon_color);
        // Letter
        int lx = icx - FONT_W / 2;
        int ly = icy - FONT_H / 2;
        draw_char(lx, ly, letter, COLOR_WHITE, icon_color);
        // Label
        draw_text_centered(x, y + 38, w, label, C_SHORTCUT_TEXT);
    }

    // Draw a navigation tab
    void draw_nav_tab(int x, int y, int w, int h, const char* label, bool selected) {
        Color bg = selected ? C_PORTAL_TAB_SEL : C_PORTAL_TAB_BG;
        Color fg = selected ? C_PORTAL_TAB_TEXT_SEL : C_PORTAL_TAB_TEXT;
        fill_rounded_rect(x, y, w, h, 6, bg);
        draw_text_centered_bg(x, y + (h - FONT_H) / 2, w, label, fg, bg);
    }
};

// =====================================================================
//  Mouse cursor
// =====================================================================
struct MouseCursor {
    static constexpr int CURSOR_SIZE = 32;
    int x, y;
    bool visible;
    uint32_t saved[CURSOR_SIZE * CURSOR_SIZE];
    bool saved_valid;

    // Win11 aero_arrow cursor: 32x32, white fill + black outline, hotspot at (0,0)
    // Each bit = 1 pixel; MSB = column 0
    static constexpr uint32_t cursor_outline[32] = {
        0x80000000, 0xC0000000, 0xA0000000, 0x90000000,
        0x88000000, 0x84000000, 0x82000000, 0x81000000,
        0x80800000, 0x80400000, 0x80200000, 0x80100000,
        0x81F00000, 0x89000000, 0x94800000, 0xA4800000,
        0xC2400000, 0x02400000, 0x01800000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    };
    static constexpr uint32_t cursor_fill[32] = {
        0x00000000, 0x00000000, 0x40000000, 0x60000000,
        0x70000000, 0x78000000, 0x7C000000, 0x7E000000,
        0x7F000000, 0x7F800000, 0x7FC00000, 0x7FE00000,
        0x7E000000, 0x76000000, 0x63000000, 0x43000000,
        0x01800000, 0x01800000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000,
    };

    void init(int w, int h) { x = w / 2; y = h / 2; visible = false; saved_valid = false; }

    void save_bg(Graphics& g) {
        if (!saved_valid) {
            for (int dy = 0; dy < CURSOR_SIZE; dy++)
                for (int dx = 0; dx < CURSOR_SIZE; dx++)
                    saved[dy * CURSOR_SIZE + dx] = g.get_pixel(x + dx, y + dy);
            saved_valid = true;
        }
    }
    void restore_bg(Graphics& g) {
        if (saved_valid) {
            for (int dy = 0; dy < CURSOR_SIZE; dy++)
                for (int dx = 0; dx < CURSOR_SIZE; dx++)
                    g.put_pixel(x + dx, y + dy, saved[dy * CURSOR_SIZE + dx]);
            saved_valid = false;
        }
    }
    void draw(Graphics& g) {
        uint32_t mask = 0x80000000;
        for (int dy = 0; dy < CURSOR_SIZE; dy++) {
            uint32_t ob = cursor_outline[dy];
            uint32_t fb = cursor_fill[dy];
            if (ob == 0 && fb == 0) continue;
            for (int dx = 0; dx < CURSOR_SIZE; dx++) {
                if (ob & (mask >> dx))
                    g.put_pixel(x + dx, y + dy, COLOR_BLACK);
                else if (fb & (mask >> dx))
                    g.put_pixel(x + dx, y + dy, COLOR_WHITE);
            }
        }
    }
    void move(Graphics& g, int dx, int dy) {
        if (!visible) return;
        restore_bg(g);
        x += dx; y += dy;
        if (x < 0) x = 0; if (y < 0) y = 0;
        if (x >= g.width - CURSOR_SIZE) x = g.width - CURSOR_SIZE;
        if (y >= g.height - CURSOR_SIZE) y = g.height - CURSOR_SIZE;
        save_bg(g); draw(g);
    }
    void show(Graphics& g) { visible = true; save_bg(g); draw(g); }
    void hide(Graphics& g) { restore_bg(g); visible = false; }
};

// =====================================================================
//  Win11 Window Manager
// =====================================================================

// App types
enum AppType {
    APP_NONE = 0,
    APP_CONTROL_PANEL,
    APP_FILE_EXPLORER,
    APP_TASK_MANAGER,
    APP_TERMINAL,
    APP_CALCULATOR,
    APP_ABOUT,
    APP_MEM_OPTIMIZER,
    APP_BROWSER,
    APP_WIN32,          // native Win32 (PE32) application window
    APP_MANAGED,        // C#/.NET application hosted by MiniCLR
    APP_NOTEPAD,        // managed-only; no legacy native drawer
    APP_AISETUP,        // one-tap AI enablement wizard (managed)
    APP_AIAGENT,        // AI Agent runner (managed): Planner/Actor/Critic
};

constexpr int MAX_WINDOWS = 8;
constexpr int TITLE_BAR_H = 32;
constexpr int MAX_ICONS = 8;
constexpr int MAX_START_ITEMS = 8;

struct Win11Window {
    int x, y, w, h;
    char title[40];
    bool visible;
    bool active;
    AppType app;
    // App-specific state
    int scroll_offset;
    int selected_item;
    bool mem_optimized;
    uint32_t mem_before_kb;
    uint32_t mem_after_kb;
    int calc_display;       // calculator display value
    int calc_prev;          // previous operand
    int calc_op;            // 0=none,1=+,2=-,3=*,4=/
    bool calc_new_input;    // true = start new number
    char term_buf[1024];    // terminal output buffer
    int term_len;
    char term_input[128];
    int term_input_len;
    char sel_file[64];      // selected file name in file explorer
    int sel_file_idx;       // selected file index (-1 = none)
    int file_scroll;        // file list scroll offset
    // Browser state
    char browser_url[256];  // URL input buffer
    int browser_url_len;    // URL input length
    char browser_page[6144];// page content buffer
    int browser_page_len;   // page content length
    int browser_scroll;     // scroll offset for page content
    int browser_status;     // 0=idle,1=connecting,2=loading,3=done,-1=error
    bool browser_url_focused; // URL bar is focused for input

    // Undo buffers for text input fields (Ctrl+Z).  -1 == nothing to undo.
    char term_undo[128];
    int  term_undo_len = -1;
    char url_undo[256];
    int  url_undo_len  = -1;

    // Control Panel category view state
    int cp_category;        // -1 = category list, 0-7 = selected category
    // Task Manager search state
    char tm_search[64];     // search text
    int tm_search_len;      // search text length
    bool tm_search_focused; // search bar focused
    int tm_selected_proc;   // selected process index (-1 = none)
    // Window manager state
    bool minimized;         // hidden but kept in taskbar
    bool fullscreen;        // covers whole screen (floats over taskbar)
    bool floating;          // always-on-top
    int restore_x, restore_y, restore_w, restore_h; // pre-fullscreen rect
    // Portal desktop state
    int portal_tab;         // selected navigation tab (0=home,1=apps,2=system,3=tools)
    // Win32 application state
    int  w32_index;         // index into the win32 subsystem window table (-1 = none)
    char w32_file[48];      // source PE32 file name
    // Managed (C#) application state: which class inside shell.mex owns
    // this window.  All per-app state lives on the managed side.
    int  managed_app;       // NexOS.Forms app id (-1 = none)
    // Which application this window *is*, independent of whether it ended
    // up managed or native.  Lets the shell focus an already-open app
    // instead of stacking a second copy of it (Win11 taskbar behaviour).
    AppType launch_kind;

    // Animation state (Phase 2 visual polish)
    int anim_state;   // 0=none,1=opening,2=closing,3=minimizing,4=restoring
    int anim_p;       // progress 0..1000 (per-mille)

    bool contains(int px, int py) {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    // Title-bar button geometry: which 0=close 1=fullscreen 2=minimize 3=float
    void title_btn_rect(int which, int& bx, int& by) const {
        bx = x + w - 28 - which * 24;
        by = y + 4;
    }
    bool title_contains(int px, int py) {
        return px >= x && px < x + w && py >= y && py < y + TITLE_BAR_H;
    }
    // Content area (below title bar)
    int content_y() { return y + TITLE_BAR_H; }
    int content_h() { return h - TITLE_BAR_H; }
    bool content_contains(int px, int py) {
        return px >= x && px < x + w && py >= content_y() && py < y + h;
    }
};

struct DesktopIcon {
    int x, y;
    char label[24];
    AppType app;
    Color icon_color;
    char icon_letter;
    bool selected;
};

struct StartMenuItem {
    char label[24];
    AppType app;
    Color color;
    char letter;
};

    // ---- Web Browser ----
    // =====================================================================
    //  NexOS native browser engine
    //  (a lightweight HTML/CSS renderer + tab/history/bookmark manager)
    // =====================================================================
    namespace browser {

    constexpr int MAX_TABS  = 8;
    constexpr int PAGE_CAP  = 16384;
    constexpr int MAX_LINKS = 640;
    constexpr int MAX_BM    = 12;

    struct Tab {
        char url[256];
        char title[64];
        char page[PAGE_CAP];
        int  page_len;
        int  scroll;
        int  status;     // 0 idle,1 connecting,2 loading,3 done,-1 err
        bool loading;
        char back[16][256];
        int  back_n;
        char fwd[16][256];
        int  fwd_n;
    };

    struct Rect {
        int x, y, w, h;
        char href[256];
        void set(int a, int b, int c, int d){ x=a; y=b; w=c; h=d; href[0]=0; }
        bool has(int mx, int my) const { return mx>=x && mx<x+w && my>=y && my<y+h; }
    };

    static Tab       g_tabs[MAX_TABS];
    static int       g_ntabs = 0;
    static int       g_active = 0;
    static Rect      g_links[MAX_LINKS];
    static int       g_nlinks = 0;
    static struct { char name[28]; char url[160]; } g_bm[MAX_BM];
    static int       g_nbm = 0;
    static char      g_search[160] = "https://www.bing.com/search?q=";
    static Rect      g_tab_rects[MAX_TABS];
    static Rect      g_tab_close[MAX_TABS];
    static Rect      g_newtab_rect;
    static Rect      g_nav_rects[5];
    static Rect      g_bm_rects[MAX_BM];
    static Rect      g_scroll_rect;
    static int       g_scroll_max = 0;
    static bool      g_inited = false;

    static const char* HOME_HTML =
        "<!DOCTYPE html><html><head><title>NexOS - Bing</title>"
        "<style>"
        "body { color: #1a1a1a; }"
        ".b1 { color: #0067C0; font-weight: bold; }"
        ".b2 { color: #008373; font-weight: bold; }"
        ".b3 { color: #C50F1F; font-weight: bold; }"
        ".b4 { color: #FFB900; font-weight: bold; }"
        ".b5 { color: #0067C0; font-weight: bold; }"
        ".searchbox { background: #E8F4FF; }"
        ".card { background: #f0f0f0; }"
        ".cardtitle { color: #0067C0; font-weight: bold; }"
        ".lnk { color: #0067C0; text-decoration: underline; }"
        ".muted { color: #666666; }"
        "</style></head><body>"
        "<h1><span class='b1'>B</span><span class='b2'>i</span><span class='b3'>n</span>"
        "<span class='b4'>g</span></h1>"
        "<div class='searchbox'>Search the web with NexOS Browser &mdash; type a keyword in the address bar above.</div>"
        "<h2 class='cardtitle'>Quick Links</h2>"
        "<div class='card'>"
        "<ul>"
        "<li><a class='lnk' href='http://10.0.2.15:8080/'>NexOS local Web server (NexOS AI)</a></li>"
        "<li><a class='lnk' href='https://www.bing.com/search?q=NexOS'>Bing Search</a></li>"
        "<li><a class='lnk' href='https://duckduckgo.com/html/'>DuckDuckGo Search</a></li>"
        "</ul>"
        "</div>"
        "<h2 class='cardtitle'>Tips</h2>"
        "<p class='muted'>Type a URL to visit it directly, or a keyword to search with the "
        "default engine <b>Bing</b>. Use the tab bar for multiple pages and the bookmark bar for favorites.</p>"
        "<hr>"
        "<p class='muted'><b>Note:</b> only plain HTTP is supported today; HTTPS/TLS is not yet "
        "implemented. Visit the kernel's built-in server at "
        "<a class='lnk' href='http://10.0.2.15:8080/'>http://10.0.2.15:8080/</a> to see a real page.</p>"
        "</body></html>";

    static bool eqi(const char* a, const char* b){
        while (*a && *b){ char ca=*a, cb=*b;
            if (ca>='A'&&ca<='Z') ca+=32; if (cb>='A'&&cb<='Z') cb+=32;
            if (ca!=cb) return false; a++; b++; }
        return *a==0 && *b==0;
    }
    static void cpy(char* d, const char* s, int n){ int i=0; while(s[i] && i<n-1){ d[i]=s[i]; i++; } d[i]=0; }
    static bool is_about(const char* u){ return u[0]=='a'&&u[1]=='b'&&u[2]=='o'&&u[3]=='u'&&u[4]=='t'&&u[5]==':'; }

    static void set_about(Tab& t){
        const char* h = eqi(t.url, "about:blank") ? "<html><body><h1>New Tab</h1></body></html>" : HOME_HTML;
        int l = (int)strlen_(h); if (l > PAGE_CAP-1) l = PAGE_CAP-1;
        memcpy_(t.page, h, l); t.page[l]=0; t.page_len=l;
        t.loading=false; t.status=3; cpy(t.title, eqi(t.url,"about:blank")?"New Tab":"NexOS - Bing", 64);
    }

    static void extract_title(Tab& t){
        if (t.title[0]) return;
        // derive a simple title from the host
        const char* u = t.url;
        if (is_about(u)) { cpy(t.title, "NexOS - New Tab", 64); return; }
        const char* s = u; while (*s && *s!='/') s++;
        int n = (int)(s - u); if (n>56) n=56;
        int i=0; t.title[i++]='M'; t.title[i++]='i'; t.title[i++]='n'; t.title[i++]='i'; t.title[i++]='O'; t.title[i++]='S'; t.title[i++]=' '; t.title[i++]='-'; t.title[i++]=' ';
        for (int k=0;k<n && i<63;k++) t.title[i++]=u[k];
        t.title[i]=0;
    }

    static void normalize(const char* raw, char out[256]){
        // trim leading spaces
        while (*raw==' ') raw++;
        const char* p = raw;
        bool has_scheme=false, has_dot=false, has_space=false;
        for (const char* q=raw; *q; q++){ if (*q==':'&&q[1]=='/'&&q[2]=='/') has_scheme=true; if (*q=='.') has_dot=true; if (*q==' ') has_space=true; }
        if (is_about(raw)) { cpy(out, raw, 256); return; }
        if (has_scheme) { cpy(out, raw, 256); return; }
        if (has_dot && !has_space) { out[0]='h';out[1]='t';out[2]='t';out[3]='p';out[4]=':';out[5]='/';out[6]='/'; cpy(out+7, raw, 249); return; }
        // search query
        int pos = 0;
        for (int k=0; g_search[k] && pos<150; k++) out[pos++]=g_search[k];
        for (const char* q=raw; *q && pos<250; q++){
            char c=*q;
            if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out[pos++]=c;
            else if (c==' ') out[pos++]='+';
            else { out[pos++]='%'; static const char* hx="0123456789ABCDEF"; out[pos++]=hx[(c>>4)&0xF]; out[pos++]=hx[c&0xF]; }
        }
        out[pos]=0;
    }

    static void reload(int ti);
    static void navigate(int ti, const char* raw){
        if (ti<0||ti>=g_ntabs) return;
        Tab& t = g_tabs[ti];
        char url[256]; normalize(raw, url);
        if (t.url[0] && t.back_n<16) cpy(t.back[t.back_n++], t.url, 256);
        t.fwd_n = 0;
        cpy(t.url, url, 256);
        t.title[0]=0; t.scroll=0;
        if (is_about(url)) { set_about(t); return; }
        t.page_len=0; t.status=1; t.loading=true;
        if (g_cb.browser_navigate) g_cb.browser_navigate(url);
    }
    static void reload(int ti){
        if (ti<0||ti>=g_ntabs) return;
        Tab& t = g_tabs[ti];
        if (!t.url[0]) return;
        t.scroll=0;
        if (is_about(t.url)) { set_about(t); return; }
        t.page_len=0; t.status=1; t.loading=true;
        if (g_cb.browser_navigate) g_cb.browser_navigate(t.url);
    }
    static void go_back(int ti){
        if (ti<0||ti>=g_ntabs) return; Tab& t=g_tabs[ti];
        if (t.back_n==0) return;
        if (t.fwd_n<16) cpy(t.fwd[t.fwd_n++], t.url, 256);
        cpy(t.url, t.back[--t.back_n], 256); t.back_n = t.back_n<0?0:t.back_n; reload(ti);
    }
    static void go_forward(int ti){
        if (ti<0||ti>=g_ntabs) return; Tab& t=g_tabs[ti];
        if (t.fwd_n==0) return;
        if (t.back_n<16) cpy(t.back[t.back_n++], t.url, 256);
        cpy(t.url, t.fwd[--t.fwd_n], 256); t.fwd_n = t.fwd_n<0?0:t.fwd_n; reload(ti);
    }
    static void stop(int ti){
        if (ti<0||ti>=g_ntabs) return; Tab& t=g_tabs[ti];
        t.loading=false; t.status=0; if (g_cb.browser_reset) g_cb.browser_reset();
    }
    static void home(int ti){ navigate(ti, "https://www.bing.com/"); }

    static void new_tab(const char* raw){
        if (g_ntabs >= MAX_TABS) g_ntabs = MAX_TABS-1;
        int ti = g_ntabs++;
        Tab& t = g_tabs[ti];
        t.url[0]=0; t.title[0]=0; t.page[0]=0; t.page_len=0; t.scroll=0; t.status=0; t.loading=false;
        t.back_n=0; t.fwd_n=0;
        g_active = ti;
        if (raw && raw[0]) navigate(ti, raw);
        else navigate(ti, "https://www.bing.com/");
    }
    static void close_tab(int ti){
        if (ti<0||ti>=g_ntabs) return;
        for (int i=ti; i<g_ntabs-1; i++) g_tabs[i] = g_tabs[i+1];
        g_ntabs--;
        if (g_ntabs==0) { new_tab("about:home"); return; }
        if (g_active >= g_ntabs) g_active = g_ntabs-1;
        if (g_active < 0) g_active = 0;
    }

    static void ensure_init(){
        if (g_inited) return;
        g_inited = true;
        // default bookmarks
        const char* defs[][2] = {
            {"New Tab", "about:home"},
            {"NexOS AI", "http://10.0.2.15:8080/"},
            {"Bing", "https://www.bing.com/search?q=NexOS"},
            {"DuckDuckGo", "https://duckduckgo.com/html/"},
        };
        for (int i=0;i<4 && g_nbm<MAX_BM;i++){ cpy(g_bm[g_nbm].name, defs[i][0], 28); cpy(g_bm[g_nbm].url, defs[i][1], 160); g_nbm++; }
        new_tab("about:home");
    }

    // ---- Minimal CSS engine (C-1): styling for the HTML renderer ----
    struct CssRule {
        char sel[28];
        uint32_t color, bg, border_color;
        bool bold, underline;
        bool has_color, has_bg, has_bold, has_underline;
        int border_w, pad_l, pad_r, pad_t, pad_b, radius;
        int align;   // 0 left, 1 center, 2 right
        int display; // 0 block, 1 inline, 2 inline-block
        int width;   // 0 = auto (fill parent)
        int ml, mr, mt, mb;
        bool ml_auto, mr_auto;
        bool bg_fill; // paint box background
    };
    static CssRule g_css[40];
    static int     g_ncss = 0;

    struct StyleState {
        uint32_t color, bg, border_color;
        bool bold, underline;
        bool has_color, has_bg, has_bold, has_underline;
        int border_w, pad_l, pad_r, pad_t, pad_b, radius;
        int align;
        int display;
        int width;
        int ml, mr, mt, mb;
        bool ml_auto, mr_auto;
        bool bg_fill;
    };
    static StyleState g_ss[24];
    static int        g_sp = 0;

    static uint32_t css_color(const char* s){
        if (!s||!*s) return 0x1A1A1A;
        if (s[0]=='#'){
            int v=0,k=1;
            while (s[k]){ char d=s[k]; if(d>='0'&&d<='9')v=v*16+d-'0'; else if(d>='a'&&d<='f')v=v*16+d-'a'+10; else if(d>='A'&&d<='F')v=v*16+d-'A'+10; k++; }
            return (uint32_t)(0x00FFFFFF & (unsigned)v);
        }
        if (eqi(s,"red")) return 0x00FF0000;
        if (eqi(s,"blue")) return 0x000000FF;
        if (eqi(s,"green")) return 0x0000FF00;
        if (eqi(s,"black")) return 0x00000000;
        if (eqi(s,"white")) return 0x00FFFFFF;
        if (eqi(s,"gray")||eqi(s,"grey")) return 0x00808080;
        if (eqi(s,"orange")) return 0x00FFA500;
        if (eqi(s,"yellow")) return 0x00FFFF00;
        if (eqi(s,"purple")) return 0x00800080;
        if (eqi(s,"teal")) return 0x00808000;
        if (eqi(s,"navy")) return 0x00800000;
        return 0x1A1A1A;
    }
    // Parse a length list like "8px", "4px 8px", "1 2 3 4" (strips "px") into 4 sides.
    static void css_set_padding(int* pl,int* pr,int* pt,int* pb,const char* v){
        int a[4]; int n=0; const char* p=v;
        while(*p && n<4){
            while(*p && (*p==' '||*p=='p'||*p=='x'||*p=='\t')) p++;
            if(!*p) break;
            int x=0; while(*p>='0'&&*p<='9'){ x=x*10+(*p-'0'); p++; }
            a[n++]=x; while(*p && *p!=' ') p++;
        }
        if(n==1){*pl=*pr=*pt=*pb=a[0];}
        else if(n==2){*pt=*pb=a[0];*pl=*pr=a[1];}
        else if(n==3){*pt=a[0];*pl=*pr=a[1];*pb=a[2];}
        else if(n>=4){*pt=a[0];*pr=a[1];*pb=a[2];*pl=a[3];}
    }
    // Parse margin shorthand / detect "auto". Sets s.ml/mr/mt/mb and auto flags.
    static void css_set_margin(StyleState& s, const char* v){
        int a[4]; int n=0; const char* p=v; bool auto_=false;
        while(*p && n<4){
            while(*p && (*p==' '||*p=='p'||*p=='x'||*p=='\t')) p++;
            if(!*p) break;
            if(*p=='a'){ auto_=true; while(*p && *p!=' ') p++; continue; }
            int x=0; while(*p>='0'&&*p<='9'){ x=x*10+(*p-'0'); p++; }
            a[n++]=x; while(*p && *p!=' ') p++;
        }
        if(auto_){ s.ml_auto=true; s.mr_auto=true; return; }
        if(n==1){ s.ml=s.mr=s.mt=s.mb=a[0]; }
        else if(n==2){ s.mt=s.mb=a[0]; s.ml=s.mr=a[1]; }
        else if(n>=3){ s.mt=a[0]; s.mr=a[1]; s.mb=a[2]; s.ml=(n>=4)?a[3]:a[1]; }
    }
    static int css_int(const char* v){
        int x=0; const char* p=v;
        while(*p && (*p<'0'||*p>'9')) p++;
        while(*p>='0'&&*p<='9'){ x=x*10+(*p-'0'); p++; }
        return x;
    }
    static bool css_class_has(const char* cls, const char* name){
        if (!cls||!*cls||!name[0]) return false;
        int nl = (int)strlen_(name);
        const char* p=cls;
        while(*p){
            while(*p==' ') p++;
            const char* s=p; while(*p && *p!=' ') p++;
            int n=(int)(p-s);
            if (n==nl){
                bool eq=true; for(int i=0;i<n;i++){ char a=s[i],b=name[i]; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b){eq=false;break;} }
                if(eq) return true;
            }
        }
        return false;
    }
    static bool css_match(const char* sel, const char* tag, const char* cls){
        if (!sel||!sel[0]) return false;
        if (sel[0]=='.') return css_class_has(cls, sel+1);
        if (sel[0]=='#') return false;
        return eqi(sel, tag);
    }
    static void parse_css(const char* css){
        g_ncss=0;
        const char* p=css;
        while(*p && g_ncss<40){
            while(*p && *p!='{') p++;
            if(!*p) break;
            char sel[28]; int sl=0;
            const char* s2=p-1;
            while(s2>css && *s2!='}' && *s2!='\n' && *s2!='\r') s2--;
            if(s2>=css) s2++;
            while(s2<p && sl<27 && *s2!='{'){ char c=*s2; if(c!=' '&&c!='\t'&&c!='\n'&&c!='\r') sel[sl++]=c; s2++; }
            sel[sl]=0;
            p++;
            const char* be=p; while(*p && *p!='}') p++;
            if(!*p) break;
            CssRule r; cpy(r.sel, sel, 28); r.color=0x1A1A1A; r.bg=0xFFFFFF; r.border_color=0x888888; r.bold=false; r.underline=false;
            r.has_color=r.has_bg=r.has_bold=r.has_underline=false;
            r.border_w=0; r.pad_l=r.pad_r=r.pad_t=r.pad_b=0; r.radius=0;
            r.align=0; r.display=0; r.width=0; r.ml=r.mr=r.mt=r.mb=0; r.ml_auto=r.mr_auto=false; r.bg_fill=false;
            const char* d=be;
            while(d<p){
                while(d<p && (*d==';'||*d==' '||*d=='\t'||*d=='\n'||*d=='\r')) d++;
                if(d>=p) break;
                const char* ps=d; while(d<p && *d!=':') d++;
                char prop[24]; int pl=0; for(const char* a=ps;a<d&&pl<23;a++) prop[pl++]=*a; prop[pl]=0;
                if(d>=p) break; d++;
                const char* vs=d; while(d<p && *d!=';') d++;
                char val[64]; int vl=0; for(const char* a=vs;a<d&&vl<63;a++){ char c=*a; if(c==' '||c=='\t'||c=='\n'||c=='\r') continue; val[vl++]=c; } val[vl]=0;
                if(eqi(prop,"color")){ r.color=css_color(val); r.has_color=true; }
                else if(eqi(prop,"background")||eqi(prop,"background-color")){ r.bg=css_color(val); r.has_bg=true; r.bg_fill=true; }
                else if(eqi(prop,"font-weight")){ r.bold=eqi(val,"bold"); r.has_bold=true; }
                else if(eqi(prop,"text-decoration")){ r.underline=eqi(val,"underline"); r.has_underline=true; }
                else if(eqi(prop,"text-align")){ if(eqi(val,"center")) r.align=1; else if(eqi(val,"right")) r.align=2; else r.align=0; }
                else if(eqi(prop,"display")){ if(eqi(val,"inline-block")) r.display=2; else if(eqi(val,"inline")) r.display=1; else r.display=0; }
                else if(eqi(prop,"width")){ r.width=css_int(val); }
                else if(eqi(prop,"border-radius")){ r.radius=css_int(val); }
                else if(eqi(prop,"border")){
                    r.border_w=css_int(val);
                    const char* q=val; while(*q && (*q<'0'||*q>'9')) q++;
                    while(*q>='0'&&*q<='9') q++;
                    while(*q==' ') q++;
                    if(*q && *q!='s' && *q!='S'){ r.border_color=css_color(q); }
                }
                else if(eqi(prop,"border-width")){ r.border_w=css_int(val); }
                else if(eqi(prop,"border-color")){ r.border_color=css_color(val); }
                else if(eqi(prop,"padding")){ css_set_padding(&r.pad_l,&r.pad_r,&r.pad_t,&r.pad_b,val); }
                else if(eqi(prop,"padding-left")){ r.pad_l=css_int(val); }
                else if(eqi(prop,"padding-right")){ r.pad_r=css_int(val); }
                else if(eqi(prop,"padding-top")){ r.pad_t=css_int(val); }
                else if(eqi(prop,"padding-bottom")){ r.pad_b=css_int(val); }
                else if(eqi(prop,"margin")){
                    int a[4]; int n=0; const char* q=val; bool au=false;
                    while(*q && n<4){
                        while(*q && (*q==' '||*q=='p'||*q=='x'||*q=='\t')) q++;
                        if(!*q) break;
                        if(*q=='a'){ au=true; while(*q && *q!=' ') q++; continue; }
                        int x=0; while(*q>='0'&&*q<='9'){ x=x*10+(*q-'0'); q++; } a[n++]=x; while(*q && *q!=' ') q++;
                    }
                    if(au){ r.ml_auto=true; r.mr_auto=true; }
                    else if(n==1){ r.ml=r.mr=r.mt=r.mb=a[0]; }
                    else if(n==2){ r.mt=r.mb=a[0]; r.ml=r.mr=a[1]; }
                    else if(n>=3){ r.mt=a[0]; r.mr=a[1]; r.mb=a[2]; r.ml=(n>=4)?a[3]:a[1]; }
                }
                else if(eqi(prop,"margin-left")){ if(eqi(val,"auto")) r.ml_auto=true; else r.ml=css_int(val); }
                else if(eqi(prop,"margin-right")){ if(eqi(val,"auto")) r.mr_auto=true; else r.mr=css_int(val); }
                else if(eqi(prop,"margin-top")){ r.mt=css_int(val); }
                else if(eqi(prop,"margin-bottom")){ r.mb=css_int(val); }
            }
            g_css[g_ncss++]=r;
            p++;
        }
    }
    static void css_apply(const char* tag, const char* cls){
        StyleState& s = g_ss[g_sp];
        for (int i=0;i<g_ncss;i++){
            if (css_match(g_css[i].sel, tag, cls)){
                if (g_css[i].has_color){ s.color=g_css[i].color; s.has_color=true; }
                if (g_css[i].has_bg){ s.bg=g_css[i].bg; s.has_bg=true; s.bg_fill=true; }
                if (g_css[i].has_bold){ s.bold=g_css[i].bold; s.has_bold=true; }
                if (g_css[i].has_underline){ s.underline=g_css[i].underline; s.has_underline=true; }
                s.border_color=g_css[i].border_color; s.border_w=g_css[i].border_w;
                s.pad_l=g_css[i].pad_l; s.pad_r=g_css[i].pad_r; s.pad_t=g_css[i].pad_t; s.pad_b=g_css[i].pad_b;
                s.radius=g_css[i].radius; s.align=g_css[i].align; s.display=g_css[i].display; s.width=g_css[i].width;
                s.ml=g_css[i].ml; s.mr=g_css[i].mr; s.mt=g_css[i].mt; s.mb=g_css[i].mb;
                s.ml_auto=g_css[i].ml_auto; s.mr_auto=g_css[i].mr_auto;
            }
        }
    }
    static void css_apply_inline(const char* style){
        if (!style||!*style) return;
        StyleState& s = g_ss[g_sp];
        const char* d=style;
        while(*d){
            while(*d && (*d==';'||*d==' '||*d=='\t')) d++;
            if(!*d) break;
            const char* ps=d; while(*d && *d!=':') d++;
            char prop[24]; int pl=0; for(const char* a=ps;a<d&&pl<23;a++) prop[pl++]=*a; prop[pl]=0;
            if(*d!=':') break; d++;
            const char* vs=d; while(*d && *d!=';') d++;
            char val[64]; int vl=0; for(const char* a=vs;a<d&&vl<63;a++){ char c=*a; if(c==' '||c=='\t') continue; val[vl++]=c; } val[vl]=0;
            if(eqi(prop,"color")){ s.color=css_color(val); s.has_color=true; }
            else if(eqi(prop,"background")||eqi(prop,"background-color")){ s.bg=css_color(val); s.has_bg=true; s.bg_fill=true; }
            else if(eqi(prop,"font-weight")){ s.bold=eqi(val,"bold"); s.has_bold=true; }
            else if(eqi(prop,"text-decoration")){ s.underline=eqi(val,"underline"); s.has_underline=true; }
            else if(eqi(prop,"text-align")){ if(eqi(val,"center")) s.align=1; else if(eqi(val,"right")) s.align=2; else s.align=0; }
            else if(eqi(prop,"display")){ if(eqi(val,"inline-block")) s.display=2; else if(eqi(val,"inline")) s.display=1; else s.display=0; }
            else if(eqi(prop,"width")){ s.width=css_int(val); }
            else if(eqi(prop,"border-radius")){ s.radius=css_int(val); }
            else if(eqi(prop,"border")){ s.border_w=css_int(val); const char* q=val; while(*q && (*q<'0'||*q>'9')) q++; while(*q>='0'&&*q<='9') q++; while(*q==' ') q++; if(*q && *q!='s' && *q!='S') s.border_color=css_color(q); }
            else if(eqi(prop,"border-width")){ s.border_w=css_int(val); }
            else if(eqi(prop,"border-color")){ s.border_color=css_color(val); }
            else if(eqi(prop,"padding")){ css_set_padding(&s.pad_l,&s.pad_r,&s.pad_t,&s.pad_b,val); }
            else if(eqi(prop,"padding-left")){ s.pad_l=css_int(val); }
            else if(eqi(prop,"padding-right")){ s.pad_r=css_int(val); }
            else if(eqi(prop,"padding-top")){ s.pad_t=css_int(val); }
            else if(eqi(prop,"padding-bottom")){ s.pad_b=css_int(val); }
            else if(eqi(prop,"margin")){ css_set_margin(s, val); }
            else if(eqi(prop,"margin-left")){ if(eqi(val,"auto")) s.ml_auto=true; else s.ml=css_int(val); }
            else if(eqi(prop,"margin-right")){ if(eqi(val,"auto")) s.mr_auto=true; else s.mr=css_int(val); }
            else if(eqi(prop,"margin-top")){ s.mt=css_int(val); }
            else if(eqi(prop,"margin-bottom")){ s.mb=css_int(val); }
        }
    }
    static bool is_block(const char* t){
        if (t[0]=='h'&&t[1]>='1'&&t[1]<='6') return true;
        return eqi(t,"p")||eqi(t,"div")||eqi(t,"section")||eqi(t,"article")||eqi(t,"header")||eqi(t,"footer")||eqi(t,"aside")||eqi(t,"nav")||eqi(t,"main")||eqi(t,"blockquote")||eqi(t,"ul")||eqi(t,"ol")||eqi(t,"li")||eqi(t,"pre")||eqi(t,"table")||eqi(t,"tr")||eqi(t,"td")||eqi(t,"th");
    }

    // ---- HTML layout / render ----
    static void emit_text(Graphics& gfx, const char* s, int n, int& x, int& y,
                          int lx, int rx, int indent, bool bold, bool link,
                          const char* href, uint32_t color, int heading){
        char dec[640]; int dl=0;
        for (int i=0;i<n && dl<620;i++){
            char c=s[i];
            if (c=='&'){
                int j=i+1; char e[40]; int el=0;
                while (j<n && s[j]!=';' && el<39 && s[j]!=' ' && s[j]!='<') e[el++]=s[j++];
                if (j<n && s[j]==';'){ e[el]=0; i=j; } else { e[el]=0; i=j-1; }
                if (eqi(e,"amp")) dec[dl++]='&';
                else if (eqi(e,"lt")) dec[dl++]='<';
                else if (eqi(e,"gt")) dec[dl++]='>';
                else if (eqi(e,"quot")) dec[dl++]='"';
                else if (eqi(e,"nbsp")) dec[dl++]=' ';
                else if (eqi(e,"mdash")) { dec[dl++]='-'; dec[dl++]='-'; }
                else if (eqi(e,"ndash")) dec[dl++]='-';
                else if (eqi(e,"hellip")) { dec[dl++]='.'; dec[dl++]='.'; dec[dl++]='.'; }
                else if (eqi(e,"copy")) { dec[dl++]='('; dec[dl++]='c'; dec[dl++]=')'; }
                else if (eqi(e,"reg")) { dec[dl++]='('; dec[dl++]='R'; dec[dl++]=')'; }
                else if (eqi(e,"trade")) { dec[dl++]='('; dec[dl++]='T'; dec[dl++]='M'; dec[dl++]=')'; }
                else if (eqi(e,"apos")) dec[dl++]='\'';
                else if (e[0]=='#'){ int v=0; if(e[1]=='x'||e[1]=='X'){ for(int k=2;k<el;k++){char d=e[k]; if(d>='0'&&d<='9')v=v*16+d-'0'; else if(d>='a'&&d<='f')v=v*16+d-'a'+10; else if(d>='A'&&d<='F')v=v*16+d-'A'+10;} } else { for(int k=1;k<el;k++){ if(e[k]>='0'&&e[k]<='9') v=v*10+e[k]-'0'; } } dec[dl++]=(char)(v&0xFF); }
                else { dec[dl++]='&'; for(int k=0;k<el;k++) dec[dl++]=e[k]; dec[dl++]=';'; }
            } else if (c=='\n'||c=='\r'||c=='\t'){ dec[dl++]=' '; }
            else dec[dl++]=(c==0)?' ':c;
        }
        dec[dl]=0;
        int lh = heading ? (24 - heading*2) : 16;
        if (heading==0 && (bold || g_ss[g_sp].bold)) lh = 16;
        const StyleState& st = g_ss[g_sp];
        uint32_t base = st.has_color ? st.color : color;
        uint32_t col = link ? 0x0000C0 : ((st.bold||bold) ? 0x000000 : base);
        char* tok = dec;
        while (*tok){
            while (*tok==' ') tok++;
            if (!*tok) break;
            char* start = tok;
            while (*tok && *tok!=' ') tok++;
            char saved = *tok; *tok=0;
            int tw = (int)strlen_(start) * FONT_W;
            if (x + tw > rx && x > lx + indent + 4){ y += lh; x = lx + indent; }
            gfx.draw_text(x, y, start, col, COLOR_WHITE);
            if (link || st.underline){
                gfx.draw_line(x, y + lh - 2, x + tw - 1, y + lh - 2, col);
                if (link && g_nlinks < MAX_LINKS){
                    Rect& L = g_links[g_nlinks++];
                    L.set(x, y, tw, lh); cpy(L.href, href, 256);
                }
            }
            x += tw;
            *tok = saved;
            if (saved==' ') { x += FONT_W; tok++; }
        }
    }

    static void render(Graphics& gfx, int cx0, int cy0, int cw, int ch){
        g_nlinks = 0;
        if (g_ntabs==0) return;
        Tab& t = g_tabs[g_active];
        const char* html = t.page;
        int lx = cx0 + 8, rx = cx0 + cw - 12;
        int x = lx, y = cy0 - t.scroll;
        bool bold=false, in_link=false, pre=false, in_title=false, skip=false;
        char linkhref[256]; linkhref[0]=0;
        int indent=0; uint32_t color=0x1A1A1A; int heading=0;
        uint32_t hcolors[7] = {0, 0x0067C0, 0x0078D7, 0x0080C0, 0x1A1A1A, 0x1A1A1A, 0x1A1A1A};

        // ---- style stack init ----
        g_sp = 0;
        g_ss[0].color = 0x1A1A1A; g_ss[0].bg = 0xFFFFFF;
        g_ss[0].bold = false; g_ss[0].underline = false;
        g_ss[0].has_color = false; g_ss[0].has_bg = false;
        g_ss[0].has_bold = false; g_ss[0].has_underline = false;
        bool in_style = false; static char stylebuf[2048]; int stylelen = 0;
        int bg_stack[12]; int bg_sp = 0;

        const char* p = html;
        while (*p){
            if (*p == '<'){
                const char* te = p+1;
                while (*te && *te != '>') te++;
                if (*te != '>'){ // literal '<'
                    if (x+FONT_W>rx && x>lx+indent+4){ y+=16; x=lx+indent; }
                    gfx.draw_text(x, y, "<", color, COLOR_WHITE); x+=FONT_W; p++; continue;
                }
                // tag name (lowercased, no attrs)
                char tag[32]; int tl=0;
                const char* q=p+1;
                bool closing = false;
                if (q<te && *q=='/') { closing=true; q++; }
                while (q<te && tl<31 && *q && *q!=' ' && *q!='\t' && *q!='/' && *q!='>') tag[tl++]=*q++;
                tag[tl]=0;
                // closing?
                char* ct = tag;
                // attributes
                char a_href[256]={0}, a_color[32]={0}, a_alt[128]={0}, a_class[64]={0}, a_style[256]={0}, a_id[64]={0};
                const char* r = p+1;
                while (r < te){
                    while (r<te && (*r==' '||*r=='\t'||*r=='\n'||*r=='\r'||*r=='/')) r++;
                    if (r>=te) break;
                    const char* ns=r; while(r<te && *r!='=' && *r!=' ' && *r!='\t' && *r!='>') r++;
                    char nm[24]; int nl=0; for(const char* a=ns;a<r&&nl<23;a++) nm[nl++]=*a; nm[nl]=0;
                    while(r<te && *r==' ') r++;
                    if (r<te && *r=='='){ r++; while(r<te && *r==' ') r++;
                        char val[256]; int vl=0; char qc=0;
                        if (r<te && (*r=='"'||*r=='\'')){ qc=*r++; while(r<te && *r!=qc && vl<255) val[vl++]=*r++; if(r<te)r++; }
                        else { while(r<te && *r!=' ' && *r!='\t' && *r!='>' && vl<255) val[vl++]=*r++; }
                        val[vl]=0;
                        if (eqi(nm,"href")) cpy(a_href,val,256);
                        else if (eqi(nm,"color")) cpy(a_color,val,32);
                        else if (eqi(nm,"alt")) cpy(a_alt,val,128);
                        else if (eqi(nm,"class")) cpy(a_class,val,64);
                        else if (eqi(nm,"style")) cpy(a_style,val,256);
                        else if (eqi(nm,"id")) cpy(a_id,val,64);
                    }
                }
                bool void_el = eqi(ct,"br")||eqi(ct,"hr")||eqi(ct,"img")||eqi(ct,"meta")||eqi(ct,"link")||eqi(ct,"input");
                bool no_push = eqi(ct,"style")||eqi(ct,"script")||eqi(ct,"head")||eqi(ct,"!doctype");
                if (!closing){
                    if (eqi(ct,"style")){ in_style=true; stylelen=0; }
                    else if (eqi(ct,"script")||eqi(ct,"head")){ skip=true; }
                    else if (eqi(ct,"title")){ in_title=true; t.title[0]=0; }
                    else if (is_block(ct) && !void_el && g_ss[g_sp].has_bg && bg_sp<12) bg_stack[bg_sp++]=y;
                    if (!void_el && !no_push && g_sp<23){ g_sp++; g_ss[g_sp] = g_ss[g_sp-1]; css_apply(ct, a_class); css_apply_inline(a_style); }
                    if (eqi(ct,"br")) { y+=16; x=lx+indent; }
                    else if (eqi(ct,"p")||eqi(ct,"div")||eqi(ct,"section")||eqi(ct,"article")||eqi(ct,"blockquote")) { y+=16; x=lx+indent; }
                    else if (ct[0]=='h' && ct[1]>='1'&&ct[1]<='6') { heading = ct[1]-'0'; if(!g_ss[g_sp].has_color) color = hcolors[heading]; y+=16; x=lx+indent; }
                    else if (eqi(ct,"ul")) { indent+=16; y+=8; }
                    else if (eqi(ct,"ol")) { indent+=16; y+=8; }
                    else if (eqi(ct,"li")) { gfx.draw_text(lx+indent, y, "-", 0x666666, COLOR_WHITE); x=lx+indent+16; }
                    else if (eqi(ct,"a")) { in_link=true; cpy(linkhref,a_href,256); }
                    else if (eqi(ct,"b")||eqi(ct,"strong")) { bold=true; }
                    else if (eqi(ct,"hr")) { gfx.draw_line(lx, y+8, rx, y+8, 0x0067C0); y+=16; x=lx+indent; }
                    else if (eqi(ct,"img")) { int bw=80,bh=58; gfx.fill_rect(lx+indent, y, bw, bh, 0xE8E8E8); gfx.draw_rect(lx+indent, y, bw, bh, 0xBBBBBB); gfx.draw_text(lx+indent+4, y+4, a_alt[0]?a_alt:"[image]", 0x666666, 0xE8E8E8); gfx.draw_line(lx+indent+4, y+bh-6, lx+indent+bw-4, y+bh-6, 0x0067C0); y+=bh+8; x=lx+indent; }
                    else if (eqi(ct,"pre")) { pre=true; }
                    else if (eqi(ct,"font")) { if (a_color[0]=='#'){ int v=0; for(int k=1;a_color[k];k++){char d=a_color[k]; if(d>='0'&&d<='9')v=v*16+d-'0'; else if(d>='a'&&d<='f')v=v*16+d-'a'+10; else if(d>='A'&&d<='F')v=v*16+d-'A'+10;} color=0x00FFFFFF & v; } }
                } else {
                    if (!void_el && !no_push && g_sp>0){
                        if (is_block(ct) && bg_sp>0){ int sy=bg_stack[--bg_sp]; int hh=y-sy; if(hh>0) gfx.fill_rect(lx, sy, rx-lx, hh, g_ss[g_sp].bg); }
                        g_sp--;
                    }
                    if (eqi(ct,"b")||eqi(ct,"strong")) bold=false;
                    else if (eqi(ct,"a")) { in_link=false; linkhref[0]=0; }
                    else if (eqi(ct,"pre")) pre=false;
                    else if (ct[0]=='h' && ct[1]>='1'&&ct[1]<='6') { heading=0; if(!g_ss[g_sp].has_color) color=0x1A1A1A; }
                    else if (eqi(ct,"font")) color=0x1A1A1A;
                    else if (eqi(ct,"title")) in_title=false;
                    else if (eqi(ct,"style")){ in_style=false; stylebuf[stylelen]=0; parse_css(stylebuf); }
                    else if (eqi(ct,"script")||eqi(ct,"head")) { skip=false; }
                    else if (eqi(ct,"p")||eqi(ct,"div")||eqi(ct,"section")||eqi(ct,"article")||eqi(ct,"blockquote")||eqi(ct,"ul")||eqi(ct,"ol")||eqi(ct,"li")) { y+=16; x=lx+indent; }
                }
                p = (*te) ? te+1 : te;
                continue;
            }
            if (in_style){ while (*p && *p!='<'){ if(stylelen<2047) stylebuf[stylelen++]=*p; p++; } continue; }
            if (skip){ while (*p && *p!='<') p++; continue; }
            const char* run = p;
            while (*p && *p!='<') p++;
            int runlen = (int)(p - run);
            if (runlen>0){
                if (in_title){
                    int tl=(int)strlen_(t.title);
                    for (int k=0;k<runlen && tl<63;k++){ char c=run[k]; if(c=='\n'||c=='\r') continue; t.title[tl++]=c; }
                    t.title[tl]=0;
                } else {
                    emit_text(gfx, run, runlen, x, y, lx, rx, indent, bold, in_link, linkhref, color, heading);
                }
            }
        }
    }

    static void poll(){
        if (g_ntabs==0) return;
        Tab& t = g_tabs[g_active];
        if (!t.loading) return;
        int st = g_cb.browser_status ? g_cb.browser_status() : 0;
        if (st==3){
            int n = g_cb.browser_get_page ? g_cb.browser_get_page(t.page, PAGE_CAP-1) : 0;
            if (n<0) n=0; t.page[n]=0; t.page_len=n; t.loading=false; t.status=3; extract_title(t);
        } else if (st==-1){
            const char* e="Error: could not load the page.\nThe host may be unreachable, or HTTPS/TLS is not yet implemented in NexOS.\nTry an HTTP URL such as http://10.0.2.15:8080/";
            int l=(int)strlen_(e); memcpy_(t.page, e, l); t.page[l]=0; t.page_len=l; t.loading=false; t.status=-1;
        } else t.status = st;
    }

    static bool hit_link(int mx, int my, char href[256]){
        for (int i=0;i<g_nlinks;i++){
            if (g_links[i].has(mx,my)){ cpy(href, g_links[i].href, 256); return true; }
        }
        return false;
    }

    // ---- draw full browser chrome + content ----
    static void draw(Graphics& gfx, int id, Win11Window& win, int x, int y, int w, int h, int mx, int my){
        ensure_init();
        poll();
        Tab& t = g_tabs[g_active];

        // Tab bar
        int tabbar_h = 28;
        gfx.fill_rect(x, y, w, tabbar_h, 0xE0E0E0);
        gfx.draw_line(x, y+tabbar_h-1, x+w, y+tabbar_h-1, 0xC0C0C0);
        int tx = x+4;
        for (int i=0;i<g_ntabs;i++){
            int tw=150; bool act=(i==g_active);
            Color tbg = act ? COLOR_WHITE : 0xDCDCDC;
            gfx.fill_rounded_rect(tx, y+3, tw, tabbar_h-5, 4, tbg);
            if (act) gfx.draw_line(tx, y+tabbar_h-2, tx+tw, y+tabbar_h-2, COLOR_WHITE);
            const char* lbl = g_tabs[i].title[0]?g_tabs[i].title:(g_tabs[i].url[0]?g_tabs[i].url:"New Tab");
            char sh[40]; sh[0]=0; int sl=0; for(int k=0;lbl[k]&&sl<26;k++) sh[sl++]=lbl[k]; sh[sl]=0;
            gfx.draw_text(tx+6, y+8, sh, act?0x000000:0x333333, tbg);
            gfx.draw_text(tx+tw-14, y+7, "x", 0x666666, tbg);
            g_tab_rects[i].set(tx, y+3, tw, tabbar_h-5);
            g_tab_close[i].set(tx+tw-16, y+3, 14, tabbar_h-5);
            tx += tw+4;
        }
        gfx.fill_rounded_rect(tx+2, y+4, 22, tabbar_h-7, 4, 0xDCDCDC);
        gfx.draw_text(tx+8, y+7, "+", 0x1A1A1A, 0xDCDCDC);
        g_newtab_rect.set(tx+2, y+4, 22, tabbar_h-7);
        y += tabbar_h; h -= tabbar_h;

        // Nav bar
        int nav_h = 34;
        gfx.fill_rect(x, y, w, nav_h, 0xF2F2F2);
        const char* labels[5] = {"<",">","R","X","H"};
        int bx=x+4, bw=32;
        for (int i=0;i<5;i++){
            bool hover = g_nav_rects[i].has(mx, my);
            Color bbg = hover ? 0xE0E0E0 : COLOR_WHITE;
            gfx.fill_rounded_rect(bx, y+4, bw, nav_h-10, 5, bbg);
            gfx.draw_rounded_rect(bx, y+4, bw, nav_h-10, 5, 0xCCCCCC);
            gfx.draw_text(bx+11, y+10, labels[i], 0x1A1A1A, bbg);
            g_nav_rects[i].set(bx, y+4, bw, nav_h-10);
            bx+=bw+5;
        }
        int url_x = bx+4, url_w = x+w - url_x - 12;
        if (url_w < 40) url_w = 40;
        Color url_bg = win.browser_url_focused ? COLOR_WHITE : 0xFFFFFF;
        gfx.fill_rounded_rect(url_x, y+4, url_w, nav_h-10, 6, url_bg);
        gfx.draw_rounded_rect(url_x, y+4, url_w, nav_h-10, 6, win.browser_url_focused ? 0x0067C0 : 0xCCCCCC);
        const char* shown = t.url[0] ? t.url : "about:home";
        if (win.browser_url_focused && win.browser_url_len > 0) shown = win.browser_url;
        char sh2[140]; sh2[0]=0; int s2=0; for(int k=0;shown[k]&&s2<130;k++) sh2[s2++]=shown[k]; sh2[s2]=0;
        gfx.draw_text(url_x+8, y+10, sh2, 0x1A1A1A, url_bg);
        if (win.browser_url_focused){ int cx=url_x+8+(int)strlen_(sh2)*FONT_W; if(cx<url_x+url_w-4) gfx.fill_rect(cx, y+9, 2, 16, 0x1A1A1A); }
        y += nav_h; h -= nav_h;

        // Bookmarks bar
        int bm_h = 26;
        gfx.fill_rect(x, y, w, bm_h, 0xF8F8F8);
        gfx.draw_line(x, y+bm_h-1, x+w, y+bm_h-1, 0xE0E0E0);
        int bmx = x+6;
        for (int i=0;i<g_nbm;i++){
            const char* nm = g_bm[i].name;
            int nw = (int)strlen_(nm)*FONT_W + 14; if (nw<44) nw=44;
            bool bmh = g_bm_rects[i].has(mx, my);
            Color bmbg = bmh ? 0xE8E8E8 : 0xF8F8F8;
            gfx.fill_rounded_rect(bmx, y+3, nw, bm_h-7, 4, bmbg);
            gfx.draw_text(bmx+7, y+7, nm, 0x124FB0, bmbg);
            g_bm_rects[i].set(bmx, y+3, nw, bm_h-7);
            bmx += nw+4;
        }
        y += bm_h; h -= bm_h;

        // Content
        int status_h = 18;
        int content_x = x+2, content_y = y+2, content_w = w-16, content_h = h-4-status_h;
        if (content_w < 20) content_w = 20; if (content_h < 20) content_h = 20;
        gfx.fill_rect(content_x, content_y, content_w, content_h, COLOR_WHITE);
        gfx.draw_rect(content_x, content_y, content_w, content_h, 0xCCCCCC);
        render(gfx, content_x+6, content_y+4, content_w-12, content_h-8);

        // Scrollbar
        int sbx = content_x + content_w - 2;
        gfx.fill_rect(sbx, content_y, 8, content_h, 0xE8E8E8);
        int est = (t.page_len/70 + 16)*16; int maxs = est - (content_h-8); if (maxs<0) maxs=0;
        int th = (content_h-8 < est && est>0) ? ((content_h-8)*(content_h-8)/est) : (content_h-8);
        if (th<16) th=16;
        int toff = maxs>0 ? (t.scroll * ((content_h-8)-th) / maxs) : 0;
        gfx.fill_rect(sbx, content_y+toff, 8, th, 0xAAAAAA);
        g_scroll_rect.set(sbx, content_y, 8, content_h);
        g_scroll_max = maxs;

        // Status bar
        int status_y = content_y + content_h;
        gfx.fill_rect(content_x, status_y, content_w, status_h, 0xF8F8F8);
        gfx.draw_line(content_x, status_y, content_x+content_w, status_y, 0xE0E0E0);
        const char* stt;
        switch (t.status){ case 1: stt="Connecting..."; break; case 2: stt="Loading..."; break; case 3: stt="Done"; break; case -1: stt="Error"; break; default: stt="Ready"; }
        gfx.draw_text(content_x+6, status_y+3, stt, 0x666666, 0xF8F8F8);
    }

    } // namespace browser

struct Win11Desktop {
    Graphics gfx;
    MouseCursor cursor;
    Win11Window windows[MAX_WINDOWS];
    int window_count;
    int active_window;
    int drag_window;
    int drag_off_x, drag_off_y;

    DesktopIcon icons[MAX_ICONS];
    int icon_count;
    int selected_icon;

    StartMenuItem start_items[MAX_START_ITEMS];
    int start_item_count;
    bool start_menu_open;

    bool gui_mode;
    int mouse_x, mouse_y;
    bool mouse_left;
    int drag_counter;
    bool prev_mouse_left;

    // Clock
    int clock_h, clock_m, clock_s;

    // Animation frame driver (Phase 2 visual polish)
    int anim_tsc_per_ms;       // calibrated TSC ticks per ms, 0 = no usable clock
    uint32_t last_anim_tsc;    // timestamp of last animated frame
    bool anim_active_prev;     // was any window animating last frame
    int anim_stall;            // consecutive throttled passes (deadlock guard)
    uint32_t anim_ms_scale;    // 0xFFFFFFFF / anim_tsc_per_ms (tick_ms_now)

    // Frame counter (fallback ms source for managed double-click when the
    // TSC calibration was rejected) and desktop double-click state.
    uint32_t frame_counter;
    uint32_t dbl_t, dbl_kind;
    int      dbl_x, dbl_y;

    // Monotonic milliseconds for double-click detection.  Uses the
    // PIT-calibrated TSC rate when calibrate_tsc() succeeded; otherwise
    // falls back to the frame counter (~16 ms/frame is plenty for a
    // 500 ms window).
    //
    // The low 32 bits of rdtsc wrap every ~1.7 s at 2.5 GHz, so a naive
    // (uint32_t)rdtsc / per_ms restarts every couple of seconds and any
    // click separated by a slow move_to() looks like a "double click".
    // Use the full 64-bit TSC instead:
    //     ms = hi * (2^32 / per_ms) + lo / per_ms
    // The scale is precomputed in calibrate_tsc() with a plain 32-bit
    // divide (no libgcc), and hi*scale is one imul.  The 32-bit result
    // wraps only after ~49 days, which uint-difference arithmetic in the
    // callers tolerates.
    uint32_t tick_ms_now() {
        if (anim_tsc_per_ms > 0) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            uint32_t ms = (uint32_t)((uint64_t)hi * anim_ms_scale)
                        + lo / (uint32_t)anim_tsc_per_ms;
            return ms;
        }
        return frame_counter;
    }

    void init() {
        gfx.init();
        if (!gfx.initialized) return;
        cursor.init(gfx.width, gfx.height);
        window_count = 0;
        active_window = -1;
        drag_window = -1;
        gui_mode = false;
        mouse_left = false;
        drag_counter = 0;
        prev_mouse_left = false;
        start_menu_open = false;
        anim_active_prev = false;
        anim_tsc_per_ms = 0;
        anim_ms_scale = 0;
        last_anim_tsc = 0;
        anim_stall = 0;
        frame_counter = 0;
        dbl_t = 0; dbl_kind = 0xFFFFFFFFu; dbl_x = -9999; dbl_y = -9999;
        selected_icon = -1;
        clock_h = 0; clock_m = 0; clock_s = 0;

        // Setup desktop icons
        icon_count = 0;
        int icon_y = TOPBAR_H + 20;
        int icon_x = 16;
        struct { const char* name; AppType app; Color c; char l; } icon_defs[] = {
            {"Control Panel", APP_CONTROL_PANEL, 0x0067C0, 'C'},
            {"File Explorer", APP_FILE_EXPLORER, 0xF0C800, 'F'},
            {"Task Manager",  APP_TASK_MANAGER,  0x107C10, 'T'},
            {"Memory Optim.", APP_MEM_OPTIMIZER, 0x8B5CF6, 'M'},
            {"Terminal",      APP_TERMINAL,      0x1A1A1A, '>'},
            {"Browser",       APP_BROWSER,       0x0078D7, 'W'},
            {"Calculator",    APP_CALCULATOR,    0xCA5010, '#'},
            {"About NexOS",  APP_ABOUT,         0x0099BC, 'i'},
        };
        for (int i = 0; i < 8 && icon_count < MAX_ICONS; i++) {
            icons[icon_count].x = icon_x;
            icons[icon_count].y = icon_y + i * 76;
            strcpy_(icons[icon_count].label, icon_defs[i].name);
            icons[icon_count].app = icon_defs[i].app;
            icons[icon_count].icon_color = icon_defs[i].c;
            icons[icon_count].icon_letter = icon_defs[i].l;
            icons[icon_count].selected = false;
            icon_count++;
        }

        // Setup start menu items
        start_item_count = 0;
        struct { const char* name; AppType app; Color c; char l; } start_defs[] = {
            {"Control Panel", APP_CONTROL_PANEL, 0x0067C0, 'C'},
            {"File Explorer", APP_FILE_EXPLORER, 0xF0C800, 'F'},
            {"Task Manager",  APP_TASK_MANAGER,  0x107C10, 'T'},
            {"Memory Optimizer", APP_MEM_OPTIMIZER, 0x8B5CF6, 'M'},
            {"Terminal",      APP_TERMINAL,      0x1A1A1A, '>'},
            {"Browser",       APP_BROWSER,       0x0078D7, 'W'},
            {"Calculator",    APP_CALCULATOR,    0xCA5010, '#'},
            {"About",         APP_ABOUT,         0x0099BC, 'i'},
        };
        for (int i = 0; i < 8 && start_item_count < MAX_START_ITEMS; i++) {
            strcpy_(start_items[start_item_count].label, start_defs[i].name);
            start_items[start_item_count].app = start_defs[i].app;
            start_items[start_item_count].color = start_defs[i].c;
            start_items[start_item_count].letter = start_defs[i].l;
            start_item_count++;
        }
    }

    int create_window(int x, int y, int w, int h, const char* title, AppType app) {
        if (window_count >= MAX_WINDOWS) {
            // Find a closed window slot
            for (int i = 0; i < window_count; i++) {
                if (!windows[i].visible) {
                    return setup_window(i, x, y, w, h, title, app);
                }
            }
            return -1;
        }
        int id = window_count++;
        return setup_window(id, x, y, w, h, title, app);
    }

    int setup_window(int id, int x, int y, int w, int h, const char* title, AppType app) {
        windows[id].x = x; windows[id].y = y;
        windows[id].w = w; windows[id].h = h;
        windows[id].visible = true;
        windows[id].active = true;
        windows[id].app = app;
        windows[id].scroll_offset = 0;
        windows[id].selected_item = 0;
        windows[id].mem_optimized = false;
        windows[id].mem_before_kb = 0;
        windows[id].mem_after_kb = 0;
        windows[id].calc_display = 0;
        windows[id].calc_prev = 0;
        windows[id].calc_op = 0;
        windows[id].calc_new_input = true;
        windows[id].term_len = 0;
        windows[id].term_input_len = 0;
        windows[id].term_buf[0] = 0;
        windows[id].cp_category = -1;
        windows[id].tm_search[0] = 0;
        windows[id].tm_search_len = 0;
        windows[id].tm_search_focused = false;
        windows[id].tm_selected_proc = -1;
        windows[id].portal_tab = 0;
        windows[id].minimized = false;
        windows[id].fullscreen = false;
        windows[id].floating = false;
        windows[id].restore_x = x; windows[id].restore_y = y;
        windows[id].restore_w = w; windows[id].restore_h = h;
        windows[id].w32_index = -1;
        windows[id].w32_file[0] = 0;
        windows[id].managed_app = -1;
        windows[id].launch_kind = APP_NONE;
        windows[id].anim_state = 1;   // play opening animation
        windows[id].anim_p = 0;
        windows[id].term_input[0] = 0;
        windows[id].sel_file[0] = 0;
        windows[id].sel_file_idx = -1;
        windows[id].file_scroll = 0;
        windows[id].browser_url[0] = 0;
        windows[id].browser_url_len = 0;
        windows[id].browser_page[0] = 0;
        windows[id].browser_page_len = 0;
        windows[id].browser_scroll = 0;
        windows[id].browser_status = 0;
        windows[id].browser_url_focused = true;
        int tlen = strlen_(title);
        if (tlen > 39) tlen = 39;
        memcpy_(windows[id].title, title, tlen);
        windows[id].title[tlen] = 0;
        for (int i = 0; i < window_count; i++) {
            if (i != id) windows[i].active = false;
        }
        active_window = id;
        return id;
    }

    void close_window(int id) {
        // Defer the actual removal to the close animation (finish_close),
        // so the window can fade out smoothly instead of vanishing.
        if (windows[id].anim_state == 2) return; // already closing
        if (windows[id].anim_state == 0) {
            windows[id].anim_state = 2; // closing
            windows[id].anim_p = 0;
        }
    }

    // ---- Drawing functions ----

    void draw_wallpaper() {
        // Portal-style light background with subtle gradient
        gfx.fill_gradient(0, TOPBAR_H, gfx.width, gfx.height - TOPBAR_H,
                          C_PORTAL_BG, 0xE8EDF2);
    }

    void draw_portal_desktop() {
        // ---- Portal-style desktop (below top bar) ----
        int dx = 0;
        int dy = TOPBAR_H;
        int dw = gfx.width;
        (void)dy; // used indirectly via calculations below

        // ---- Search bar (centered, prominent) ----
        int search_w = 400;
        if (search_w > dw - 40) search_w = dw - 40;
        int search_h = 36;
        int sx = dx + (dw - search_w) / 2;
        int sy = dy + 24;

        // Search bar with magnifying glass icon
        gfx.fill_rounded_rect(sx, sy, search_w, search_h, search_h/2, C_PORTAL_SEARCH_BG);
        gfx.draw_rounded_rect(sx, sy, search_w, search_h, search_h/2, C_PORTAL_SEARCH_BORDER);
        // Search icon
        int six = sx + 14, siy = sy + search_h/2;
        gfx.draw_circle(six, siy, 5, C_WIN_TEXT_SEC);
        gfx.draw_line(six + 4, siy + 4, six + 9, siy + 9, C_WIN_TEXT_SEC);
        // Placeholder text
        gfx.draw_text_transparent(sx + 28, sy + (search_h - FONT_H)/2,
                                  "Search NexOS...", C_WIN_TEXT_SEC);

        // NexOS logo text above search
        const char* logo = "NexOS";
        int logo_w = strlen_(logo) * FONT_W * 2; // larger spacing
        gfx.draw_text_transparent(dx + (dw - logo_w)/2, sy - 28, logo, C_ACCENT);

        // ---- Quick access shortcuts grid ----
        int grid_y = sy + search_h + 24;
        int tile_w = 80;
        int tile_h = 60;
        int tile_gap = 12;
        int grid_cols = 8;
        int grid_w = grid_cols * tile_w + (grid_cols - 1) * tile_gap;
        if (grid_w > dw - 32) {
            grid_cols = (dw - 32) / (tile_w + tile_gap);
            grid_w = grid_cols * tile_w + (grid_cols - 1) * tile_gap;
        }
        int grid_x = dx + (dw - grid_w) / 2;

        // Shortcut definitions (reuse desktop icon definitions)
        struct { const char* name; AppType app; Color c; char l; } shortcuts[] = {
            {"Control",  APP_CONTROL_PANEL, 0x0067C0, 'C'},
            {"Files",    APP_FILE_EXPLORER, 0xF0C800, 'F'},
            {"Tasks",    APP_TASK_MANAGER,  0x107C10, 'T'},
            {"Memory",   APP_MEM_OPTIMIZER, 0x8B5CF6, 'M'},
            {"Terminal", APP_TERMINAL,      0x1A1A1A, '>'},
            {"Browser",  APP_BROWSER,       0x0078D7, 'W'},
            {"Calc",     APP_CALCULATOR,    0xCA5010, '#'},
            {"About",    APP_ABOUT,         0x0099BC, 'i'},
        };

        for (int i = 0; i < 8; i++) {
            int col = i % grid_cols;
            int row = i / grid_cols;
            int tx = grid_x + col * (tile_w + tile_gap);
            int ty = grid_y + row * (tile_h + tile_gap);

            bool hover = (mouse_x >= tx && mouse_x < tx + tile_w &&
                          mouse_y >= ty && mouse_y < ty + tile_h);

            gfx.draw_shortcut_tile(tx, ty, tile_w, tile_h,
                                   shortcuts[i].c, shortcuts[i].l,
                                   shortcuts[i].name, hover);
        }

        // ---- Navigation tabs ----
        int tab_y = grid_y + 2 * (tile_h + tile_gap) + 16;
        int tab_h = 28;
        const char* tabs[] = {"Home", "Apps", "System", "Tools"};
        int ntabs = 4;
        int tab_w = 80;
        int tab_gap = 4;
        int tabs_w = ntabs * tab_w + (ntabs - 1) * tab_gap;
        int tab_x = dx + (dw - tabs_w) / 2;

        for (int i = 0; i < ntabs; i++) {
            int tx = tab_x + i * (tab_w + tab_gap);
            bool selected = (i == 0); // Home is always selected on desktop
            gfx.draw_nav_tab(tx, tab_y, tab_w, tab_h, tabs[i], selected);
        }

        // ---- Content cards below tabs ----
        int card_y = tab_y + tab_h + 16;
        int card_w = (dw - 48) / 3;
        int card_h = 100;

        // Card 1: System Status
        {
            int cx = dx + 16;
            gfx.draw_card(cx, card_y, card_w, card_h, C_CARD_BG);
            gfx.draw_text_transparent(cx + 12, card_y + 8, "System Status", C_ACCENT);
            gfx.draw_line(cx + 12, card_y + 26, cx + card_w - 12, card_y + 26, C_WIN_BORDER);

            char buf[64];
            if (g_cb.is_64bit) {
                strcpy_(buf, g_cb.is_64bit() ? "Mode: 64-bit" : "Mode: 32-bit");
                gfx.draw_text_transparent(cx + 12, card_y + 32, buf, C_WIN_TEXT);
            }
            if (g_cb.get_total_mem_kb) {
                uint32_t total = g_cb.get_total_mem_kb();
                strcpy_(buf, "RAM: ");
                uint_to_str(total / 1024, buf + strlen_(buf));
                strcat_safe(buf, " MB", sizeof(buf));
                gfx.draw_text_transparent(cx + 12, card_y + 50, buf, C_WIN_TEXT);
            }
            // Memory usage bar
            if (g_cb.get_used_pages && g_cb.get_total_pages) {
                uint32_t used = g_cb.get_used_pages();
                uint32_t tp = g_cb.get_total_pages();
                int pct = tp > 0 ? (int)(used * 100 / tp) : 0;
                gfx.draw_progress(cx + 12, card_y + 72, card_w - 24, 8, pct,
                                  pct < 50 ? C_MEM_GOOD : (pct < 80 ? C_MEM_WARN : C_MEM_BAD));
            }
        }

        // Card 2: Clock
        {
            int cx = dx + 16 + card_w + 16;
            gfx.draw_card(cx, card_y, card_w, card_h, C_CARD_BG);
            gfx.draw_text_transparent(cx + 12, card_y + 8, "Clock", C_ACCENT);
            gfx.draw_line(cx + 12, card_y + 26, cx + card_w - 12, card_y + 26, C_WIN_BORDER);

            char time_str[16];
            time_str[0] = '0' + (clock_h / 10);
            time_str[1] = '0' + (clock_h % 10);
            time_str[2] = ':';
            time_str[3] = '0' + (clock_m / 10);
            time_str[4] = '0' + (clock_m % 10);
            time_str[5] = ':';
            time_str[6] = '0' + (clock_s / 10);
            time_str[7] = '0' + (clock_s % 10);
            time_str[8] = 0;
            // Draw time in large style (centered)
            gfx.draw_text_centered(cx, card_y + 36, card_w, time_str, C_ACCENT);

            // Date placeholder
            gfx.draw_text_centered(cx, card_y + 60, card_w, "NexOS Desktop", C_WIN_TEXT_SEC);
        }

        // Card 3: Quick Actions
        {
            int cx = dx + 16 + 2 * (card_w + 16);
            gfx.draw_card(cx, card_y, card_w, card_h, C_CARD_BG);
            gfx.draw_text_transparent(cx + 12, card_y + 8, "Quick Actions", C_ACCENT);
            gfx.draw_line(cx + 12, card_y + 26, cx + card_w - 12, card_y + 26, C_WIN_BORDER);

            gfx.draw_text_transparent(cx + 12, card_y + 34, "Click shortcuts above", C_WIN_TEXT_SEC);
            gfx.draw_text_transparent(cx + 12, card_y + 52, "or use Start menu", C_WIN_TEXT_SEC);
            gfx.draw_text_transparent(cx + 12, card_y + 70, "to launch apps", C_WIN_TEXT_SEC);
        }
    }

    void draw_topbar() {
        // Top status bar background
        gfx.fill_rect(0, 0, gfx.width, TOPBAR_H, C_TOPBAR_BG);
        gfx.draw_line(0, TOPBAR_H, gfx.width, TOPBAR_H, C_TOPBAR_BORDER);

        // Start button (Win logo square)
        int sbx = 8, sby = 6;
        int sbw = 44, sbh = 20;
        Color sb_bg = start_menu_open ? C_TOPBAR_HOVER : C_TOPBAR_BG;
        // Check hover
        if (mouse_x >= sbx && mouse_x < sbx + sbw && mouse_y >= 0 && mouse_y < TOPBAR_H) {
            sb_bg = C_TOPBAR_HOVER;
        }
        gfx.fill_rect(sbx, sby, sbw, sbh, sb_bg);
        // 4 squares logo
        gfx.fill_rect(sbx + 8, sby + 3, 6, 6, C_ACCENT);
        gfx.fill_rect(sbx + 16, sby + 3, 6, 6, C_ACCENT_LIGHT);
        gfx.fill_rect(sbx + 8, sby + 11, 6, 6, C_ACCENT_LIGHT);
        gfx.fill_rect(sbx + 16, sby + 11, 6, 6, C_ACCENT_ORANGE);
        gfx.draw_text(sbx + 26, sby + 2, "Start", C_TOPBAR_TEXT, sb_bg);

        // Running app indicators (center area)
        int ax = sbx + sbw + 16;
        for (int i = 0; i < window_count; i++) {
            if (!windows[i].visible) continue;
            int aw = strlen_(windows[i].title) * FONT_W + 24;
            Color abg = windows[i].active ? C_TOPBAR_HOVER : C_TOPBAR_BG;
            if (mouse_x >= ax && mouse_x < ax + aw && mouse_y >= 0 && mouse_y < TOPBAR_H)
                abg = C_TOPBAR_HOVER;
            gfx.fill_rect(ax, 4, aw, TOPBAR_H - 8, abg);
            gfx.draw_text(ax + 8, 8, windows[i].title, C_TOPBAR_TEXT, abg);
            // Active indicator line
            if (windows[i].active) {
                gfx.fill_rect(ax, TOPBAR_H - 4, aw, 3, C_ACCENT);
            }
            ax += aw + 4;
        }

        // Right side: clock removed by request; language indicator is drawn
        // in render_all() so it overlays the managed C# desktop as well.
        int rx = gfx.width - 8;

        // Memory indicator (compact)
        if (g_cb.get_total_mem_kb) {
            uint32_t used_p = g_cb.get_used_pages ? g_cb.get_used_pages() : 0;
            uint32_t total_p = g_cb.get_total_pages ? g_cb.get_total_pages() : 1;
            int pct = total_p > 0 ? (int)(used_p * 100 / total_p) : 0;
            char mem_str[8];
            mem_str[0] = 'M';
            int_to_str(pct, mem_str + 1);
            int ml = strlen_(mem_str);
            mem_str[ml] = '%'; mem_str[ml+1] = 0;
            int mw = strlen_(mem_str) * FONT_W + 8;
            gfx.draw_text(rx - mw, 8, mem_str, C_TOPBAR_TEXT, C_TOPBAR_BG);
            rx -= mw + 8;
        }

        // 64-bit indicator
        if (g_cb.is_64bit && g_cb.is_64bit()) {
            gfx.draw_text(rx - 3*FONT_W, 8, "64b", C_MEM_GOOD, C_TOPBAR_BG);
            rx -= 3 * FONT_W + 8;
        }
    }

    void draw_desktop_icons() {
        for (int i = 0; i < icon_count; i++) {
            DesktopIcon& ic = icons[i];
            int sz = 48;
            // Selection background
            if (ic.selected) {
                gfx.fill_rounded_rect(ic.x - 4, ic.y - 4, sz + 8, 60, 4, C_ICON_SELECTED);
            }
            // Icon
            gfx.draw_icon(ic.x, ic.y, sz, ic.icon_color, ic.icon_letter, COLOR_WHITE);
            // Label (centered under icon)
            int lw = strlen_(ic.label) * FONT_W;
            int lx = ic.x + (sz - lw) / 2;
            if (lx < ic.x) lx = ic.x;
            gfx.draw_text_transparent(lx, ic.y + sz + 4, ic.label, C_ICON_TEXT);
        }
    }

    void draw_start_menu() {
        const int mw = 380, mh = 248;
        const int mx = 8, my = TOPBAR_H + 2;

        // Shadow + background
        gfx.fill_rect(mx + 3, my + 3, mw, mh, 0x40000000);
        gfx.fill_rounded_rect(mx, my, mw, mh, 10, C_STARTMENU_BG);
        gfx.draw_rounded_rect(mx, my, mw, mh, 10, C_STARTMENU_BORDER);

        // Header
        gfx.draw_text(mx + 16, my + 12, "Pinned", C_TOPBAR_TEXT, C_STARTMENU_BG);
        gfx.draw_text(mx + mw - 16 - 8 * FONT_W, my + 12, "All apps", C_ACCENT, C_STARTMENU_BG);

        // Pinned grid: 4 columns x 2 rows of app tiles
        const int tile_w = 80, tile_h = 64, gap = 8;
        const int grid_x = mx + 16, grid_y = my + 40;
        for (int i = 0; i < start_item_count; i++) {
            int col = i % 4, row = i / 4;
            int tx = grid_x + col * (tile_w + gap);
            int ty = grid_y + row * (tile_h + gap);
            bool hover = (mouse_x >= tx && mouse_x < tx + tile_w &&
                          mouse_y >= ty && mouse_y < ty + tile_h);
            Color tbg = hover ? C_STARTMENU_HOVER : C_STARTMENU_BG;
            gfx.fill_rounded_rect(tx, ty, tile_w, tile_h, 6, tbg);
            gfx.draw_icon(tx + (tile_w - 32) / 2, ty + 8, 32,
                          start_items[i].color, start_items[i].letter, COLOR_WHITE);
            int lw = strlen_(start_items[i].label) * FONT_W;
            int lx = tx + (tile_w - lw) / 2; if (lx < tx) lx = tx;
            gfx.draw_text(lx, ty + tile_h - 16, start_items[i].label, C_TOPBAR_TEXT, tbg);
        }

        // Power bar (bottom): Shut down / Restart
        const int pbar_y = my + mh - 44;
        gfx.draw_line(mx + 8, pbar_y - 6, mx + mw - 8, pbar_y - 6, C_STARTMENU_BORDER);
        int pw = (mw - 32 - 12) / 2;
        int sx = mx + 16, rx = sx + pw + 12;
        bool sh_hover = (mouse_x >= sx && mouse_x < sx + pw && mouse_y >= pbar_y && mouse_y < pbar_y + 32);
        bool rs_hover = (mouse_x >= rx && mouse_x < rx + pw && mouse_y >= pbar_y && mouse_y < pbar_y + 32);
        gfx.fill_rounded_rect(sx, pbar_y, pw, 32, 6, sh_hover ? C_STARTMENU_HOVER : C_STARTMENU_BG);
        gfx.fill_rounded_rect(rx, pbar_y, pw, 32, 6, rs_hover ? C_STARTMENU_HOVER : C_STARTMENU_BG);
        gfx.draw_text(sx + 10, pbar_y + 9, "Shut down", C_TOPBAR_TEXT, sh_hover ? C_STARTMENU_HOVER : C_STARTMENU_BG);
        gfx.draw_text(rx + 10, pbar_y + 9, "Restart",   C_TOPBAR_TEXT, rs_hover ? C_STARTMENU_HOVER : C_STARTMENU_BG);
    }

    void draw_window(int id, int rx = -1, int ry = -1, int rw = -1, int rh = -1, int alpha = 255) {
        Win11Window& win = windows[id];
        if (!win.visible || win.minimized) return;
        if (rx < 0) { rx = win.x; ry = win.y; rw = win.w; rh = win.h; }

        // Window shadow (skip in fullscreen)
        if (!win.fullscreen) {
            for (int i = 0; i < 4; i++) {
                gfx.fill_rect(rx + 3 + i, ry + 3 + i, rw, rh, 0x20000000);
            }
        }

        // Window background
        if (win.fullscreen)
            gfx.fill_rect(rx, ry, rw, rh, C_WIN_BG);
        else
            gfx.fill_rounded_rect(rx, ry, rw, rh, 6, C_WIN_BG);

        // Title bar
        Color tb_color = win.active ? C_WIN_TITLEBAR_ACT : C_WIN_TITLEBAR;
        if (win.fullscreen)
            gfx.fill_rect(rx, ry, rw, TITLE_BAR_H, tb_color);
        else {
            gfx.fill_rounded_rect(rx, ry, rw, TITLE_BAR_H, 6, tb_color);
            gfx.fill_rect(rx, ry + TITLE_BAR_H - 6, rw, 6, tb_color);
        }
        // Chrome texture over the title bar (subtle; flat colour if absent)
        if (g_tex[TEX_CHROME].loaded)
            gfx.draw_image(g_tex[TEX_CHROME], rx, ry, rw, TITLE_BAR_H);

        // Title text (centered)
        int tw = strlen_(win.title) * FONT_W;
        gfx.draw_text(rx + (rw - tw) / 2, ry + 8, win.title, C_WIN_TEXT, tb_color);

        // Title-bar control buttons: which 0=close 1=fullscreen 2=minimize 3=float
        for (int b = 0; b < 4; b++) {
            int bx = rx + rw - 28 - b * 24;
            int by = ry + 4;
            bool hover = (mouse_x >= bx && mouse_x < bx + 24 &&
                          mouse_y >= by && mouse_y < by + 24);
            Color bbg = hover ? C_CLOSE_HOVER : tb_color;
            gfx.fill_rounded_rect(bx, by, 24, 24, 4, bbg);
            const char* g = (b == 0) ? "X" : (b == 1) ? "M" : (b == 2) ? "_" : "T";
            bool active_state = (b == 1 && win.fullscreen) || (b == 3 && win.floating);
            Color gcol = active_state ? C_ACCENT : (hover ? COLOR_WHITE : C_CLOSE_TEXT);
            gfx.draw_text(bx + 8, by + 4, g, gcol, bbg);
        }

        // Border
        Color border = win.active ? C_WIN_BORDER_ACT : C_WIN_BORDER;
        if (win.fullscreen)
            gfx.draw_rect(rx, ry, rw, rh, border);
        else
            gfx.draw_rounded_rect(rx, ry, rw, rh, 6, border);

        // Separator line below title bar
        gfx.draw_line(rx + 1, ry + TITLE_BAR_H, rx + rw - 2, ry + TITLE_BAR_H, C_WIN_BORDER);

        // Draw app content
        draw_app_content(id, rx, ry, rw, rh);

        // Fade overlay for open/close/minimize animations
        if (alpha < 255) gfx.blend_rect(rx, ry, rw, rh, 0x000000, 255 - alpha);
    }

    // ---- App content rendering ----
    void draw_app_content(int id, int rx, int ry, int rw, int rh) {
        Win11Window& win = windows[id];
        int cx = rx + 8;
        int cy = ry + TITLE_BAR_H + 8;
        int cw = rw - 16;
        int ch = rh - TITLE_BAR_H - 16;

        switch (win.app) {
            case APP_CONTROL_PANEL:   draw_control_panel(id, cx, cy, cw, ch); break;
            case APP_FILE_EXPLORER:   draw_file_explorer(id, cx, cy, cw, ch); break;
            case APP_TASK_MANAGER:    draw_task_manager(id, cx, cy, cw, ch); break;
            case APP_MEM_OPTIMIZER:   draw_mem_optimizer(id, cx, cy, cw, ch); break;
            case APP_CALCULATOR:      draw_calculator(id, cx, cy, cw, ch); break;
            case APP_TERMINAL:        draw_terminal(id, cx, cy, cw, ch); break;
            case APP_ABOUT:           draw_about(id, cx, cy, cw, ch); break;
            case APP_BROWSER:         draw_browser(id, cx, cy, cw, ch); break;
            case APP_WIN32:           draw_win32_app(id, rx + 2, ry + TITLE_BAR_H + 2,
                                                     rw - 4, rh - TITLE_BAR_H - 4); break;
            case APP_MANAGED: {
                // Hand the whole client area to the managed shell.  It
                // paints in client pixels; mforms.cpp adds this origin and
                // clips to the rectangle, so a buggy C# app cannot smear
                // over the title bar or a neighbouring window.
                int ox = rx + 1;
                int oy = ry + TITLE_BAR_H;
                int mw = rw - 2;
                int mh = rh - TITLE_BAR_H;
                mforms_set_mouse(mouse_x, mouse_y);
                mforms_paint(win.managed_app, ox, oy, mw, mh);
                break;
            }
            default: break;
        }
    }

    // ---- Control Panel ----
    void draw_control_panel(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];
        char buf[128];

        // Category definitions
        struct CatDef { const char* title; const char* desc; Color color; char letter; };
        static const CatDef cats[] = {
            {"System & Security",  "Check system status",       C_CAT_SYSTEM,     'S'},
            {"Network & Internet", "View network status",       C_CAT_NETWORK,    'N'},
            {"Hardware & Sound",   "View devices",              C_CAT_HARDWARE,   'H'},
            {"Programs",           "Uninstall programs",        C_CAT_PROGRAMS,   'P'},
            {"User Accounts",      "Change account type",       C_CAT_ACCOUNT,    'U'},
            {"Appearance",         "Display settings",          C_CAT_APPEARANCE, 'A'},
            {"Time & Region",      "Change date, time",         C_CAT_TIME,       'T'},
            {"Ease of Access",     "Optimize display",          C_CAT_ACCESS,     'E'},
        };

        if (win.cp_category < 0) {
            // ---- Category list view (like Windows Control Panel) ----
            // Header: "Adjust your computer's settings"
            gfx.draw_text_transparent(x, y, "Adjust your ", C_WIN_TEXT_SEC);
            int hw = strlen_("Adjust your ") * FONT_W;
            gfx.draw_text_transparent(x + hw, y, "computer", C_ACCENT);
            hw += strlen_("computer") * FONT_W;
            gfx.draw_text_transparent(x + hw, y, "'s settings", C_WIN_TEXT_SEC);
            y += 24;

            // View selector (right-aligned)
            const char* view_label = "View by: Category";
            int vw = strlen_(view_label) * FONT_W;
            gfx.draw_text_transparent(x + w - vw, y - 20, view_label, C_WIN_TEXT_SEC);
            gfx.draw_line(x, y, x + w, y, C_WIN_BORDER);
            y += 12;

            // 2-column grid of categories
            int col_w = (w - 12) / 2;  // 2 columns with gap
            int row_h = 64;
            int gap = 8;

            for (int i = 0; i < 8; i++) {
                int col = i % 2;
                int row = i / 2;
                int cx = x + col * (col_w + gap);
                int cy = y + row * (row_h + gap);

                // Check hover
                bool hover = (mouse_x >= cx && mouse_x < cx + col_w &&
                              mouse_y >= cy && mouse_y < cy + row_h);

                // Card background
                Color card_bg = hover ? C_CARD_HOVER : C_CARD_BG;
                gfx.fill_rounded_rect(cx, cy, col_w, row_h, 6, card_bg);
                if (hover) gfx.draw_rounded_rect(cx, cy, col_w, row_h, 6, C_ACCENT_LIGHT);

                // Category icon (circle)
                int icon_cx = cx + 28;
                int icon_cy = cy + row_h / 2;
                gfx.fill_circle(icon_cx, icon_cy, 18, cats[i].color);
                // Letter
                int lx = icon_cx - FONT_W / 2;
                int ly = icon_cy - FONT_H / 2;
                gfx.draw_char(lx, ly, cats[i].letter, COLOR_WHITE, cats[i].color);

                // Title (bold blue)
                gfx.draw_text_transparent(cx + 56, cy + 10, cats[i].title, C_ACCENT);
                // Description (gray)
                gfx.draw_text_transparent(cx + 56, cy + 28, cats[i].desc, C_WIN_TEXT_SEC);
            }
        } else {
            // ---- Category detail view ----
            int cat = win.cp_category;

            // Back button
            draw_action_button(x, y, 80, 24, "< Back", false, win);
            y += 32;

            // Category title with icon
            gfx.fill_circle(x + 16, y + 10, 14, cats[cat].color);
            gfx.draw_char(x + 12, y + 2, cats[cat].letter, COLOR_WHITE, cats[cat].color);
            gfx.draw_text_transparent(x + 38, y + 2, cats[cat].title, C_ACCENT);
            y += 28;
            gfx.draw_line(x, y, x + w, y, C_WIN_BORDER);
            y += 8;

            switch (cat) {
            case 0: // System & Security
                gfx.draw_text_transparent(x, y, "System Information", C_ACCENT);
                y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER);
                y += 8;

                if (g_cb.get_os_name) {
                    strcpy_(buf, "OS: ");
                    strcat_safe(buf, g_cb.get_os_name(), sizeof(buf));
                } else {
                    strcpy_(buf, "OS: NexOS");
                }
                gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;

                if (g_cb.is_64bit) {
                    strcpy_(buf, "Arch: ");
                    strcat_safe(buf, g_cb.is_64bit() ? "x86-64 (Long Mode)" : "x86 (Protected Mode)", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }

                strcpy_(buf, "Display: ");
                uint_to_str(gfx.width, buf + strlen_(buf));
                strcat_safe(buf, "x", sizeof(buf));
                uint_to_str(gfx.height, buf + strlen_(buf));
                gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;

                if (g_cb.get_total_mem_kb) {
                    uint32_t total = g_cb.get_total_mem_kb();
                    strcpy_(buf, "Memory: ");
                    uint_to_str(total / 1024, buf + strlen_(buf));
                    strcat_safe(buf, " MB total", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }

                if (g_cb.get_used_pages && g_cb.get_total_pages) {
                    uint32_t used = g_cb.get_used_pages();
                    uint32_t total = g_cb.get_total_pages();
                    int pct = total > 0 ? (int)(used * 100 / total) : 0;
                    strcpy_(buf, "Page Usage: ");
                    uint_to_str(pct, buf + strlen_(buf));
                    strcat_safe(buf, "%", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 20;
                    Color bar_color = pct < 50 ? C_MEM_GOOD : (pct < 80 ? C_MEM_WARN : C_MEM_BAD);
                    gfx.draw_progress(x, y, w, 12, pct, bar_color); y += 24;
                }

                if (g_cb.get_heap_alloc_bytes) {
                    uint32_t ha = g_cb.get_heap_alloc_bytes();
                    uint32_t hf = g_cb.get_heap_free_bytes();
                    strcpy_(buf, "Heap: ");
                    uint_to_str(ha / 1024, buf + strlen_(buf));
                    strcat_safe(buf, "KB used / ", sizeof(buf));
                    uint_to_str(hf / 1024, buf + strlen_(buf));
                    strcat_safe(buf, "KB free", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }

                y += 8;
                gfx.draw_text_transparent(x, y, "Quick Actions", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;
                draw_action_button(x, y, 140, 28, "Optimize Memory", win.mem_optimized, win);
                draw_action_button(x + 150, y, 140, 28, "Task Manager", false, win);
                if (win.mem_optimized) {
                    y += 36;
                    strcpy_(buf, "Saved ");
                    uint_to_str(win.mem_before_kb - win.mem_after_kb, buf + strlen_(buf));
                    strcat_safe(buf, " KB", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_MEM_GOOD);
                }
                break;

            case 1: // Network & Internet
                gfx.draw_text_transparent(x, y, "Network Status", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;

                if (g_cb.get_nic_present) {
                    strcpy_(buf, "NIC: ");
                    strcat_safe(buf, g_cb.get_nic_present() ? "NE2000/PCI detected" : "No NIC", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;

                    if (g_cb.get_nic_present()) {
                        gfx.draw_text_transparent(x, y, "Status: Connected", C_MEM_GOOD); y += 18;
                        gfx.draw_text_transparent(x, y, "Driver: NE2000", C_WIN_TEXT_SEC); y += 18;
                    }
                } else {
                    gfx.draw_text_transparent(x, y, "No network interface found", C_WIN_TEXT_SEC); y += 18;
                }
                break;

            case 2: // Hardware & Sound
                gfx.draw_text_transparent(x, y, "Devices", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;

                if (g_cb.get_cpu_vendor) {
                    strcpy_(buf, "CPU: ");
                    strcat_safe(buf, g_cb.get_cpu_vendor(), sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }
                if (g_cb.get_disk_model && g_cb.get_disk_size_mb) {
                    strcpy_(buf, "Disk: ");
                    strcat_safe(buf, g_cb.get_disk_model(), sizeof(buf));
                    strcat_safe(buf, " (", sizeof(buf));
                    uint_to_str(g_cb.get_disk_size_mb(), buf + strlen_(buf));
                    strcat_safe(buf, " MB)", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }
                if (g_cb.get_keyboard_present) {
                    strcpy_(buf, "Keyboard: ");
                    strcat_safe(buf, g_cb.get_keyboard_present() ? "PS/2 detected" : "Not found", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }
                if (g_cb.get_mouse_present) {
                    strcpy_(buf, "Mouse: ");
                    strcat_safe(buf, g_cb.get_mouse_present() ? "PS/2 detected" : "Not found", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }
                if (g_cb.get_pci_count) {
                    strcpy_(buf, "PCI Devices: ");
                    uint_to_str(g_cb.get_pci_count(), buf + strlen_(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }
                if (g_cb.get_bga_available && g_cb.get_vbe_mode_set) {
                    strcpy_(buf, "Display: ");
                    if (g_cb.get_bga_available()) strcat_safe(buf, "BGA ", sizeof(buf));
                    if (g_cb.get_vbe_mode_set()) strcat_safe(buf, "BIOS-mode ", sizeof(buf));
                    strcat_safe(buf, g_bga_available ? "(emulator)" : "(real HW)", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT_SEC); y += 18;
                }
                break;

            case 3: // Programs
                gfx.draw_text_transparent(x, y, "Running Programs", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;

                {
                    int proc = 0;
                    for (int i = 0; i < window_count; i++) {
                        if (!windows[i].visible) continue;
                        strcpy_(buf, "  ");
                        strcat_safe(buf, windows[i].title, sizeof(buf));
                        gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 16;
                        proc++;
                    }
                    strcpy_(buf, "  NexOS Shell (PID 1)");
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 16;
                    strcpy_(buf, "Total: ");
                    int_to_str(proc + 1, buf + strlen_(buf));
                    strcat_safe(buf, " programs", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT_SEC);
                }
                break;

            case 4: // User Accounts
                gfx.draw_text_transparent(x, y, "Account Information", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;
                gfx.draw_text_transparent(x, y, "User: Administrator", C_WIN_TEXT); y += 18;
                gfx.draw_text_transparent(x, y, "Type: Local Account", C_WIN_TEXT); y += 18;
                gfx.draw_text_transparent(x, y, "Privileges: Full", C_WIN_TEXT); y += 18;
                gfx.draw_text_transparent(x, y, "Groups: Administrators", C_WIN_TEXT_SEC); y += 18;
                break;

            case 5: // Appearance
                gfx.draw_text_transparent(x, y, "Display Settings", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;
                strcpy_(buf, "Resolution: ");
                uint_to_str(gfx.width, buf + strlen_(buf));
                strcat_safe(buf, "x", sizeof(buf));
                uint_to_str(gfx.height, buf + strlen_(buf));
                gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;

                strcpy_(buf, "Color Depth: ");
                uint_to_str(gfx.bpp, buf + strlen_(buf));
                strcat_safe(buf, " bpp", sizeof(buf));
                gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;

                {
                    const char* fmt = gfx.pixel_format == 0 ? "BGRX32" :
                                      gfx.pixel_format == 1 ? "RGBX32" :
                                      gfx.pixel_format == 2 ? "RGB24" :
                                      gfx.pixel_format == 3 ? "RGB565" : "Unknown";
                    strcpy_(buf, "Pixel Format: ");
                    strcat_safe(buf, fmt, sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                }
                strcpy_(buf, "Double Buffering: ");
                strcat_safe(buf, gfx.backbuffer ? "Enabled" : "Disabled", sizeof(buf));
                gfx.draw_text_transparent(x, y, buf, C_MEM_GOOD); y += 18;
                break;

            case 6: // Time & Region
                gfx.draw_text_transparent(x, y, "Date and Time", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;

                {
                    char time_str[32];
                    strcpy_(time_str, "Time: ");
                    time_str[6] = '0' + (clock_h / 10);
                    time_str[7] = '0' + (clock_h % 10);
                    time_str[8] = ':';
                    time_str[9] = '0' + (clock_m / 10);
                    time_str[10] = '0' + (clock_m % 10);
                    time_str[11] = ':';
                    time_str[12] = '0' + (clock_s / 10);
                    time_str[13] = '0' + (clock_s % 10);
                    time_str[14] = 0;
                    gfx.draw_text_transparent(x, y, time_str, C_WIN_TEXT); y += 18;
                }
                gfx.draw_text_transparent(x, y, "Region: System Default", C_WIN_TEXT_SEC); y += 18;
                gfx.draw_text_transparent(x, y, "Format: English (US)", C_WIN_TEXT_SEC); y += 18;
                break;

            case 7: // Ease of Access
                gfx.draw_text_transparent(x, y, "Accessibility", C_ACCENT); y += 20;
                gfx.draw_line(x, y, x + w, y, C_WIN_BORDER); y += 8;
                gfx.draw_text_transparent(x, y, "High Contrast: Off", C_WIN_TEXT); y += 18;
                gfx.draw_text_transparent(x, y, "Cursor Size: Standard", C_WIN_TEXT); y += 18;
                gfx.draw_text_transparent(x, y, "Font Scale: 100%", C_WIN_TEXT); y += 18;
                gfx.draw_text_transparent(x, y, "Screen Reader: Not available", C_WIN_TEXT_SEC); y += 18;
                break;
            }
        }
    }

    // ---- File type detection ----
    enum FileType { FT_TEXT, FT_BMP, FT_AUDIO, FT_VIDEO, FT_DIR, FT_UNKNOWN };

    FileType detect_file_type(const char* name) {
        // Check for directory prefix
        if (name[0] == '[' && name[1] == 'D') return FT_DIR;

        // Find extension - look for last '.'
        const char* ext = 0;
        for (int i = 0; name[i]; i++) {
            if (name[i] == '.') ext = name + i + 1;
        }
        if (!ext) return FT_UNKNOWN;

        // Compare extension (case-insensitive)
        auto ext_eq = [](const char* a, const char* b) -> bool {
            while (*a && *b) {
                char ca = *a, cb = *b;
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) return false;
                a++; b++;
            }
            return *a == 0 && *b == 0;
        };

        if (ext_eq(ext, "txt") || ext_eq(ext, "c") || ext_eq(ext, "h") ||
            ext_eq(ext, "cpp") || ext_eq(ext, "asm") || ext_eq(ext, "s") ||
            ext_eq(ext, "log") || ext_eq(ext, "md") || ext_eq(ext, "cfg") ||
            ext_eq(ext, "ini") || ext_eq(ext, "bat") || ext_eq(ext, "sh"))
            return FT_TEXT;
        if (ext_eq(ext, "bmp")) return FT_BMP;
        if (ext_eq(ext, "wav") || ext_eq(ext, "mp3") || ext_eq(ext, "ogg") || ext_eq(ext, "flac"))
            return FT_AUDIO;
        if (ext_eq(ext, "avi") || ext_eq(ext, "mp4") || ext_eq(ext, "mkv") || ext_eq(ext, "mov"))
            return FT_VIDEO;
        return FT_UNKNOWN;
    }

    // Extract clean filename from list entry (strip "[D] ", "    ", " (sizeB)")
    void extract_filename(const char* entry, char* out, int outsize) {
        const char* p = entry;
        // Skip "[D] " or "    " prefix
        if (p[0] == '[' && p[1] == 'D' && p[2] == ']' && p[3] == ' ') p += 4;
        while (*p == ' ') p++;
        // Copy until newline or '(' or end
        int i = 0;
        while (*p && *p != '\n' && *p != '(' && i < outsize - 1) {
            out[i++] = *p++;
        }
        // Trim trailing spaces
        while (i > 0 && out[i-1] == ' ') i--;
        out[i] = 0;
    }

    // ---- File Explorer ----
    void draw_file_explorer(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];
        gfx.draw_text_transparent(x, y, "File Explorer", C_WIN_TEXT);
        y += 24;

        // File system tabs: MKFS, SFS, FAT32
        int tab_w = 70;
        const char* tabs[] = {"MKFS", "SFS", "FAT32"};
        for (int i = 0; i < 3; i++) {
            int tx = x + i * (tab_w + 4);
            bool active = (win.selected_item == i);
            bool hover = (mouse_x >= tx && mouse_x < tx + tab_w &&
                         mouse_y >= y && mouse_y < y + 22);
            Color bg = active ? C_ACCENT : (hover ? C_BTN_HOVER : C_BTN_BG);
            gfx.fill_rounded_rect(tx, y, tab_w, 22, 4, bg);
            gfx.draw_text(tx + 12, y + 3, tabs[i], active ? COLOR_WHITE : C_BTN_TEXT, bg);
        }
        y += 30;

        // Separator
        gfx.draw_line(x, y, x + w, y, C_WIN_BORDER);
        y += 4;

        // Layout: left = file list (220px), right = preview
        int list_w = 220;
        int preview_x = x + list_w + 4;
        int preview_w = w - list_w - 8;
        int list_h = h - (y - win.content_y() - 8);

        // Draw divider between list and preview
        gfx.draw_line(x + list_w + 2, y, x + list_w + 2, y + list_h, C_WIN_BORDER);

        // Get file list from kernel
        static char filebuf[4096];
        int fs_type = win.selected_item; // 0=MKFS, 1=SFS, 2=FAT32
        int file_count = 0;
        if (g_cb.list_files) {
            file_count = g_cb.list_files(fs_type, filebuf, sizeof(filebuf));
        }

        if (file_count <= 0) {
            gfx.draw_text_transparent(x + 8, y + 8, "(no files or FS not mounted)", C_WIN_TEXT_SEC);
            // Preview pane placeholder
            gfx.fill_rect(preview_x, y, preview_w, list_h, C_WIN_BG);
            gfx.draw_text_transparent(preview_x + 8, y + 8, "Preview", C_WIN_TEXT_SEC);
            return;
        }

        // Parse and display files (newline-separated) - left pane
        int line_y = y + 4;
        char* p = filebuf;
        int max_lines = list_h / 18;
        int line = 0;

        // Store file entries for click detection
        static char file_entries[64][64];
        static int file_entry_count = 0;
        file_entry_count = 0;

        while (*p && line < max_lines && file_entry_count < 64) {
            // Extract one line
            char fname[64];
            int fi = 0;
            while (*p && *p != '\n' && fi < 63) {
                fname[fi++] = *p++;
            }
            fname[fi] = 0;
            if (*p == '\n') p++;

            // Save entry for click detection
            memcpy_(file_entries[file_entry_count], fname, fi + 1);
            file_entry_count++;

            // Extract clean filename for type detection
            char clean_name[64];
            extract_filename(fname, clean_name, sizeof(clean_name));
            FileType ftype = detect_file_type(fname);

            // Determine icon color based on file type
            Color icon_color;
            const char* icon_letter;
            if (ftype == FT_DIR) {
                icon_color = 0xFFD700; icon_letter = "D";
            } else if (ftype == FT_TEXT) {
                icon_color = 0x4FC3F7; icon_letter = "T";
            } else if (ftype == FT_BMP) {
                icon_color = 0x66BB6A; icon_letter = "I";
            } else if (ftype == FT_AUDIO) {
                icon_color = 0xAB47BC; icon_letter = "A";
            } else if (ftype == FT_VIDEO) {
                icon_color = 0xEF5350; icon_letter = "V";
            } else {
                icon_color = 0xF0C800; icon_letter = "F";
            }

            // Highlight selected file
            bool selected = (win.sel_file_idx == line);
            if (selected) {
                gfx.fill_rect(x + 2, line_y - 1, list_w - 4, 17, C_ACCENT_LIGHT);
            }

            // Draw file icon
            gfx.fill_rounded_rect(x + 4, line_y + 2, 14, 14, 2, icon_color);
            gfx.draw_text(x + 6, line_y + 3, icon_letter, COLOR_WHITE, icon_color);

            // Draw filename (use clean name for display)
            gfx.draw_text_transparent(x + 24, line_y, clean_name,
                                       selected ? C_ACCENT : C_WIN_TEXT);

            line_y += 18;
            line++;
        }

        // ---- Preview pane (right side) ----
        gfx.fill_rect(preview_x, y, preview_w, list_h, 0xFAFAFA);

        if (win.sel_file_idx < 0 || win.sel_file_idx >= file_entry_count) {
            gfx.draw_text_transparent(preview_x + 8, y + 8, "Preview", C_WIN_TEXT_SEC);
            gfx.draw_text_transparent(preview_x + 8, y + 28, "(select a file to preview)", C_WIN_TEXT_SEC);
            return;
        }

        // Get selected file name
        char sel_name[64];
        extract_filename(file_entries[win.sel_file_idx], sel_name, sizeof(sel_name));
        FileType ftype = detect_file_type(file_entries[win.sel_file_idx]);

        int py = y + 8;

        // Preview header
        gfx.draw_text_transparent(preview_x + 8, py, "Preview: ", C_WIN_TEXT_SEC);
        gfx.draw_text_transparent(preview_x + 8 + 8*8, py, sel_name, C_ACCENT);
        py += 24;

        // File type label
        const char* type_label;
        switch (ftype) {
            case FT_DIR:   type_label = "Type: Directory"; break;
            case FT_TEXT:  type_label = "Type: Text File"; break;
            case FT_BMP:   type_label = "Type: Bitmap Image"; break;
            case FT_AUDIO: type_label = "Type: Audio File"; break;
            case FT_VIDEO: type_label = "Type: Video File"; break;
            default:       type_label = "Type: Unknown"; break;
        }
        gfx.draw_text_transparent(preview_x + 8, py, type_label, C_WIN_TEXT_SEC);
        py += 20;

        // Separator in preview
        gfx.draw_line(preview_x + 4, py, preview_x + preview_w - 4, py, C_WIN_BORDER);
        py += 8;

        // Content area
        int content_h = list_h - (py - y) - 8;

        if (ftype == FT_DIR) {
            // Directory preview - folder icon
            gfx.fill_rounded_rect(preview_x + 20, py + 10, 60, 45, 4, 0xFFD700);
            gfx.fill_rect(preview_x + 20, py + 10, 25, 8, 0xFFD700);
            gfx.draw_text_transparent(preview_x + 8, py + 65, "Directory contents", C_WIN_TEXT);
            gfx.draw_text_transparent(preview_x + 8, py + 83, "listed in file list ->", C_WIN_TEXT_SEC);
        }
        else if (ftype == FT_TEXT) {
            // Read and display text file content
            static uint8_t textbuf[4096];
            int bytes = -1;
            if (g_cb.read_file) {
                bytes = g_cb.read_file(fs_type, sel_name, textbuf, sizeof(textbuf) - 1);
            }
            if (bytes > 0) {
                textbuf[bytes] = 0;
                // Display text line by line
                char* tp = (char*)textbuf;
                int ty = py;
                int max_lines = content_h / 16;
                int tline = 0;
                while (*tp && tline < max_lines) {
                    // Extract one line
                    char tline_buf[81];
                    int ti = 0;
                    while (*tp && *tp != '\n' && *tp != '\r' && ti < 80) {
                        if (*tp >= 32 || *tp == '\t') tline_buf[ti++] = *tp;
                        tp++;
                    }
                    tline_buf[ti] = 0;
                    if (*tp == '\n') { tp++; if (*tp == '\r') tp++; }
                    else if (*tp == '\r') { tp++; if (*tp == '\n') tp++; }

                    gfx.draw_text_transparent(preview_x + 8, ty, tline_buf, C_WIN_TEXT);
                    ty += 16;
                    tline++;
                }
                // Show byte count
                char info[64];
                strcpy_(info, "(");
                uint_to_str(bytes, info + strlen_(info));
                strcat_safe(info, " bytes)", sizeof(info));
                gfx.draw_text_transparent(preview_x + 8, y + list_h - 18, info, C_WIN_TEXT_SEC);
            } else {
                gfx.draw_text_transparent(preview_x + 8, py, "Unable to read file.", C_MEM_BAD);
                gfx.draw_text_transparent(preview_x + 8, py + 18, "(File system may not support reading)", C_WIN_TEXT_SEC);
            }
        }
        else if (ftype == FT_BMP) {
            // Read BMP header and render
            static uint8_t bmpbuf[8192];
            int bytes = -1;
            if (g_cb.read_file) {
                bytes = g_cb.read_file(fs_type, sel_name, bmpbuf, sizeof(bmpbuf));
            }
            if (bytes >= 54 && bmpbuf[0] == 'B' && bmpbuf[1] == 'M') {
                // Parse BMP header
                uint32_t data_offset = *(uint32_t*)(bmpbuf + 10);
                uint32_t img_width = *(uint32_t*)(bmpbuf + 18);
                uint32_t img_height = *(uint32_t*)(bmpbuf + 22);
                uint16_t bpp = *(uint16_t*)(bmpbuf + 28);

                // Display info
                char info[64];
                strcpy_(info, "Size: ");
                uint_to_str(img_width, info + strlen_(info));
                strcat_safe(info, "x", sizeof(info));
                uint_to_str(img_height, info + strlen_(info));
                strcat_safe(info, " ", sizeof(info));
                uint_to_str(bpp, info + strlen_(info));
                strcat_safe(info, "bpp", sizeof(info));
                gfx.draw_text_transparent(preview_x + 8, py, info, C_WIN_TEXT_SEC);
                py += 20;

                // Render BMP if it fits in our buffer and is 24/32bpp
                if ((bpp == 24 || bpp == 32) && data_offset < bytes && img_width > 0 && img_height > 0) {
                    int row_size = ((bpp * img_width + 31) / 32) * 4;
                    int max_render_w = preview_w - 16;
                    int max_render_h = content_h - 30;

                    // Calculate scaled dimensions (keep aspect ratio)
                    int render_w = img_width;
                    int render_h = img_height;
                    if (render_w > max_render_w) {
                        render_h = render_h * max_render_w / render_w;
                        render_w = max_render_w;
                    }
                    if (render_h > max_render_h) {
                        render_w = render_w * max_render_h / render_h;
                        render_h = max_render_h;
                    }
                    if (render_w < 1) render_w = 1;
                    if (render_h < 1) render_h = 1;

                    // Draw border around preview area
                    gfx.draw_line(preview_x + 8, py, preview_x + 8 + render_w + 1, py, C_WIN_BORDER);
                    gfx.draw_line(preview_x + 8, py + render_h + 1, preview_x + 8 + render_w + 1, py + render_h + 1, C_WIN_BORDER);
                    gfx.draw_line(preview_x + 8, py, preview_x + 8, py + render_h + 1, C_WIN_BORDER);
                    gfx.draw_line(preview_x + 8 + render_w + 1, py, preview_x + 8 + render_w + 1, py + render_h + 1, C_WIN_BORDER);

                    // Render pixels (BMP is stored bottom-up)
                    for (int dy = 0; dy < render_h; dy++) {
                        int src_y = img_height - 1 - (dy * img_height / render_h);
                        for (int dx = 0; dx < render_w; dx++) {
                            int src_x = dx * img_width / render_w;
                            int offset = data_offset + src_y * row_size + src_x * (bpp / 8);
                            if (offset + 2 < bytes) {
                                uint8_t b = bmpbuf[offset];
                                uint8_t g = bmpbuf[offset + 1];
                                uint8_t r = bmpbuf[offset + 2];
                                Color c = (r << 16) | (g << 8) | b;
                                gfx.put_pixel(preview_x + 9 + dx, py + 1 + dy, c);
                            }
                        }
                    }
                } else {
                    gfx.draw_text_transparent(preview_x + 8, py, "Unsupported BMP format", C_MEM_WARN);
                    gfx.draw_text_transparent(preview_x + 8, py + 18, "(only 24/32bpp supported)", C_WIN_TEXT_SEC);
                }
            } else {
                gfx.draw_text_transparent(preview_x + 8, py, "Unable to read BMP file.", C_MEM_BAD);
            }
        }
        else if (ftype == FT_AUDIO) {
            // Audio file preview - draw music note icon and info
            // Draw music note (circle + stem)
            int cx = preview_x + 40;
            int cy = py + 30;
            gfx.fill_rounded_rect(cx, cy + 15, 18, 14, 4, 0xAB47BC);
            gfx.fill_rect(cx + 14, cy, 4, 20, 0xAB47BC);
            gfx.fill_rect(cx + 14, cy, 14, 4, 0xAB47BC);

            gfx.draw_text_transparent(preview_x + 8, py + 60, "Audio file", C_WIN_TEXT);
            gfx.draw_text_transparent(preview_x + 8, py + 78, "Cannot play audio in", C_WIN_TEXT_SEC);
            gfx.draw_text_transparent(preview_x + 8, py + 94, "this environment.", C_WIN_TEXT_SEC);

            // Show file size if available
            if (g_cb.read_file) {
                static uint8_t abuf[32];
                int ab = g_cb.read_file(fs_type, sel_name, abuf, sizeof(abuf));
                if (ab > 0) {
                    // Check for WAV header
                    if (ab >= 12 && abuf[0] == 'R' && abuf[1] == 'I' && abuf[2] == 'F' && abuf[3] == 'F') {
                        gfx.draw_text_transparent(preview_x + 8, py + 114, "Format: WAV", C_MEM_GOOD);
                    }
                }
            }
        }
        else if (ftype == FT_VIDEO) {
            // Video file preview - draw film icon and info
            int cx = preview_x + 30;
            int cy = py + 20;
            // Film strip body
            gfx.fill_rect(cx, cy, 50, 36, 0xEF5350);
            // Perforations (top and bottom)
            for (int i = 0; i < 5; i++) {
                gfx.fill_rect(cx + 3 + i * 10, cy + 2, 6, 4, 0xFAFAFA);
                gfx.fill_rect(cx + 3 + i * 10, cy + 30, 6, 4, 0xFAFAFA);
            }
            // Play triangle in center
            int px2 = cx + 20, py2 = cy + 13;
            for (int r = 0; r < 5; r++) {
                for (int c = 0; c <= r; c++) {
                    gfx.put_pixel(px2 + c, py2 + r, COLOR_WHITE);
                }
            }

            gfx.draw_text_transparent(preview_x + 8, py + 65, "Video file", C_WIN_TEXT);
            gfx.draw_text_transparent(preview_x + 8, py + 83, "Cannot play video in", C_WIN_TEXT_SEC);
            gfx.draw_text_transparent(preview_x + 8, py + 99, "this environment.", C_WIN_TEXT_SEC);
        }
        else {
            // Unknown file type
            gfx.fill_rounded_rect(preview_x + 20, py + 10, 50, 40, 4, 0xBDBDBD);
            gfx.draw_text_transparent(preview_x + 35, py + 22, "?", C_WIN_TEXT_SEC);

            gfx.draw_text_transparent(preview_x + 8, py + 60, "Unknown file type", C_WIN_TEXT_SEC);

            // Try to show file size by reading first bytes
            if (g_cb.read_file) {
                static uint8_t ubuf[256];
                int ub = g_cb.read_file(fs_type, sel_name, ubuf, sizeof(ubuf));
                if (ub >= 0) {
                    char info[64];
                    strcpy_(info, "Size: >= ");
                    uint_to_str(ub, info + strlen_(info));
                    strcat_safe(info, " bytes", sizeof(info));
                    gfx.draw_text_transparent(preview_x + 8, py + 80, info, C_WIN_TEXT_SEC);
                }
            }
        }
    }

    // ---- Task Manager ----
    void draw_task_manager(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];
        char buf[128];

        // ---- Search bar ----
        int search_h = 28;
        gfx.draw_search_bar(x, y, w - 180, search_h,
                            "Search name or PID",
                            win.tm_search, win.tm_search_len,
                            win.tm_search_focused);
        // Action buttons (right-aligned)
        int btn_w = 80;
        int btn_gap = 8;
        int bx = x + w - 180 + 8;
        draw_action_button(bx, y, btn_w, search_h, "End Task", false, win);
        bx += btn_w + btn_gap;
        draw_action_button(bx, y, btn_w, search_h, "Run New", false, win);
        y += search_h + 8;

        // ---- Summary bar (memory usage, CPU) ----
        int sum_h = 36;
        gfx.fill_rounded_rect(x, y, w, sum_h, 4, C_CARD_BG);
        gfx.draw_rounded_rect(x, y, w, sum_h, 4, C_CARD_BORDER);

        // Memory summary
        if (g_cb.get_total_mem_kb) {
            uint32_t total = g_cb.get_total_mem_kb();
            uint32_t total_p = g_cb.get_total_pages ? g_cb.get_total_pages() : 1;
            uint32_t used_p = g_cb.get_used_pages ? g_cb.get_used_pages() : 0;
            int pct = total_p > 0 ? (int)(used_p * 100 / total_p) : 0;

            // Memory label
            strcpy_(buf, "Memory: ");
            uint_to_str(pct, buf + strlen_(buf));
            strcat_safe(buf, "%", sizeof(buf));
            gfx.draw_text_transparent(x + 8, y + 4, buf, C_WIN_TEXT);

            // Mini progress bar
            int bar_w = 120;
            int bar_x = x + 8 + strlen_(buf) * FONT_W + 8;
            gfx.draw_progress(bar_x, y + 8, bar_w, 8, pct,
                              pct < 50 ? C_MEM_GOOD : (pct < 80 ? C_MEM_WARN : C_MEM_BAD));

            // RAM total
            strcpy_(buf, "RAM: ");
            uint_to_str(total / 1024, buf + strlen_(buf));
            strcat_safe(buf, " MB", sizeof(buf));
            gfx.draw_text_transparent(bar_x + bar_w + 8, y + 4, buf, C_WIN_TEXT_SEC);
        }

        // Process count
        {
            int proc_count = 0;
            for (int i = 0; i < window_count; i++) {
                if (windows[i].visible) proc_count++;
            }
            proc_count += 3; // Shell + Network + AI
            strcpy_(buf, "Processes: ");
            int_to_str(proc_count, buf + strlen_(buf));
            gfx.draw_text_transparent(x + w - strlen_(buf) * FONT_W - 12, y + 4, buf, C_WIN_TEXT_SEC);
        }
        y += sum_h + 8;

        // ---- Process table ----
        // Table headers
        const char* headers[] = {"Name", "Status", "CPU", "Memory", "Disk"};
        int col_w[] = {w - 280, 70, 60, 80, 60};
        int ncols = 5;
        int header_h = 24;

        gfx.draw_table_header(x, y, w, header_h, headers, col_w, ncols);
        y += header_h;

        // Process list
        int row_h = 20;
        int max_rows = (h - (y - (win.content_y() + 8))) / row_h;
        if (max_rows < 1) max_rows = 1;

        int row_idx = 0;

        // Build process list
        struct ProcEntry { const char* name; const char* status; int cpu; uint32_t mem_kb; int disk; Color icon_color; char letter; };
        ProcEntry procs[16];
        int nproc = 0;

        // Add window processes
        for (int i = 0; i < window_count && nproc < 16; i++) {
            if (!windows[i].visible) continue;
            procs[nproc].name = windows[i].title;
            procs[nproc].status = "Running";
            procs[nproc].cpu = i == active_window ? 5 : 1;
            procs[nproc].mem_kb = 50 + i * 30;
            procs[nproc].disk = 0;
            procs[nproc].icon_color = C_ACCENT;
            procs[nproc].letter = windows[i].title[0];
            nproc++;
        }

        // System processes
        if (nproc < 16) {
            procs[nproc].name = "NexOS Shell"; procs[nproc].status = "Running";
            procs[nproc].cpu = 2; procs[nproc].mem_kb = 120; procs[nproc].disk = 0;
            procs[nproc].icon_color = C_CAT_SYSTEM; procs[nproc].letter = 'S'; nproc++;
        }
        if (nproc < 16) {
            procs[nproc].name = "NE2000 Network"; procs[nproc].status = "Running";
            procs[nproc].cpu = 0; procs[nproc].mem_kb = 15; procs[nproc].disk = 0;
            procs[nproc].icon_color = C_CAT_NETWORK; procs[nproc].letter = 'N'; nproc++;
        }
        if (nproc < 16) {
            procs[nproc].name = "AI Engine"; procs[nproc].status = "Running";
            procs[nproc].cpu = 0; procs[nproc].mem_kb = 85; procs[nproc].disk = 0;
            procs[nproc].icon_color = C_CAT_PROGRAMS; procs[nproc].letter = 'A'; nproc++;
        }
        if (nproc < 16) {
            procs[nproc].name = "Desktop Window Mgr"; procs[nproc].status = "Running";
            procs[nproc].cpu = 1; procs[nproc].mem_kb = 95; procs[nproc].disk = 0;
            procs[nproc].icon_color = C_CAT_APPEARANCE; procs[nproc].letter = 'D'; nproc++;
        }

        // Draw process rows
        for (int i = 0; i < nproc && row_idx < max_rows; i++) {
            int ry = y + row_idx * row_h;
            bool hover = (mouse_x >= x && mouse_x < x + w &&
                          mouse_y >= ry && mouse_y < ry + row_h);
            bool selected = (win.tm_selected_proc == i);

            // Row background
            if (selected) {
                gfx.fill_rect(x, ry, w, row_h, C_TM_ROW_SELECTED);
            } else if (hover) {
                gfx.fill_rect(x, ry, w, row_h, C_TM_ROW_HOVER);
            }

            // Alternating row color
            if (!hover && !selected && (i % 2 == 1)) {
                gfx.fill_rect(x, ry, w, row_h, 0xF8F9FA);
            }

            // Icon (small circle)
            int icx = x + 12;
            int icy = ry + row_h / 2;
            gfx.fill_circle(icx, icy, 7, procs[i].icon_color);
            gfx.draw_char(icx - FONT_W/2, icy - FONT_H/2, procs[i].letter,
                          COLOR_WHITE, procs[i].icon_color);

            // Name
            gfx.draw_text_transparent(x + 26, ry + 2, procs[i].name, C_WIN_TEXT);

            // Status (column 2)
            int cx = x + col_w[0] + 4;
            gfx.draw_text_transparent(cx, ry + 2, procs[i].status, C_MEM_GOOD);

            // CPU (column 3 - right aligned)
            cx = x + col_w[0] + col_w[1];
            strcpy_(buf, "");
            int_to_str(procs[i].cpu, buf);
            strcat_safe(buf, "%", sizeof(buf));
            int cpu_tw = strlen_(buf) * FONT_W;
            Color cpu_color = procs[i].cpu > 10 ? C_MEM_BAD : (procs[i].cpu > 5 ? C_MEM_WARN : C_WIN_TEXT);
            gfx.draw_text_transparent(cx + col_w[2] - cpu_tw - 4, ry + 2, buf, cpu_color);

            // Memory (column 4 - right aligned)
            cx = x + col_w[0] + col_w[1] + col_w[2];
            strcpy_(buf, "");
            uint_to_str(procs[i].mem_kb, buf);
            strcat_safe(buf, " MB", sizeof(buf));
            int mem_tw = strlen_(buf) * FONT_W;
            gfx.draw_text_transparent(cx + col_w[3] - mem_tw - 4, ry + 2, buf, C_WIN_TEXT);

            // Disk (column 5 - right aligned)
            cx = x + col_w[0] + col_w[1] + col_w[2] + col_w[3];
            strcpy_(buf, "");
            int_to_str(procs[i].disk, buf);
            strcat_safe(buf, " MB/s", sizeof(buf));
            int disk_tw = strlen_(buf) * FONT_W;
            gfx.draw_text_transparent(cx + col_w[4] - disk_tw - 4, ry + 2, buf, C_WIN_TEXT_SEC);

            row_idx++;
        }

        // Separator line below table
        gfx.draw_line(x, y + row_idx * row_h, x + w, y + row_idx * row_h, C_WIN_BORDER);

        // Heap stats at the bottom
        y += row_idx * row_h + 8;
        if (g_cb.get_heap_alloc_bytes && y < win.y + win.h - 40) {
            uint32_t ha = g_cb.get_heap_alloc_bytes();
            uint32_t hf = g_cb.get_heap_free_bytes();
            strcpy_(buf, "Heap: ");
            uint_to_str(ha / 1024, buf + strlen_(buf));
            strcat_safe(buf, "KB / ", sizeof(buf));
            uint_to_str(hf / 1024, buf + strlen_(buf));
            strcat_safe(buf, "KB free", sizeof(buf));
            gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT_SEC);
        }
    }

    // ---- Memory Optimizer ----
    void draw_mem_optimizer(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];
        gfx.draw_text_transparent(x, y, "Memory Optimizer", C_WIN_TEXT);
        y += 24;

        char buf[128];

        // Before optimization
        gfx.draw_text_transparent(x, y, "Current State", C_ACCENT);
        y += 20;
        gfx.draw_line(x, y, x + w, y, C_WIN_BORDER);
        y += 8;

        if (g_cb.get_total_pages) {
            uint32_t used = g_cb.get_used_pages();
            uint32_t total = g_cb.get_total_pages();
            int pct = total > 0 ? (int)(used * 100 / total) : 0;

            strcpy_(buf, "Page Usage: ");
            uint_to_str(pct, buf + strlen_(buf));
            strcat_safe(buf, "%  (", sizeof(buf));
            uint_to_str(used, buf + strlen_(buf));
            strcat_safe(buf, "/", sizeof(buf));
            uint_to_str(total, buf + strlen_(buf));
            strcat_safe(buf, " pages)", sizeof(buf));
            gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 22;

            gfx.draw_progress(x, y, w, 16, pct, pct < 50 ? C_MEM_GOOD : (pct < 80 ? C_MEM_WARN : C_MEM_BAD));
            y += 26;
        }

        if (g_cb.get_heap_alloc_bytes) {
            uint32_t ha = g_cb.get_heap_alloc_bytes();
            uint32_t hf = g_cb.get_heap_free_bytes();
            uint32_t total_h = ha + hf;
            int hpct = total_h > 0 ? (int)(ha * 100 / total_h) : 0;

            strcpy_(buf, "Heap Usage: ");
            uint_to_str(hpct, buf + strlen_(buf));
            strcat_safe(buf, "%  (", sizeof(buf));
            uint_to_str(ha/1024, buf + strlen_(buf));
            strcat_safe(buf, "KB/", sizeof(buf));
            uint_to_str(total_h/1024, buf + strlen_(buf));
            strcat_safe(buf, "KB)", sizeof(buf));
            gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 22;

            gfx.draw_progress(x, y, w, 16, hpct, C_ACCENT);
            y += 26;
        }

        // Optimize button
        draw_action_button(x, y, 160, 32, "Optimize Now", false, win);
        y += 42;

        // Results
        if (win.mem_optimized) {
            gfx.draw_text_transparent(x, y, "Optimization Complete!", C_MEM_GOOD);
            y += 22;

            if (win.mem_before_kb > 0) {
                strcpy_(buf, "Before: ");
                uint_to_str(win.mem_before_kb, buf + strlen_(buf));
                strcat_safe(buf, " KB used", sizeof(buf));
                gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT_SEC); y += 18;

                strcpy_(buf, "After:  ");
                uint_to_str(win.mem_after_kb, buf + strlen_(buf));
                strcat_safe(buf, " KB used", sizeof(buf));
                gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT_SEC); y += 18;

                int saved = (int)win.mem_before_kb - (int)win.mem_after_kb;
                strcpy_(buf, "Saved:  ");
                if (saved > 0) uint_to_str((uint32_t)saved, buf + strlen_(buf));
                else strcpy_(buf + strlen_(buf), "0");
                strcat_safe(buf, " KB", sizeof(buf));
                gfx.draw_text_transparent(x, y, buf, C_MEM_GOOD);
            }
        }
    }

    // ---- Calculator ----
    void draw_calculator(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];

        // Display
        gfx.fill_rounded_rect(x, y, w, 36, 6, 0x1A1A1A);
        char disp[24];
        int_to_str(win.calc_display, disp);
        int dw = strlen_(disp) * FONT_W;
        gfx.draw_text(x + w - dw - 8, y + 10, disp, COLOR_WHITE, 0x1A1A1A);
        y += 44;

        // Buttons: 7 8 9 +  4 5 6 -  1 2 3 *  0 = C /
        const char* labels[] = {"7","8","9","+","4","5","6","-","1","2","3","*","0","=","C","/"};
        int bw = (w - 20) / 4;
        int bh = 32;
        for (int i = 0; i < 16; i++) {
            int bx = x + (i % 4) * (bw + 4);
            int by = y + (i / 4) * (bh + 4);
            const char* lbl = labels[i];
            bool is_op = (lbl[0]=='+'||lbl[0]=='-'||lbl[0]=='*'||lbl[0]=='/'||lbl[0]=='=');
            bool is_clear = (lbl[0]=='C');
            Color bg = is_clear ? C_CLOSE_HOVER : (is_op ? C_ACCENT : C_BTN_BG);
            Color fg = is_clear ? COLOR_WHITE : (is_op ? COLOR_WHITE : C_BTN_TEXT);
            // Hover
            if (mouse_x >= bx && mouse_x < bx + bw && mouse_y >= by && mouse_y < by + bh) {
                bg = is_clear ? 0xFF3344 : (is_op ? C_ACCENT_DARK : C_BTN_HOVER);
            }
            gfx.fill_rounded_rect(bx, by, bw, bh, 4, bg);
            int lx = bx + (bw - FONT_W) / 2;
            int ly = by + (bh - FONT_H) / 2;
            gfx.draw_text(lx, ly, lbl, fg, bg);
        }
    }

    // ---- Terminal ----
    void draw_terminal(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];
        // Terminal background (black)
        gfx.fill_rect(win.x + 1, win.content_y(), win.w - 2, win.content_h(), 0x0C0C0C);

        // Terminal output (UTF-8 aware)
        char* p = win.term_buf;
        int ty = y;
        int max_lines = h / 16;
        int line = 0;
        while (*p && line < max_lines) {
            char t[160];
            int ti = 0;
            while (*p && *p != '\n' && ti < 159) t[ti++] = *p++;
            t[ti] = 0;
            if (*p == '\n') p++;
            gfx.draw_text_utf8_transparent(x, ty, t, 0xCCCCCC);
            ty += 16;
            line++;
        }

        // Input line
        gfx.draw_text_transparent(x, ty, "> ", 0x00FF66);
        gfx.draw_text_utf8_transparent(x + 16, ty, win.term_input, 0xCCCCCC);
        // Cursor (account for CJK width)
        int cw = utf8_display_width(win.term_input);
        int cx = x + 16 + cw;
        gfx.fill_rect(cx, ty, 8, 16, 0xCCCCCC);

        // ---- IME candidate bar (drawn just above the input line, inside window) ----
        if (g_ime_active && g_ime_cand_count > 0) {
            int cy = ty - 20;     // ty = input-line y; place bar above it
            gfx.fill_rect(x, cy, w - 2, 24, 0x1E1E1E);
            // [pinyin]
            gfx.draw_text_transparent(x + 4, cy + 4, "[", 0x8888FF);
            gfx.draw_text_transparent(x + 12, cy + 4, g_ime_py, 0x8888FF);
            int ccx = x + 12 + g_ime_len * 8 + 14;
            for (int i = 0; i < g_ime_cand_count; i++) {
                char num[2]; num[0] = '1' + i; num[1] = 0;
                gfx.draw_text_transparent(ccx, cy + 4, num, 0xFFCC00);
                gfx.draw_cjk_transparent(ccx + 10, cy + 3, (uint32_t)g_ime_cands[i], 0xFFFFFF);
                ccx += 34;
            }
        }
    }

    // ---- About ----
    void draw_about(int id, int x, int y, int w, int h) {
        gfx.draw_text_transparent(x, y, "About NexOS", C_WIN_TEXT);
        y += 28;

        gfx.draw_text_transparent(x, y, "NexOS v2.0", C_ACCENT); y += 20;
        gfx.draw_text_transparent(x, y, "Win11-style GUI Desktop", C_WIN_TEXT_SEC); y += 20;

        gfx.draw_text_transparent(x, y, "Features:", C_WIN_TEXT); y += 20;
        gfx.draw_text_transparent(x, y, "  - 32-bit protected-mode kernel", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - VBE framebuffer GUI", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - MKFS/SFS/FAT32 support", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - AI engine (Markov+GPT)", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - NE2000 networking", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - Memory optimization", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - Control Panel", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - File Explorer", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - Task Manager", C_WIN_TEXT_SEC); y += 16;
        gfx.draw_text_transparent(x, y, "  - Calculator", C_WIN_TEXT_SEC); y += 16;

        y += 8;
        if (g_cb.is_64bit) {
            char buf[64];
            strcpy_(buf, "Mode: ");
            strcat_safe(buf, g_cb.is_64bit() ? "64-bit Long Mode" : "32-bit Protected Mode", sizeof(buf));
            gfx.draw_text_transparent(x, y, buf, C_MEM_GOOD);
            y += 16;
        }
    }


        void draw_browser(int id, int x, int y, int w, int h) {
        browser::draw(gfx, id, windows[id], x, y, w, h, mouse_x, mouse_y);
    }

    // ---- Shared IME candidate bar (browser / managed text boxes) ----
    // Drawn at a fixed screen position because the C# text box caret
    // location is not known to the native layer.
    void draw_ime_bar(int bx, int by) {
        if (!g_ime_active || g_ime_cand_count <= 0) return;
        const int bw = gfx.width - 16;
        gfx.fill_rounded_rect(bx, by, bw, 28, 6, 0x1E1E1E);
        gfx.draw_rounded_rect(bx, by, bw, 28, 6, 0x3A3A3A);
        // [pinyin]
        gfx.draw_text_transparent(bx + 8, by + 6, "[", 0x8888FF);
        gfx.draw_text_transparent(bx + 16, by + 6, g_ime_py, 0x8888FF);
        int ccx = bx + 16 + g_ime_len * 8 + 14;
        for (int i = 0; i < g_ime_cand_count; i++) {
            char num[2]; num[0] = '1' + i; num[1] = 0;
            gfx.draw_text_transparent(ccx, by + 6, num, 0xFFCC00);
            gfx.draw_cjk_transparent(ccx + 10, by + 5, (uint32_t)g_ime_cands[i], 0xFFFFFF);
            ccx += 34;
        }
    }


    // ---- Helper: draw an action button ----
    void draw_action_button(int x, int y, int w, int h, const char* label,
                            bool pressed, Win11Window& win) {
        bool hover = (mouse_x >= x && mouse_x < x + w &&
                     mouse_y >= y && mouse_y < y + h);
        Color bg = pressed ? C_BTN_PRESSED : (hover ? C_BTN_HOVER : C_BTN_BG);
        gfx.fill_rounded_rect(x, y, w, h, 4, bg);
        gfx.draw_rounded_rect(x, y, w, h, 4, C_BTN_BORDER);
        int lw = strlen_(label) * FONT_W;
        gfx.draw_text(x + (w - lw) / 2, y + (h - FONT_H) / 2, label, C_BTN_TEXT, bg);
    }

    // Bit i set == a window of managed Kind i is on screen.  The C#
    // taskbar turns those into running-app indicators.
    uint32_t running_mask() const {
        uint32_t m = 0;
        for (int i = 0; i < window_count; i++) {
            if (!windows[i].visible) continue;
            int k = managed_kind_for(windows[i].launch_kind);
            if (k >= 0 && k < 32) m |= (1u << k);
        }
        return m;
    }

    // ---- Animation (Phase 2 visual polish) ----
    // Duration, open/close slide distance, and the nominal frame step
    // (~60 fps).  ANIM_STALL_MAX is the deadlock escape hatch: if the TSC
    // pacing never lets a frame through, force one anyway rather than
    // stranding a window mid-animation (which would wedge every click,
    // because close/minimise are no-ops while anim_state != 0).
    enum { ANIM_MS = 160, ANIM_SLIDE = 12, ANIM_STEP_MS = 16,
           ANIM_STALL_MAX = 100000 };

    static uint32_t rdtsc32() {
        uint32_t lo;
        __asm__ volatile("rdtsc" : "=a"(lo) : : "edx");
        return lo;
    }

    // Derive TSC ticks per millisecond from PIT channel 2 (mode 0, one-shot).
    // Channel 2 drives the PC speaker and is otherwise unused, so borrowing
    // it is safe.  The result is only a *pacing hint*: on any doubt we leave
    // anim_tsc_per_ms at 0 and animate_frame() paces by main-loop pass
    // instead.  A wrong value here used to strand windows mid-animation for
    // seconds, so the sanity check is deliberately strict.
    void calibrate_tsc() {
        anim_tsc_per_ms = 0;
        const uint16_t count = 11932;        // 1.193182 MHz * 10 ms
        uint8_t pc = inb(0x61);
        outb(0x61, (uint8_t)(pc & ~0x03));   // speaker off, gate LOW (stop)
        outb(0x43, 0xB0);                    // ch2, lo/hi byte, mode 0, binary
        outb(0x42, (uint8_t)(count & 0xFF));
        outb(0x42, (uint8_t)(count >> 8));
        outb(0x61, (uint8_t)((pc & ~0x02) | 0x01));   // gate HIGH -> counting
        uint32_t t0 = rdtsc32();
        uint32_t guard = 0;
        bool done = false;
        // Bit 5 of port 0x61 mirrors channel 2's OUT line: low while
        // counting, high at terminal count.
        while (guard++ < 2000000u) {
            if (inb(0x61) & 0x20) { done = true; break; }
        }
        uint32_t t1 = rdtsc32();
        outb(0x61, pc);                      // restore original state
        if (!done) {
            serial_puts("[ANIM] PIT ch2 never fired; pacing by frame\n");
            return;
        }
        uint32_t per_ms = (t1 - t0) / 10;
        char b[16];
        // Accept 100 MHz .. 10 GHz.  Anything outside means the PIT lied or
        // the 32-bit TSC wrapped during the wait.
        if (per_ms < 100000u || per_ms > 10000000u) {
            uint_to_str(per_ms, b);
            serial_puts("[ANIM] TSC calibration rejected ("); serial_puts(b);
            serial_puts(" tsc/ms); pacing by frame\n");
            return;
        }
        anim_tsc_per_ms = (int)per_ms;
        anim_ms_scale   = 0xFFFFFFFFu / per_ms;
        uint_to_str(per_ms, b);
        serial_puts("[ANIM] TSC "); serial_puts(b); serial_puts(" ticks/ms\n");
    }

    bool any_animating() {
        for (int i = 0; i < window_count; i++)
            if (windows[i].anim_state != 0) return true;
        return false;
    }

    void finish_close(int id) {
        if (windows[id].app == APP_WIN32 && windows[id].w32_index >= 0) {
            win32_window_close(windows[id].w32_index);
            windows[id].w32_index = -1;
        }
        if (windows[id].app == APP_MANAGED && windows[id].managed_app >= 0) {
            mforms_close(windows[id].managed_app);
            windows[id].managed_app = -1;
        }
        windows[id].visible = false;
        if (active_window == id) {
            active_window = -1;
            for (int i = window_count - 1; i >= 0; i--)
                if (windows[i].visible) { windows[i].active = true; active_window = i; break; }
        }
    }

    void step_animations(int dt) {
        for (int i = 0; i < window_count; i++) {
            Win11Window& w = windows[i];
            if (w.anim_state == 0) continue;
            w.anim_p += (dt * 1000) / ANIM_MS;
            if (w.anim_p >= 1000) {
                w.anim_p = 1000;
                if (w.anim_state == 1)       w.anim_state = 0;              // opening done
                else if (w.anim_state == 2) { finish_close(i); w.anim_state = 0; } // closing done
                else if (w.anim_state == 3) { // minimizing done
                    w.minimized = true; w.anim_state = 0;
                    if (active_window == i) {
                        active_window = -1;
                        for (int k = window_count - 1; k >= 0; k--)
                            if (windows[k].visible && !windows[k].minimized) {
                                windows[k].active = true; active_window = k; break;
                            }
                    }
                } // minimizing done
                else if (w.anim_state == 4)  w.anim_state = 0;              // restoring done
            }
        }
    }

    void animate_frame() {
        bool now = any_animating();
        // anim_active_prev makes sure the frame that *ends* an animation
        // (window fully open / gone) is still painted once.
        if (!now && !anim_active_prev) return;

        int dt;
        if (anim_tsc_per_ms > 0) {
            uint32_t t  = rdtsc32();
            uint32_t el = t - last_anim_tsc;   // wraps correctly modulo 2^32
            dt = (int)(el / (uint32_t)anim_tsc_per_ms);
            if (now && dt < ANIM_STEP_MS) {
                // Throttle to ~60 fps -- but never spin here forever.
                if (++anim_stall < ANIM_STALL_MAX) return;
                dt = ANIM_STEP_MS;
            }
            anim_stall = 0;
            last_anim_tsc = t;
        } else {
            dt = ANIM_STEP_MS;                 // no usable clock: one step per pass
        }
        if (dt < 1)  dt = 1;
        if (dt > 64) dt = 64;                  // don't teleport after a long stall
        step_animations(dt);
        render_all();
        anim_active_prev = any_animating();
    }

    void draw_window_animated(int id) {
        Win11Window& w = windows[id];
        int rx = w.x, ry = w.y, rw = w.w, rh = w.h, alpha = 255;
        int ty = gfx.height - 48; // top of taskbar
        if (w.anim_state == 1 || w.anim_state == 2) {
            int a = (w.anim_state == 1) ? w.anim_p : (1000 - w.anim_p);
            alpha = (a * 255) / 1000;
            ry = w.y + ((1000 - a) * ANIM_SLIDE) / 1000;
        } else if (w.anim_state == 3) { // minimize: collapse toward taskbar
            int p = w.anim_p;
            ry = w.y + ((ty - w.y) * p) / 1000;
            rh = w.h - (w.h * 900 * p) / 10000;
            rw = w.w - (w.w * 700 * p) / 10000;
            rx = w.x + (w.w - rw) / 2;
            alpha = 255 - (p * 180) / 1000;
        } else if (w.anim_state == 4) { // restore: expand from taskbar
            int p = w.anim_p;
            ry = ty + ((w.y - ty) * p) / 1000;
            rh = w.h - (w.h * 900 * (1000 - p)) / 10000;
            rw = w.w - (w.w * 700 * (1000 - p)) / 10000;
            rx = w.x + (w.w - rw) / 2;
            alpha = 75 + (p * 180) / 1000;
        }
        draw_window(id, rx, ry, rw, rh, alpha);
    }

    // ---- Win32 TrackPopupMenu rendering / hit-testing ----------------
    // The active menu's screen rect.  Shared by draw and hit so they can
    // never disagree.  Returns false when no menu session is active.
    bool w32_popup_rect(int* x, int* y, int* w, int* h) {
        int px, py; uint32_t hw;
        if (!win32_menu_active(&px, &py, &hw)) return false;
        const int item_h = 28, pad_y = 6, pad_x = 14;
        int n = win32_menu_item_count();
        if (n <= 0) return false;
        int mw = 150;
        for (int i = 0; i < n; i++) {
            if (win32_menu_item_flags(i) & 0x800) continue;   // separator
            int len = 0;
            const char* s = win32_menu_item_text(i);
            while (s && s[len]) len++;
            int tw = len * 8 + 2 * pad_x;
            if (tw > mw) mw = tw;
        }
        int mh = n * item_h + pad_y * 2;
        if (px + mw > gfx.width)  px = gfx.width - mw - 4;
        if (px < 4) px = 4;
        if (py + mh > gfx.height) py = gfx.height - mh - 4;
        if (py < 4) py = 4;
        if (x) *x = px; if (y) *y = py; if (w) *w = mw; if (h) *h = mh;
        return true;
    }

    // Paint the active Win32 popup menu over everything (taskbar included),
    // in the same flat style as the managed context menus.
    void draw_win32_popup() {
        int px, py, pw, ph;
        if (!w32_popup_rect(&px, &py, &pw, &ph)) return;
        const int item_h = 28, pad_y = 6, pad_x = 14;
        int n = win32_menu_item_count();

        gfx.fill_rounded_rect(px, py, pw, ph, 8, 0xF7F9FC);
        gfx.draw_rounded_rect(px, py, pw, ph, 8, 0xD5DDE8);

        int hy = -1;
        if (mouse_x >= px && mouse_x < px + pw && mouse_y >= py && mouse_y < py + ph)
            hy = (mouse_y - py - pad_y) / item_h;

        for (int i = 0; i < n; i++) {
            int iy = py + pad_y + i * item_h;
            int fl = win32_menu_item_flags(i);
            if (fl & 0x800) {   // separator
                gfx.draw_line(px + 10, iy + item_h / 2, px + pw - 10, iy + item_h / 2, 0xD5DDE8);
                continue;
            }
            if (i == hy && !(fl & 0x3))
                gfx.fill_rounded_rect(px + 4, iy + 1, pw - 8, item_h - 2, 4, 0xE7EEF8);
            uint32_t col = (fl & 0x3) ? 0x909090 : 0x1B1B1B;   // grayed/disabled -> faint
            gfx.draw_text_transparent(px + pad_x, iy + (item_h - 16) / 2,
                                      win32_menu_item_text(i), col);
            if (fl & 0x8)   // checked: small dot at the left edge
                gfx.fill_rounded_rect(px + 5, iy + item_h / 2 - 3, 6, 6, 2, col);
        }
    }

    // Which menu item is under (mx,my), or -1 for none / -2 outside.
    int w32_popup_hit(int mx, int my) {
        int px, py, pw, ph;
        if (!w32_popup_rect(&px, &py, &pw, &ph)) return -1;
        if (mx < px || mx >= px + pw || my < py || my >= py + ph) return -2;
        const int item_h = 28, pad_y = 6;
        int idx = (my - py - pad_y) / item_h;
        int n = win32_menu_item_count();
        if (idx < 0 || idx >= n) return -2;
        if (win32_menu_item_flags(idx) & 0x800) return -2;   // separator
        if (win32_menu_item_flags(idx) & 0x3)  return -2;    // disabled
        return idx;
    }

    // ---- Rendering ----
    void render_all() {
        if (!gfx.initialized) return;
        if (!gui_mode) return;   // never paint after gui_exit() (e.g. "Terminal" shortcut)
        frame_counter++;
        serial_puts("[GUI] render_all begin\n");

        // The managed (C#) Win11 shell paints in two layers so windows
        // land between the wallpaper and the taskbar, exactly as they do
        // on Windows.  Layer 2 goes in after the window loop below.
        const bool managed_desk = mforms_has_desktop() != 0;
        if (managed_desk) {
            mforms_set_mouse(mouse_x, mouse_y);
            mforms_set_running(running_mask());
            mforms_paint_desktop(gfx.width, gfx.height);
        } else {
            draw_wallpaper();
            draw_topbar();
            draw_portal_desktop();
        }

        // Draw windows with proper z-order:
        //  normal inactive -> normal active -> floating inactive -> floating active
        for (int i = 0; i < window_count; i++)
            if (windows[i].visible && !windows[i].minimized && !windows[i].floating && !windows[i].active) draw_window_animated(i);
        for (int i = 0; i < window_count; i++)
            if (windows[i].visible && !windows[i].minimized && !windows[i].floating && windows[i].active) draw_window_animated(i);
        for (int i = 0; i < window_count; i++)
            if (windows[i].visible && !windows[i].minimized && windows[i].floating && !windows[i].active) draw_window_animated(i);
        for (int i = 0; i < window_count; i++)
            if (windows[i].visible && !windows[i].minimized && windows[i].floating && windows[i].active) draw_window_animated(i);

        // Layer 2 of the managed shell: taskbar + Start menu, on top of
        // every window.  The native start menu only exists as a fallback.
        if (managed_desk) mforms_paint_overlay(gfx.width, gfx.height);
        else if (start_menu_open) draw_start_menu();

        // A Win32 TrackPopupMenu sits above everything, taskbar included.
        draw_win32_popup();

        // Screen-level IME candidate bar for browser URL / managed text boxes
        // (the terminal draws its own bar inside its window).
        if (g_ime_active && g_ime_cand_count > 0 && active_window >= 0 &&
            active_window < window_count) {
            int ak = windows[active_window].app;
            if (ak == APP_BROWSER || ak == APP_MANAGED)
                draw_ime_bar(8, TOPBAR_H + 4);
        }

        // Top-right language indicator: 中 = Chinese IME, EN = English.
        // Drawn here so it overlays the managed C# desktop wallpaper/taskbar.
        {
            const char* lang = g_ime_cn ? "中" : "EN";
            int lw = g_ime_cn ? 16 : (2 * FONT_W);
            int lx = gfx.width - 8 - lw;
            Color fg = g_ime_cn ? 0xFFCC00 : C_TOPBAR_TEXT;
            gfx.fill_rect(lx - 4, 0, lw + 8, TOPBAR_H, C_TOPBAR_BG);
            gfx.draw_text_utf8(lx, 8, lang, fg, C_TOPBAR_BG);
        }

        // Redraw cursor on top without hide/show flicker
        // Invalidate saved background so it's re-captured from the freshly-drawn screen
        if (cursor.visible) {
            cursor.saved_valid = false;
            cursor.save_bg(gfx);
            cursor.draw(gfx);
        }

        // Flip backbuffer to screen atomically
        gfx.present();
        serial_puts("[GUI] render_all end\n");
    }

    void enter_gui() {
        if (!gfx.initialized) { serial_puts("[GUI] No VBE\n"); return; }
        gfx.enable_vbe_mode();  // Sets BGA mode (emulator) or uses BIOS-set mode (real HW)
        gfx.force_clear_lfb();  // wipe bootuefi/OVMF residue
        serial_puts("[GUI] LFB cleared\n");
        gui_mode = true;
        calibrate_tsc();
        last_anim_tsc = rdtsc32();
        cursor.visible = true;       // Make cursor visible on entry
        cursor.saved_valid = false;  // Force background re-capture
        ime_reset();
        serial_puts("[GUI] IME reset done\n");
        render_all();   // draws everything + cursor + presents to screen
        // Auto-open a Terminal for immediate IME testing
        // (disabled while debugging Win32 window rendering)
        // if (window_count == 0) launch_app(APP_TERMINAL);
        if (g_startup_app_id >= 0) {
            launch_app((AppType)g_startup_app_id);
            g_startup_app_id = -1;
            // A startup app may have exited the GUI (the "Terminal" shortcut
            // drops back to the text shell); don't repaint or log "entered"
            // in that case.
            if (gui_mode) { render_all(); serial_puts("[GUI] Entered GUI mode\n"); }
            return;
        }
        // Reopen the previous session (apps left running when the GUI was
        // exited) if a saved one exists.  The user can clear it with the
        // `session` shell command.
        if (gui_session_restore() > 0) render_all();
        serial_puts("[GUI] Entered GUI mode\n");
    }

    // ---- Event handling ----
    void update_clock() {
        if (g_cb.get_time) {
            g_cb.get_time(&clock_h, &clock_m, &clock_s);
        }
    }

    void handle_mouse_move(int dx, int dy) {
        if (!gui_mode) return;
        int ox = cursor.x, oy = cursor.y;
        cursor.move(gfx, dx, dy);
        mouse_x = cursor.x;
        mouse_y = cursor.y;

        if (drag_window >= 0 && drag_window < window_count) {
            windows[drag_window].x = mouse_x - drag_off_x;
            windows[drag_window].y = mouse_y - drag_off_y;
            // Keep title bar below top bar
            if (windows[drag_window].y < TOPBAR_H)
                windows[drag_window].y = TOPBAR_H;
            drag_counter++;
            if (drag_counter >= 3) {
                drag_counter = 0;
                render_all();
            }
        } else {
            // Just move the cursor, no topbar redraw.
            // Flip only the union of the old and new cursor boxes to avoid
            // a full-screen tear and the per-move full blit cost.
            if (cursor.visible) {
                int nx = cursor.x, ny = cursor.y;
                int minx = ox < nx ? ox : nx;
                int miny = oy < ny ? oy : ny;
                int maxx = (ox > nx ? ox : nx) + MouseCursor::CURSOR_SIZE;
                int maxy = (oy > ny ? oy : ny) + MouseCursor::CURSOR_SIZE;
                gfx.present_rect(minx, miny, maxx - minx, maxy - miny);
            } else {
                gfx.present();
            }
        }
    }

    void toggle_fullscreen(int i) {
        Win11Window& win = windows[i];
        if (win.fullscreen) {
            win.x = win.restore_x; win.y = win.restore_y;
            win.w = win.restore_w; win.h = win.restore_h;
            win.fullscreen = false;
        } else {
            win.restore_x = win.x; win.restore_y = win.y;
            win.restore_w = win.w; win.restore_h = win.h;
            win.x = 0; win.y = 0;
            win.w = gfx.width; win.h = gfx.height;
            win.fullscreen = true;
        }
        for (int j = 0; j < window_count; j++) windows[j].active = false;
        win.active = true; active_window = i;
    }

    // Returns true if the click was consumed by window i (title button / drag / content)
    bool window_click(int i) {
        Win11Window& win = windows[i];
        if (!win.visible || win.minimized) return false;
        // Title bar region
        if (win.title_contains(mouse_x, mouse_y)) {
            for (int b = 0; b < 4; b++) {
                int bx, by; win.title_btn_rect(b, bx, by);
                if (mouse_x >= bx && mouse_x < bx + 24 &&
                    mouse_y >= by && mouse_y < by + 24) {
                    if (b == 0) {
                        close_window(i);
                    } else if (b == 1) {
                        toggle_fullscreen(i);
                    } else if (b == 2) {
                        // Start a minimize animation (finish_close-style collapse).
                        if (win.anim_state == 0 && !win.minimized) {
                            win.anim_state = 3; // minimizing
                            win.anim_p = 0;
                        }
                    } else { // b == 3 float
                        win.floating = !win.floating;
                        for (int j = 0; j < window_count; j++) windows[j].active = false;
                        win.active = true; active_window = i;
                    }
                    render_all();
                    return true;
                }
            }
            // Title drag (ignore in fullscreen)
            if (!win.fullscreen) {
                drag_window = i;
                drag_off_x = mouse_x - win.x;
                drag_off_y = mouse_y - win.y;
            }
            for (int j = 0; j < window_count; j++) windows[j].active = false;
            win.active = true; active_window = i;
            render_all();
            return true;
        }
        // Content region
        if (win.contains(mouse_x, mouse_y)) {
            for (int j = 0; j < window_count; j++) windows[j].active = false;
            win.active = true; active_window = i;
            handle_app_click(i);
            render_all();
            return true;
        }
        return false;
    }

    void handle_mouse_down() {
        if (!gui_mode) return;
        mouse_left = true;
        start_menu_open = false; // close by default, reopen if clicked

        // A Win32 popup menu is modal: a click selects the item under the
        // cursor (WM_COMMAND to its owner) or dismisses the menu.
        if (w32_popup_hit(mouse_x, mouse_y) >= 0) {
            win32_menu_choose(w32_popup_hit(mouse_x, mouse_y));
            render_all();
            return;
        }
        if (win32_menu_active(nullptr, nullptr, nullptr)) {
            win32_menu_dismiss();
            render_all();
            return;
        }

        // ---- Managed (C#) Win11 shell owns the desktop surface -------
        // Layer order at paint time is wallpaper -> windows -> taskbar,
        // so input has to be tested in the mirror order: taskbar first,
        // then windows, then the desktop icons underneath them.
        if (mforms_has_desktop()) {
            // While the Start menu is up it is modal, exactly as on
            // Windows: every click goes to the shell, and clicking away
            // just dismisses it.
            if (mforms_desktop_menu_open()) {
                int r = mforms_desktop_click(mouse_x, mouse_y);
                if (r >= 0) launch_app(app_for_managed_kind(r));
                render_all();
                return;
            }
            if (mouse_y >= gfx.height - MANAGED_TASKBAR_H) {
                int r = mforms_desktop_click(mouse_x, mouse_y);
                if (r >= 0) taskbar_activate(app_for_managed_kind(r));
                render_all();
                return;
            }
            if (click_windows_z()) return;
            // Desktop icons: a single click only selects (no launch);
            // launching requires a double-click, like Windows.  A second
            // click is a double-click when it lands on the same icon
            // within 500 ms.
            int r = mforms_desktop_click(mouse_x, mouse_y);
            if (r >= 0) {
                // The Terminal shortcut drops straight to the text shell on
                // a single click (task 1: "open terminal -> close GUI"),
                // bypassing the double-click rule other desktop icons use.
                if (app_for_managed_kind(r) == APP_TERMINAL) {
                    launch_app(APP_TERMINAL);
                    render_all();
                } else {
                    uint32_t now = tick_ms_now();
                    bool is_dbl = (r == (int)dbl_kind) &&
                                  (now - dbl_t) < 500u &&
                                  (mouse_x - dbl_x) < 60 && (mouse_x - dbl_x) > -60 &&
                                  (mouse_y - dbl_y) < 60 && (mouse_y - dbl_y) > -60;
                    dbl_t = now; dbl_kind = (uint32_t)r;
                    dbl_x = mouse_x; dbl_y = mouse_y;
                    if (is_dbl) { launch_app(app_for_managed_kind(r)); render_all(); }
                    else        render_all();   // selection state refresh
                }
            }
            else if (r == -1) render_all();
            return;
        }

        // Check top bar (Start button)
        if (mouse_y < TOPBAR_H) {
            if (mouse_x >= 8 && mouse_x < 52) {
                start_menu_open = true;
                render_all();
                return;
            }
            // Check running app buttons in top bar (toggle minimize)
            int ax = 52 + 16;
            for (int i = 0; i < window_count; i++) {
                if (!windows[i].visible) continue;
                int aw = strlen_(windows[i].title) * FONT_W + 24;
                if (mouse_x >= ax && mouse_x < ax + aw) {
                    if (windows[i].minimized) {
                        windows[i].minimized = false;
                        for (int j = 0; j < window_count; j++) windows[j].active = false;
                        windows[i].active = true; active_window = i;
                    } else if (i == active_window) {
                        windows[i].minimized = true;
                        for (int j = 0; j < window_count; j++) windows[j].active = false;
                        active_window = -1;
                        for (int k = window_count - 1; k >= 0; k--)
                            if (windows[k].visible && !windows[k].minimized) {
                                windows[k].active = true; active_window = k; break;
                            }
                    } else {
                        for (int j = 0; j < window_count; j++) windows[j].active = false;
                        windows[i].active = true; active_window = i;
                    }
                    render_all();
                    return;
                }
                ax += aw + 4;
            }
            render_all();
            return;
        }

        // Check start menu (Win11-style: pinned grid + power bar)
        if (start_menu_open) {
            start_menu_open = true;
            const int mw = 380, mh = 248, mx = 8, my = TOPBAR_H + 2;
            // Power bar buttons
            const int pbar_y = my + mh - 44;
            int pw = (mw - 32 - 12) / 2;
            int sx = mx + 16, rx = sx + pw + 12;
            if (mouse_x >= sx && mouse_x < sx + pw && mouse_y >= pbar_y && mouse_y < pbar_y + 32) {
                start_menu_open = false;
                if (g_cb.shutdown) g_cb.shutdown();
                return;
            }
            if (mouse_x >= rx && mouse_x < rx + pw && mouse_y >= pbar_y && mouse_y < pbar_y + 32) {
                start_menu_open = false;
                if (g_cb.reboot) g_cb.reboot();
                return;
            }
            // Pinned grid tiles
            const int tile_w = 80, tile_h = 64, gap = 8;
            const int grid_x = mx + 16, grid_y = my + 40;
            for (int i = 0; i < start_item_count; i++) {
                int col = i % 4, row = i / 4;
                int tx = grid_x + col * (tile_w + gap);
                int ty = grid_y + row * (tile_h + gap);
                if (mouse_x >= tx && mouse_x < tx + tile_w &&
                    mouse_y >= ty && mouse_y < ty + tile_h) {
                    start_menu_open = false;
                    launch_app(start_items[i].app);
                    return;
                }
            }
            start_menu_open = false;
            render_all();
            return;
        }

        // Check portal desktop shortcuts (quick access grid)
        {
            int dw = gfx.width;
            int dy = TOPBAR_H;
            int search_h = 36;
            int sy = dy + 24;
            int grid_y = sy + search_h + 24;
            int tile_w = 80;
            int tile_h = 60;
            int tile_gap = 12;
            int grid_cols = 8;
            int grid_w = grid_cols * tile_w + (grid_cols - 1) * tile_gap;
            if (grid_w > dw - 32) {
                grid_cols = (dw - 32) / (tile_w + tile_gap);
                grid_w = grid_cols * tile_w + (grid_cols - 1) * tile_gap;
            }
            int grid_x = (dw - grid_w) / 2;

            AppType shortcut_apps[] = {
                APP_CONTROL_PANEL, APP_FILE_EXPLORER, APP_TASK_MANAGER,
                APP_MEM_OPTIMIZER, APP_TERMINAL, APP_BROWSER,
                APP_CALCULATOR, APP_ABOUT
            };

            for (int i = 0; i < 8; i++) {
                int col = i % grid_cols;
                int row = i / grid_cols;
                int tx = grid_x + col * (tile_w + tile_gap);
                int ty = grid_y + row * (tile_h + tile_gap);
                if (mouse_x >= tx && mouse_x < tx + tile_w &&
                    mouse_y >= ty && mouse_y < ty + tile_h) {
                    launch_app(shortcut_apps[i]);
                    render_all();
                    return;
                }
            }
        }

        click_windows_z();
    }

    // Hit-test the open windows in correct z-order so a covered lower
    // window's buttons are never clicked through an upper window.
    // Returns true when a window consumed the click.
    bool click_windows_z() {
        // 1) Floating windows are always on top
        for (int i = window_count - 1; i >= 0; i--) {
            if (!windows[i].visible || windows[i].minimized || !windows[i].floating) continue;
            if (window_click(i)) return true;
        }
        // 2) Active window
        if (active_window >= 0 && windows[active_window].visible &&
            !windows[active_window].minimized) {
            if (window_click(active_window)) return true;
        }
        // 3) Inactive (non-floating) windows, topmost first
        for (int i = window_count - 1; i >= 0; i--) {
            if (i == active_window) continue;
            if (!windows[i].visible || windows[i].minimized || windows[i].floating) continue;
            if (window_click(i)) return true;
        }
        return false;
    }

    // Taskbar semantics: clicking a running app focuses it; clicking the
    // focused app again minimises it.  Anything else launches.
    void taskbar_activate(AppType app) {
        // The Terminal pin is a shortcut back to the text shell, never a
        // toggle for a (hypothetical) terminal window: always drop the GUI,
        // even if some path ever leaves a terminal window open.  This keeps
        // the taskbar Terminal button working exactly like the desktop one.
        if (app == APP_TERMINAL) { launch_app(app); return; }
        int ex = find_window_for_kind(app);
        if (ex < 0) { launch_app(app); return; }
        if (ex == active_window && !windows[ex].minimized) {
            // Clicking the focused taskbar button again -> animate minimize.
            if (windows[ex].anim_state == 0) {
                windows[ex].anim_state = 3; // minimizing
                windows[ex].anim_p = 0;
            }
            return;
        }
        // Restore if minimized (animate expand); otherwise just focus.
        if (windows[ex].minimized && windows[ex].anim_state == 0) {
            windows[ex].minimized = false;
            windows[ex].anim_state = 4; // restoring
            windows[ex].anim_p = 0;
        }
        for (int j = 0; j < window_count; j++) windows[j].active = false;
        windows[ex].active = true;
        active_window = ex;
    }

    void handle_mouse_up() {
        mouse_left = false;
        drag_window = -1;
    }

    // Right mouse button: open the kernel-native context menus.  Only
    // the C# shell (mforms) understands them, so this is a no-op when the
    // legacy desktop is active.
    void handle_mouse_down_right() {
        if (!gui_mode) return;
        // A right-click outside a Win32 popup menu dismisses it; a right
        // click on an item also chooses it (same as the left button).
        if (w32_popup_hit(mouse_x, mouse_y) >= 0) {
            win32_menu_choose(w32_popup_hit(mouse_x, mouse_y));
            render_all();
            return;
        }
        if (win32_menu_active(nullptr, nullptr, nullptr)) {
            win32_menu_dismiss();
            render_all();
            return;
        }
        // A right-click is never part of a left-button double-click; reset
        // the desktop double-click state so the next left click is judged
        // on its own.
        dbl_kind = 0xFFFFFFFFu; dbl_t = 0; dbl_x = -9999; dbl_y = -9999;
        if (!mforms_has_desktop()) return;

        // A menu is already up: any click dismisses / selects it.
        if (mforms_desktop_menu_open()) {
            mforms_desktop_click(mouse_x, mouse_y);
            render_all();
            return;
        }
        // Taskbar strip or wallpaper -> the desktop / taskbar / tray menu.
        if (mouse_y >= gfx.height - MANAGED_TASKBAR_H) {
            mforms_desktop_rclick(mouse_x, mouse_y);
            render_all();
            return;
        }
        // Otherwise test windows topmost-first for a managed one.
        for (int i = window_count - 1; i >= 0; i--) {
            Win11Window& win = windows[i];
            if (!win.visible || win.minimized) continue;
            if (win.managed_app < 0) continue;
            if (win.contains(mouse_x, mouse_y)) {
                // Same client rect as the paint / left-click path: the
                // managed handler receives window-LOCAL coordinates, so
                // using win.y here (instead of content_y()) would shift
                // every right-click down by the title-bar height and hit
                // the wrong file row.
                mforms_rclick(win.managed_app, win.x + 1, win.content_y(),
                              win.w - 2, win.content_h(), mouse_x, mouse_y);
                render_all();
                return;
            }
        }
        // Fall back to the desktop menu (wallpaper).
        mforms_desktop_rclick(mouse_x, mouse_y);
        render_all();
    }

    void handle_app_click(int win_id) {
        Win11Window& win = windows[win_id];
        int x = win.x + 8;
        int y = win.content_y() + 8;
        int w = win.w - 16;
        int h = win.content_h() - 16;

        // ---- Native Win32 app: forward the click as WM_LBUTTONDOWN/UP ----
        if (win.app == APP_WIN32) {
            if (win.w32_index < 0) return;
            int lx = mouse_x - (win.x + 2);
            int ly = mouse_y - (win.content_y() + 2);
            if (lx < 0) lx = 0;
            if (ly < 0) ly = 0;
            uint32_t lp = ((uint32_t)(ly & 0xFFFF) << 16) | (uint32_t)(lx & 0xFFFF);
            win32_window_dispatch(win.w32_index, 0x0201, 1, lp);   // WM_LBUTTONDOWN
            win32_window_dispatch(win.w32_index, 0x0202, 0, lp);   // WM_LBUTTONUP
            win32_window_repaint(win.w32_index);
            return;
        }

        // ---- Managed (C#) app: route the click into NexOS.Forms ----
        if (win.app == APP_MANAGED) {
            int ox = win.x + 1, oy = win.content_y();
            int mw = win.w - 2, mh = win.content_h();
            mforms_click(win.managed_app, ox, oy, mw, mh, mouse_x, mouse_y);
            return;   // caller repaints
        }

        if (win.app == APP_FILE_EXPLORER) {
            // Check tab clicks
            int tab_w = 70;
            int tab_y = y + 24;
            for (int i = 0; i < 3; i++) {
                int tx = x + i * (tab_w + 4);
                if (mouse_x >= tx && mouse_x < tx + tab_w &&
                    mouse_y >= tab_y && mouse_y < tab_y + 22) {
                    win.selected_item = i;
                    win.sel_file_idx = -1; // reset selection on tab change
                    return;
                }
            }

            // Check file list clicks
            int list_w = 220;
            int list_start_y = y + 24 + 30 + 4; // after title + tabs + separator
            int list_h = win.h - TITLE_BAR_H - 16 - (list_start_y - y);
            if (mouse_x >= x && mouse_x < x + list_w &&
                mouse_y >= list_start_y && mouse_y < list_start_y + list_h) {
                int clicked_line = (mouse_y - list_start_y - 4) / 18;
                if (clicked_line >= 0) {
                    win.sel_file_idx = clicked_line;
                }
                return;
            }
        }
        else if (win.app == APP_CONTROL_PANEL) {
            if (win.cp_category < 0) {
                // Category list view - check category card clicks
                int col_w = (w - 12) / 2;
                int row_h = 64;
                int gap = 8;
                int list_y = y + 24 + 12; // after header + separator

                for (int i = 0; i < 8; i++) {
                    int col = i % 2;
                    int row = i / 2;
                    int cx = x + col * (col_w + gap);
                    int cy = list_y + row * (row_h + gap);
                    if (mouse_x >= cx && mouse_x < cx + col_w &&
                        mouse_y >= cy && mouse_y < cy + row_h) {
                        win.cp_category = i;
                        return;
                    }
                }
            } else {
                // Detail view - check back button
                if (mouse_x >= x && mouse_x < x + 80 &&
                    mouse_y >= y && mouse_y < y + 24) {
                    win.cp_category = -1;
                    return;
                }
                // Check quick action buttons (only in System & Security category)
                if (win.cp_category == 0) {
                    // Calculate button Y positions (same as draw_control_panel)
                    int by = y + 32 + 28 + 8; // after back button + title + separator
                    by += 20 + 8; // "System Information" header + separator
                    // OS line + Arch line + Display line + Memory line + Page usage (if present)
                    by += 18 + 18 + 18 + 18; // 4 info lines
                    if (g_cb.get_used_pages && g_cb.get_total_pages) by += 20 + 24; // progress bar
                    if (g_cb.get_heap_alloc_bytes) by += 18; // heap line
                    by += 8 + 20 + 8; // "Quick Actions" header + separator

                    // Optimize Memory button
                    if (mouse_x >= x && mouse_x < x + 140 &&
                        mouse_y >= by && mouse_y < by + 28) {
                        do_memory_optimize(win_id);
                        return;
                    }
                    // Task Manager button
                    if (mouse_x >= x + 150 && mouse_x < x + 290 &&
                        mouse_y >= by && mouse_y < by + 28) {
                        launch_app(APP_TASK_MANAGER);
                        return;
                    }
                }
            }
        }
        else if (win.app == APP_MEM_OPTIMIZER) {
            // Optimize button
            int by = y + 24 + 20 + 8 + 22 + 26 + 22 + 26; // approximate
            if (mouse_x >= x && mouse_x < x + 160 &&
                mouse_y >= by && mouse_y < by + 32) {
                do_memory_optimize(win_id);
                return;
            }
        }
        else if (win.app == APP_CALCULATOR) {
            // Check calculator buttons
            int cy = y + 44;
            int bw = (w - 20) / 4;
            int bh = 32;
            const char* labels[] = {"7","8","9","+","4","5","6","-","1","2","3","*","0","=","C","/"};
            for (int i = 0; i < 16; i++) {
                int bx = x + (i % 4) * (bw + 4);
                int btn_y = cy + (i / 4) * (bh + 4);
                if (mouse_x >= bx && mouse_x < bx + bw &&
                    mouse_y >= btn_y && mouse_y < btn_y + bh) {
                    calc_button(win_id, labels[i][0]);
                    return;
                }
            }
        }
        else if (win.app == APP_TASK_MANAGER) {
            int search_h = 28;
            int search_w = w - 180;
            // Check search bar click
            if (mouse_x >= x && mouse_x < x + search_w &&
                mouse_y >= y && mouse_y < y + search_h) {
                win.tm_search_focused = true;
                return;
            }
            win.tm_search_focused = false;

            // Check action buttons
            int btn_w = 80;
            int btn_gap = 8;
            int bx = x + search_w + 8;
            // End Task button
            if (mouse_x >= bx && mouse_x < bx + btn_w &&
                mouse_y >= y && mouse_y < y + search_h) {
                // Close selected process window
                if (win.tm_selected_proc >= 0 && win.tm_selected_proc < window_count) {
                    close_window(win.tm_selected_proc);
                }
                return;
            }
            // Run New button
            int bx2 = bx + btn_w + btn_gap;
            if (mouse_x >= bx2 && mouse_x < bx2 + btn_w &&
                mouse_y >= y && mouse_y < y + search_h) {
                launch_app(APP_TERMINAL);
                return;
            }

            // Check process row clicks
            int sum_h = 36;
            int header_h = 24;
            int row_h = 20;
            int table_y = y + search_h + 8 + sum_h + 8 + header_h;
            int max_rows = (h - (table_y - y)) / row_h;

            // Count processes (same as draw_task_manager)
            int nproc = 0;
            for (int i = 0; i < window_count; i++) {
                if (windows[i].visible) nproc++;
            }
            nproc += 4; // Shell + Network + AI + DWM

            for (int i = 0; i < nproc && i < max_rows; i++) {
                int ry = table_y + i * row_h;
                if (mouse_x >= x && mouse_x < x + w &&
                    mouse_y >= ry && mouse_y < ry + row_h) {
                    win.tm_selected_proc = i;
                    return;
                }
            }
        }
        else if (win.app == APP_BROWSER) {
            browser::ensure_init();
            win.browser_url_focused = false;
            // Tab bar: close (x) then switch
            for (int i = 0; i < browser::g_ntabs; i++) {
                if (browser::g_tab_close[i].has(mouse_x, mouse_y)) { browser::close_tab(i); return; }
                if (browser::g_tab_rects[i].has(mouse_x, mouse_y)) { browser::g_active = i; return; }
            }
            if (browser::g_newtab_rect.has(mouse_x, mouse_y)) { browser::new_tab("about:home"); return; }
            // Navigation buttons: back, forward, reload, stop, home
            if (browser::g_nav_rects[0].has(mouse_x, mouse_y)) { browser::go_back(browser::g_active); return; }
            if (browser::g_nav_rects[1].has(mouse_x, mouse_y)) { browser::go_forward(browser::g_active); return; }
            if (browser::g_nav_rects[2].has(mouse_x, mouse_y)) { browser::reload(browser::g_active); return; }
            if (browser::g_nav_rects[3].has(mouse_x, mouse_y)) { browser::stop(browser::g_active); return; }
            if (browser::g_nav_rects[4].has(mouse_x, mouse_y)) { browser::home(browser::g_active); return; }
            // URL field (to the right of the nav buttons)
            if (mouse_x >= browser::g_nav_rects[4].x + browser::g_nav_rects[4].w + 4 &&
                mouse_y >= browser::g_nav_rects[4].y && mouse_y <= browser::g_nav_rects[4].y + browser::g_nav_rects[4].h) {
                // Seed the editable buffer with the active tab's current URL
                const char* cu = browser::g_tabs[browser::g_active].url[0] ? browser::g_tabs[browser::g_active].url : "about:home";
                win.browser_url_len = 0;
                for (int k = 0; cu[k] && win.browser_url_len < 254; k++) win.browser_url[win.browser_url_len++] = cu[k];
                win.browser_url[win.browser_url_len] = 0;
                win.browser_url_focused = true;
                serial_puts("[browser] addr="); serial_puts(win.browser_url); serial_puts("\n");
                return;
            }
            // Bookmarks bar
            for (int i = 0; i < browser::g_nbm; i++) {
                if (browser::g_bm_rects[i].has(mouse_x, mouse_y)) { browser::navigate(browser::g_active, browser::g_bm[i].url); return; }
            }
            // Scrollbar
            if (browser::g_scroll_rect.has(mouse_x, mouse_y)) {
                int sc = browser::g_tabs[browser::g_active].scroll + ((mouse_y > browser::g_scroll_rect.y + browser::g_scroll_rect.h/2) ? 60 : -60);
                if (sc < 0) sc = 0;
                if (browser::g_scroll_max > 0 && sc > browser::g_scroll_max) sc = browser::g_scroll_max;
                browser::g_tabs[browser::g_active].scroll = sc;
                return;
            }
            // Clickable hyperlink
            char href[256];
            if (browser::hit_link(mouse_x, mouse_y, href)) { browser::navigate(browser::g_active, href); return; }
        }
    }

    void calc_button(int win_id, char ch) {
        Win11Window& win = windows[win_id];
        if (ch >= '0' && ch <= '9') {
            int digit = ch - '0';
            if (win.calc_new_input) {
                win.calc_display = digit;
                win.calc_new_input = false;
            } else {
                win.calc_display = win.calc_display * 10 + digit;
            }
        } else if (ch == 'C') {
            win.calc_display = 0;
            win.calc_prev = 0;
            win.calc_op = 0;
            win.calc_new_input = true;
        } else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
            win.calc_prev = win.calc_display;
            win.calc_op = (ch == '+') ? 1 : (ch == '-') ? 2 : (ch == '*') ? 3 : 4;
            win.calc_new_input = true;
        } else if (ch == '=') {
            switch (win.calc_op) {
                case 1: win.calc_display = win.calc_prev + win.calc_display; break;
                case 2: win.calc_display = win.calc_prev - win.calc_display; break;
                case 3: win.calc_display = win.calc_prev * win.calc_display; break;
                case 4: win.calc_display = win.calc_display != 0 ? win.calc_prev / win.calc_display : 0; break;
            }
            win.calc_op = 0;
            win.calc_new_input = true;
        }
    }

    void do_memory_optimize(int win_id) {
        Win11Window& win = windows[win_id];
        // Record before
        if (g_cb.get_used_pages && g_cb.get_total_pages) {
            uint32_t used = g_cb.get_used_pages();
            win.mem_before_kb = used * 4; // 4KB per page
        }
        if (g_cb.get_heap_alloc_bytes) {
            win.mem_before_kb += g_cb.get_heap_alloc_bytes() / 1024;
        }

        // Call kernel optimization
        if (g_cb.optimize_memory) {
            g_cb.optimize_memory();
            serial_puts("[GUI] Memory optimization triggered\n");
        }

        // Record after
        if (g_cb.get_used_pages && g_cb.get_total_pages) {
            uint32_t used = g_cb.get_used_pages();
            win.mem_after_kb = used * 4;
        }
        if (g_cb.get_heap_alloc_bytes) {
            win.mem_after_kb += g_cb.get_heap_alloc_bytes() / 1024;
        }

        win.mem_optimized = true;
    }

    // =================================================================
    //  Native Win32 application window
    //  The PE32 app paints into a GDI display list (win32.cpp); here we
    //  rasterize that list into the window's client area.
    // =================================================================
    void draw_win32_app(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];

        // Win32 default client background (COLOR_WINDOW)
        gfx.fill_rect(x, y, w, h, 0xF0F0F0);
        gfx.draw_rect(x, y, w, h, 0xA8A8A8);

        if (win.w32_index < 0) {
            gfx.draw_text(x + 10, y + 10, "[Win32] process has exited", 0x808080, 0xF0F0F0);
            return;
        }

        const W32DrawCmd* cmds = nullptr;
        int n = win32_window_cmds(win.w32_index, &cmds);
        if (n <= 0 || !cmds) {
            gfx.draw_text(x + 10, y + 10, "[Win32] no paint output", 0x808080, 0xF0F0F0);
            char fb[64];
            fb[0] = 0;
            int p = 0;
            const char* s = "file: ";
            while (*s && p < 60) fb[p++] = *s++;
            for (int i = 0; win.w32_file[i] && p < 60; i++) fb[p++] = win.w32_file[i];
            fb[p] = 0;
            gfx.draw_text(x + 10, y + 30, fb, 0x808080, 0xF0F0F0);
            return;
        }

        int cl = x, ct = y, cr = x + w, cb = y + h;

        for (int i = 0; i < n; i++) {
            const W32DrawCmd& c = cmds[i];
            int rx = x + c.x;
            int ry = y + c.y;
            int rw = c.w;
            int rh = c.h;

            switch (c.kind) {
            case W32_CMD_FILLRECT:
            case W32_CMD_FRAMERECT: {
                if (rw < 0) { rx += rw; rw = -rw; }
                if (rh < 0) { ry += rh; rh = -rh; }
                if (rx < cl) { rw -= (cl - rx); rx = cl; }
                if (ry < ct) { rh -= (ct - ry); ry = ct; }
                if (rx + rw > cr) rw = cr - rx;
                if (ry + rh > cb) rh = cb - ry;
                if (rw <= 0 || rh <= 0) break;
                if (c.kind == W32_CMD_FILLRECT) gfx.fill_rect(rx, ry, rw, rh, c.color);
                else                            gfx.draw_rect(rx, ry, rw, rh, c.color);
                break;
            }
            case W32_CMD_ELLIPSE: {
                if (rw <= 0 || rh <= 0) break;
                int ecx = rx + rw / 2, ecy = ry + rh / 2;
                int r = (rw < rh ? rw : rh) / 2;
                if (ecx - r < cl || ecy - r < ct || ecx + r > cr || ecy + r > cb) {
                    if (ecx < cl || ecx > cr || ecy < ct || ecy > cb) break;
                    int m = ecx - cl; if (cr - ecx < m) m = cr - ecx;
                    if (ecy - ct < m) m = ecy - ct;
                    if (cb - ecy < m) m = cb - ecy;
                    if (r > m) r = m;
                }
                if (r > 0) gfx.fill_circle(ecx, ecy, r, c.color);
                break;
            }
            case W32_CMD_LINE: {
                int x0 = rx, y0 = ry;
                int x1 = rx + rw, y1 = ry + rh;
                if (x0 < cl) x0 = cl;
                if (x0 > cr) x0 = cr;
                if (x1 < cl) x1 = cl;
                if (x1 > cr) x1 = cr;
                if (y0 < ct) y0 = ct;
                if (y0 > cb) y0 = cb;
                if (y1 < ct) y1 = ct;
                if (y1 > cb) y1 = cb;
                gfx.draw_line(x0, y0, x1, y1, c.color);
                break;
            }
            case W32_CMD_BUTTON: {
                if (rw <= 0) rw = 76;
                if (rh <= 0) rh = 26;
                if (rx + rw > cr) rw = cr - rx;
                if (ry + rh > cb) rh = cb - ry;
                if (rw <= 4 || rh <= 4) break;
                gfx.fill_rounded_rect(rx, ry, rw, rh, 3, 0xE1E1E1);
                gfx.draw_rounded_rect(rx, ry, rw, rh, 3, 0x808080);
                int tw = strlen_(c.text) * FONT_W;
                int tx = rx + (rw - tw) / 2;
                int ty = ry + (rh - FONT_H) / 2;
                if (tx < rx + 2) tx = rx + 2;
                gfx.draw_text(tx, ty, c.text, 0x101010, 0xE1E1E1);
                break;
            }
            case W32_CMD_TEXT: {
                if (rx >= cr || ry >= cb) break;
                if (ry + FONT_H > cb) break;
                if (c.bkcolor == 0xFFFFFFFF)
                    gfx.draw_text_utf8_transparent(rx, ry, c.text, c.color);
                else
                    gfx.draw_text_utf8(rx, ry, c.text, c.color, c.bkcolor);
                break;
            }
            case W32_CMD_PIXEL: {
                if (rx >= cl && rx < cr && ry >= ct && ry < cb)
                    gfx.fill_rect(rx, ry, 1, 1, c.color);
                break;
            }
            default: break;
            }
        }
    }

    // Map a native AppType to the managed NexOS.Forms.Kind index the C#
    // shell understands.  -1 means "no managed equivalent, stay native".
    // Indices must agree with csharp/apps/Shell/Shell.cs (Kind.*).
    static int managed_kind_for(AppType app) {
        switch (app) {
            case APP_CONTROL_PANEL: return 0;   // Kind.ControlPanel
            case APP_FILE_EXPLORER: return 1;   // Kind.FileExplorer
            case APP_TASK_MANAGER:  return 2;   // Kind.TaskManager
            case APP_TERMINAL:      return 3;   // Kind.Terminal
            case APP_CALCULATOR:    return 4;   // Kind.Calculator
            case APP_ABOUT:         return 5;   // Kind.About
            case APP_MEM_OPTIMIZER: return 6;   // Kind.MemOptimizer
            case APP_NOTEPAD:       return 7;   // Kind.Notepad (managed only)
            case APP_BROWSER:       return 8;   // Kind.Browser
            case APP_AISETUP:       return 9;   // Kind.AiSetup
            case APP_AIAGENT:       return 10;  // Kind.AiAgent
            default:                return -1;   // Win32 stay native
        }
    }

    // Inverse of managed_kind_for: turn the Kind the C# shell asked for
    // back into the AppType launch_app understands.
    static AppType app_for_managed_kind(int kind) {
        switch (kind) {
            case 0: return APP_CONTROL_PANEL;
            case 1: return APP_FILE_EXPLORER;
            case 2: return APP_TASK_MANAGER;
            case 3: return APP_TERMINAL;
            case 4: return APP_CALCULATOR;
            case 5: return APP_ABOUT;
            case 6: return APP_MEM_OPTIMIZER;
            case 7: return APP_NOTEPAD;
            case 8: return APP_BROWSER;
            case 9: return APP_AISETUP;
            case 10: return APP_AIAGENT;
            default: return APP_NONE;
        }
    }

    // First visible window that *is* this application, or -1.  Clicking a
    // taskbar/desktop icon for a running app focuses it (and un-minimises
    // it) rather than opening a duplicate, which is what Windows does.
    int find_window_for_kind(AppType app) const {
        for (int i = window_count - 1; i >= 0; i--)
            if (windows[i].visible && windows[i].launch_kind == app) return i;
        return -1;
    }

    // Close (fade out) every visible window of a given app kind.  Used by
    // the taskbar right-click "Close window" / "End process" menu.
    void close_window_for_kind(AppType app) {
        for (int i = 0; i < window_count; i++) {
            if (windows[i].visible && windows[i].launch_kind == app)
                close_window(i);
        }
    }

    // Loads and EXECUTES a native Windows PE image (.exe) through the
    // win32_run / win64_run loader and surfaces the windows it creates on
    // the desktop -- exactly what `winapp <file>` does at the prompt.
    // Defined below (after gui_launch_win32).
    int launch_pe_exe(const char* filename);

    // The desktop "Browser" icon runs a real PE.  This tree is 32-bit only,
    // so the browser shell is Internet Explorer built for i386 (PE32).
    int launch_browser_pe(void) {
        if (launch_pe_exe("iexplore.exe") > 0) return 1;
        serial_puts("[GUI] iexplore.exe did not load\n");
        return 0;
    }

    void launch_app(AppType app) {
        // The Terminal is opened as a managed C# GUI window (just like
        // Notepad/Browser) so it gets the same Chinese-IME support on every
        // keypress (Shift toggles 中文/EN, pinyin commits via mforms_key).
        // The text-mode shell remains reachable by exiting the GUI (Esc on
        // the desktop); this entry point no longer drops out of the GUI.

        // The desktop "Browser" icon is a REAL WinPE-style binary loaded
        // and executed by the PE loader (iexplore.exe, PE32 i386).  Route it
        // through the same winapp path and fall back to the managed
        // BrowserApp only if the image is missing or fails to load, so the
        // icon never dead-ends.
        if (app == APP_BROWSER) {
            if (launch_browser_pe() > 0) return;
            serial_puts("[GUI] no PE browser available; using managed browser\n");
        }

        int gw = gfx.width;
        int gh = gfx.height;
        int ww, wh, wx, wy;
        const char* title = "";

        switch (app) {
            case APP_CONTROL_PANEL:
                ww = 520; wh = 440; title = "Control Panel"; break;
            case APP_FILE_EXPLORER:
                ww = 520; wh = 360; title = "File Explorer"; break;
            case APP_TASK_MANAGER:
                ww = 560; wh = 420; title = "Task Manager"; break;
            case APP_MEM_OPTIMIZER:
                ww = 360; wh = 320; title = "Memory Optimizer"; break;
            case APP_CALCULATOR:
                ww = 220; wh = 240; title = "Calculator"; break;
            case APP_TERMINAL:
                ww = 400; wh = 280; title = "Terminal"; break;
            case APP_ABOUT:
                ww = 360; wh = 360; title = "About NexOS"; break;
            case APP_NOTEPAD:
                ww = 480; wh = 360; title = "Notepad"; break;
            case APP_BROWSER:
                ww = 600; wh = 440; title = "Web Browser"; break;
            case APP_AISETUP:
                ww = 380; wh = 320; title = "AI Setup"; break;
            case APP_AIAGENT:
                ww = 420; wh = 440; title = "AI Agent"; break;
            default: return;
        }

        // Already running?  Focus + restore instead of stacking a clone.
        {
            int ex = find_window_for_kind(app);
            if (ex >= 0) {
                windows[ex].minimized = false;
                for (int j = 0; j < window_count; j++) windows[j].active = false;
                windows[ex].active = true;
                active_window = ex;
                return;
            }
        }

        wx = (gw - ww) / 2;
        wy = (gh - wh) / 2;
        // Offset for cascading
        wy += TOPBAR_H + 10;
        if (wy + wh > gh - 10) wy = gh - wh - 10;

        // ---- Prefer the C# implementation for every app that has one ----
        // The whole point of the managed shell: these windows are painted
        // and driven entirely by NexOS.Forms.  Fall back to the legacy
        // native drawer only if the shell failed to load.
        int mkind = managed_kind_for(app);
        if (mkind >= 0 && mforms_ready()) {
            int mid = mforms_open(mkind);
            if (mid >= 0) {
                int wid = create_window(wx, wy, ww, wh, title, APP_MANAGED);
                if (wid >= 0) {
                    windows[wid].managed_app = mid;
                    windows[wid].launch_kind = app;   // for taskbar / focus
                    // Title chosen by the managed app, when it offers one.
                    const char* mt = mforms_title(mid);
                    if (mt && mt[0]) {
                        int tl = strlen_(mt); if (tl > 39) tl = 39;
                        memcpy_(windows[wid].title, mt, tl);
                        windows[wid].title[tl] = 0;
                    }
                }
                return;
            }
        }

        // Fallback: the legacy native drawer.  Notepad never had one, so
        // without a managed shell there is simply nothing to open.
        if (app == APP_NOTEPAD) return;
        int id = create_window(wx, wy, ww, wh, title, app);
        if (id >= 0) windows[id].launch_kind = app;
        if (id >= 0 && app == APP_TERMINAL) {
            // Initialize terminal
            Win11Window& win = windows[id];
            strcpy_(win.term_buf, "NexOS Terminal v2.0\nType 'help' for commands.\n\n");
            win.term_len = strlen_(win.term_buf);
        }
    }

    // ---- Open a Windows executable file inside the GUI ----
    // Detects the file type (via winloader) and opens a viewer window.
    // For .bat/.ps1 scripts the engine actually executes them and the
    // output is shown inside the GUI window.
    void open_file_in_gui(const char* filename, const char* args) {
        if (!filename) return;
        // 创建窗口
        int gw = gfx.width, gh = gfx.height;
        int ww = 460, wh = 320;
        int wx = (gw - ww) / 2, wy = TOPBAR_H + 40;
        int id = create_window(wx, wy, ww, wh, filename, APP_FILE_EXPLORER);
        if (id < 0) return;
        Win11Window& win = windows[id];
        // 先显示文件头信息
        char* dst = win.term_buf;
        int   len = 0;
        const char* hdr = "=== NexOS Windows App Launcher ===\n\nFile: ";
        while (*hdr && len < 1023) dst[len++] = *hdr++;
        while (*filename && len < 1023) dst[len++] = *filename++;
        dst[len++] = '\n';
        if (args && args[0]) {
            const char* an = "Args: ";
            while (*an && len < 1023) dst[len++] = *an++;
            while (*args && len < 1023) dst[len++] = *args++;
            dst[len++] = '\n';
        }
        dst[len++] = '\n';
        dst[len] = 0;
        win.term_len = len;
        // 实际执行 winloader (BAT/PS1 引擎执行, PE/COM 检测) 捕获输出
        if (g_cb.read_file) {
            // 先确认文件存在
            static uint8_t probe[4];
            int rd = g_cb.read_file(0, filename, probe, sizeof(probe));
            if (rd < 0) rd = g_cb.read_file(1, filename, probe, sizeof(probe));
            if (rd >= 0) {
                // winloader_capture_run 输出到窗口缓冲
                static char wl_out[1800];
                winloader_capture_run(filename, args ? args : "", wl_out, sizeof(wl_out));
                // 追加输出到窗口
                int o = 0;
                while (wl_out[o] && len < 1023) dst[len++] = wl_out[o++];
                dst[len] = 0;
                win.term_len = len;
            } else {
                const char* nf = "\n[!] File not found in SFS/MKFS.\n";
                while (*nf && len < 1023) dst[len++] = *nf++;
                dst[len] = 0;
                win.term_len = len;
            }
        }
        render_all();
    }

    // =================================================================
    //  Launch a native Win32 (PE32) application and show its windows.
    //  win32_run() has already executed the image; every top-level HWND
    //  it created gets a real NexOS desktop window here.
    //  Returns the number of windows created (0 = app created none).
    // =================================================================
    int launch_win32_windows(const char* filename) {
        int wn = win32_window_count();
        int made = 0;
        int gw = gfx.width, gh = gfx.height;
        for (int i = 0; i < wn; i++) {
            W32WinInfo wi;
            if (!win32_window_info(i, &wi)) continue;
            if (!wi.visible) continue;

            int ww = wi.w + 4;
            int wh = wi.h + TITLE_BAR_H + 4;
            if (ww < 240) ww = 240;
            if (wh < 140) wh = 140;
            if (ww > gw - 20) ww = gw - 20;
            if (wh > gh - TOPBAR_H - 20) wh = gh - TOPBAR_H - 20;

            int wx = wi.x;
            int wy = wi.y + TOPBAR_H;
            if (wi.is_msgbox) {                 // centre message boxes
                wx = (gw - ww) / 2;
                wy = (gh - wh) / 2;
            }
            wx += made * 24;
            wy += made * 24;
            if (wx < 4) wx = 4;
            if (wy < TOPBAR_H + 4) wy = TOPBAR_H + 4;
            if (wx + ww > gw - 4) wx = gw - ww - 4;
            if (wy + wh > gh - 4) wy = gh - wh - 4;

            int id = create_window(wx, wy, ww, wh, wi.title, APP_WIN32);
            if (id < 0) break;
            windows[id].w32_index = i;
            int fl = 0;
            if (filename) { while (filename[fl] && fl < 47) { windows[id].w32_file[fl] = filename[fl]; fl++; } }
            windows[id].w32_file[fl] = 0;
            win32_window_repaint(i);
            made++;
        }
        if (made) render_all();
        return made;
    }

    // ---- Shared pinyin IME router (all text input boxes) ----
    // Returns true if the key was consumed by the composer.  Active only in
    // Chinese mode (g_ime_cn).  Routes a-z/space/digit/backspace/enter for
    // whichever input box currently has focus:
    //   - Terminal input line
    //   - Browser URL bar (when focused)
    //   - Managed (C#) text box (committed glyphs delivered via mforms_key)
    // `out_buf`/`out_len`/`out_max` are only used for the native buffers.
    bool ime_route(char ch, char* out_buf, int* out_len, int out_max) {
        if (!g_ime_cn) return false;
        if (active_window < 0 || active_window >= window_count) return false;
        Win11Window& w = windows[active_window];
        bool text_box = (w.app == APP_TERMINAL)
                     || (w.app == APP_BROWSER && w.browser_url_focused)
                     || (w.app == APP_MANAGED);
        if (!text_box) return false;

        if (ch >= 'a' && ch <= 'z') {
            if (g_ime_len < 15) {
                g_ime_py[g_ime_len++] = ch;
                g_ime_py[g_ime_len] = 0;
                g_ime_active = true;
                ime_lookup();
            }
            return true;
        }
        if (ch == 0x08) { // Backspace
            if (g_ime_active && g_ime_len > 0) {
                g_ime_len--;
                g_ime_py[g_ime_len] = 0;
                ime_lookup();
                return true;
            }
            return false; // let the window delete a real character
        }
        if (ch == ' ') {   // Space -> candidate #1 (or flush if none)
            if (g_ime_active) {
                if (g_ime_cand_count > 0)
                    ime_commit((uint32_t)g_ime_cands[0],
                               w.app == APP_MANAGED ? 1 : 0, w.managed_app,
                               out_buf, out_len, out_max);
                else
                    ime_flush_raw(w, out_buf, out_len, out_max);
                ime_reset();
            }
            return true;
        }
        if (ch >= '1' && ch <= '9' && g_ime_active && g_ime_cand_count > 0) {
            int idx = ch - '1';
            if (idx < g_ime_cand_count)
                ime_commit((uint32_t)g_ime_cands[idx],
                           w.app == APP_MANAGED ? 1 : 0, w.managed_app,
                           out_buf, out_len, out_max);
            ime_reset();
            return true;
        }
        if (ch == '\n' || ch == 0x0D) { // Enter flushes raw pinyin as text
            if (g_ime_active) ime_flush_raw(w, out_buf, out_len, out_max);
            return false; // let the window run its own Enter action
        }
        // Any other printable char while composing: drop the syllable and let
        // the character through as normal English input.
        if (g_ime_active && ch >= 32 && ch < 127) {
            ime_flush_raw(w, out_buf, out_len, out_max);
            return false;
        }
        return false;
    }

    // Flush the raw (un-committed) pinyin as literal ASCII into the active box.
    void ime_flush_raw(Win11Window& w, char* out_buf, int* out_len, int out_max) {
        if (w.app == APP_MANAGED) {
            for (int i = 0; i < g_ime_len; i++)
                mforms_key(w.managed_app, (int)g_ime_py[i]);
        } else if (out_buf && out_len) {
            for (int i = 0; i < g_ime_len && *out_len < out_max - 1; i++)
                out_buf[(*out_len)++] = g_ime_py[i];
            if (out_buf) out_buf[*out_len] = 0;
        }
        ime_reset();
    }

    // Flip Chinese/English input mode (bound to Shift).  Resets any
    // in-progress pinyin composition so a half-typed syllable never leaks
    // across a language toggle.
    void gui_toggle_ime() {
        g_ime_cn = !g_ime_cn;
        ime_reset();
        if (g_ime_cn) serial_puts("[GUI] IME mode -> Chinese (中文)\n");
        else          serial_puts("[GUI] IME mode -> English (EN)\n");
        if (gui_mode) render_all();
    }

    bool handle_key(char ch) {
        if (!gui_mode) return false;
        if (ch == 27) { // ESC: cancel the IME, never leave the desktop
            if (g_ime_active) {
                ime_reset();
                render_all();
                return true;
            }
            // The desktop is the primary interface: ESC used to drop the
            // whole session back to the text terminal, which made a stray
            // keypress look like a crash.  The terminal is still reachable
            // through the Start menu / desktop "Terminal" shortcut, which
            // calls gui_exit() deliberately.
            serial_puts("[GUI] ESC ignored (desktop stays up)\n");
            return true;
        }

        // ---- Desktop surface (inline rename editor, etc.) ----
        // When no window is focused and no popup menu is up, hand the
        // keystroke to the managed desktop (it is a no-op unless the
        // desktop is in rename mode).
        if (active_window < 0 && !mforms_desktop_menu_open()) {
            mforms_desktop_key((int)(unsigned char)ch);
            render_all();
            return true;
        }
        // ---- Native Win32 app: forward keystrokes as WM_KEYDOWN + WM_CHAR ----
        if (active_window >= 0 && windows[active_window].app == APP_WIN32) {
            Win11Window& wa = windows[active_window];
            if (wa.w32_index >= 0) {
                uint32_t vk = (uint32_t)(uint8_t)ch;
                if (ch >= 'a' && ch <= 'z') vk = (uint32_t)(ch - 'a' + 'A');
                if (ch == '\n' || ch == 0x0D) vk = 0x0D;
                if (ch == 8) vk = 0x08;
                serial_puts("[gui] win32 WM_CHAR ch="); serial_putdec((int)(unsigned char)ch); serial_puts("\n");
                win32_window_dispatch(wa.w32_index, 0x0100, vk, 1);              // WM_KEYDOWN
                // TranslateMessage() also synthesises a WM_CHAR for the control
                // keys (VK_BACK -> 8, VK_TAB -> 9, VK_RETURN -> 13), so we do
                // too.  A window procedure must therefore act on a backspace in
                // exactly ONE of the two messages, never both.
                win32_window_dispatch(wa.w32_index, 0x0102, (uint32_t)(uint8_t)ch, 1); // WM_CHAR
                win32_window_dispatch(wa.w32_index, 0x0101, vk, 1);              // WM_KEYUP
                win32_window_repaint(wa.w32_index);
                render_all();
            }
            return true;
        }

        // ---- Managed (C#) app: route the key into NexOS.Forms ----
        // ch is ASCII from the keyboard driver; translate the few control
        // codes into mforms' negative virtual keys (-1 bksp, -2 enter).
        if (active_window >= 0 && windows[active_window].app == APP_MANAGED) {
            Win11Window& wa = windows[active_window];
            // Chinese IME composer: pinyin is composed here and committed
            // glyphs are delivered to the focused C# text box via mforms_key.
            if (ime_route(ch, nullptr, nullptr, 0)) {
                render_all();
                return true;
            }
            int k = 0;
            if (ch == 0x08)                        k = -1;   // backspace
            else if (ch == '\n' || ch == 0x0D)     k = -2;   // enter
            else if ((unsigned char)ch >= 0x20 &&
                     (unsigned char)ch < 0x7F)     k = (int)(unsigned char)ch;
            if (k != 0) {
                mforms_key(wa.managed_app, k);
                render_all();
            }
            return true;
        }

        // Handle terminal input
        if (active_window >= 0 && windows[active_window].app == APP_TERMINAL) {
            Win11Window& win = windows[active_window];
            // Chinese IME composer consumes letters/space/digit/backspace.
            if (ime_route(ch, win.term_input, &win.term_input_len, 127)) {
                render_all();
                return true;
            }
            if (ch == '\n' || ch == 0x0D) { // Enter
                if (win.term_input_len > 0) {
                    // Add command to output
                    win.term_buf[win.term_len++] = '>';
                    for (int i = 0; i < win.term_input_len; i++)
                        win.term_buf[win.term_len++] = win.term_input[i];
                    win.term_buf[win.term_len++] = '\n';

                    // Execute command via kernel callback (full shell)
                    if (g_cb.exec_command) {
                        static char cmd_out[2048];
                        g_cb.exec_command(win.term_input, cmd_out, sizeof(cmd_out));
                        // Append output to terminal buffer
                        for (int i = 0; cmd_out[i] && win.term_len < (int)sizeof(win.term_buf) - 2; i++)
                            win.term_buf[win.term_len++] = cmd_out[i];
                    } else {
                        // Fallback: simple built-in commands
                        if (strcmp_(win.term_input, "help") == 0) {
                            const char* msg = "Commands: help, mem, clear, exit, ls, cat, etc.\nFull shell commands available.\n";
                            while (*msg) win.term_buf[win.term_len++] = *msg++;
                        } else if (strcmp_(win.term_input, "clear") == 0) {
                            win.term_len = 0;
                            win.term_buf[0] = 0;
                        } else if (strcmp_(win.term_input, "exit") == 0) {
                            close_window(active_window);
                        } else {
                            const char* msg = "Shell not available. Try: help\n";
                            while (*msg) win.term_buf[win.term_len++] = *msg++;
                        }
                    }
                    win.term_buf[win.term_len] = 0;
                    win.term_input_len = 0;
                    win.term_input[0] = 0;
                }
                render_all();
                return true;
            } else if (ch == 0x08) { // Backspace
                if (win.term_input_len > 0) {
                    memcpy_(win.term_undo, win.term_input, 128);
                    win.term_undo_len = win.term_input_len;
                    win.term_input_len--;
                    win.term_input[win.term_input_len] = 0;
                    render_all();
                }
                return true;
            } else if (ch >= 32 && ch < 127 && win.term_input_len < 126) {
                memcpy_(win.term_undo, win.term_input, 128);
                win.term_undo_len = win.term_input_len;
                win.term_input[win.term_input_len++] = ch;
                win.term_input[win.term_input_len] = 0;
                render_all();
                return true;
            }
        }
        // Handle browser URL input
        else if (active_window >= 0 && windows[active_window].app == APP_BROWSER) {
            Win11Window& win = windows[active_window];
            browser::ensure_init();
            // Chinese IME composer for the address bar.
            if (ime_route(ch, win.browser_url, &win.browser_url_len, 255)) {
                serial_puts("[browser] addr="); serial_puts(win.browser_url); serial_puts("\n");
                render_all();
                return true;
            }
            if (win.browser_url_focused) {
                if (ch == '\n' || ch == 0x0D) { // Enter = navigate
                    if (win.browser_url_len > 0) {
                        browser::navigate(browser::g_active, win.browser_url);
                        win.browser_url_focused = false;
                    }
                    render_all();
                    return true;
                } else if (ch == 0x08) { // Backspace
                    if (win.browser_url_len > 0) {
                        memcpy_(win.url_undo, win.browser_url, 256);
                        win.url_undo_len = win.browser_url_len;
                        win.browser_url_len--;
                        win.browser_url[win.browser_url_len] = 0;
                        serial_puts("[browser] addr="); serial_puts(win.browser_url); serial_puts("\n");
                        render_all();
                    }
                    return true;
                } else if (ch >= 32 && ch < 127 && win.browser_url_len < 254) {
                    memcpy_(win.url_undo, win.browser_url, 256);
                    win.url_undo_len = win.browser_url_len;
                    win.browser_url[win.browser_url_len++] = ch;
                    win.browser_url[win.browser_url_len] = 0;
                    serial_puts("[browser] addr="); serial_puts(win.browser_url); serial_puts("\n");
                    render_all();
                    return true;
                } else if (ch == 0x1B) { // Escape = unfocus
                    win.browser_url_focused = false;
                    render_all();
                    return true;
                }
            }
        }
        return false;
    }

    // ---- Ctrl+key shortcuts (Ctrl+C / Ctrl+V / Ctrl+Z / Ctrl+A) ----
    // Delivered by the GUI event loop via gui_handle_ctrl(code).
    //   code 1 = Ctrl+C   2 = Ctrl+V   3 = Ctrl+Z   4 = Ctrl+A
    void handle_ctrl(int code) {
        if (!gui_mode) return;

        // ---- Desktop inline-rename editor (no window focused) ----
        // The desktop key path already forwards every keystroke to
        // Desktop.Key(), which is a no-op unless a rename is in progress.
        // Mirror that for the Ctrl combos so Ctrl+Z/Ctrl+A reach the
        // rename box too.  Desktop.Key ignores them when not renaming.
        if (active_window < 0) {
            int k = 0;
            if      (code == 1) k = -3;   // Ctrl+C -> copy whole name
            else if (code == 2) k = -4;   // Ctrl+V -> paste
            else if (code == 3) k = -5;   // Ctrl+Z -> undo
            else if (code == 4) k = -6;   // Ctrl+A -> select all
            if (k != 0) { mforms_desktop_key(k); render_all(); }
            return;
        }
        if (active_window >= window_count) return;
        Win11Window& w = windows[active_window];

        // ---- Managed (C#) app: forward as negative virtual keys (-3..-6) ----
        if (w.app == APP_MANAGED) {
            int k = 0;
            if      (code == 1) k = -3;   // Ctrl+C -> copy
            else if (code == 2) k = -4;   // Ctrl+V -> paste
            else if (code == 3) k = -5;   // Ctrl+Z -> undo
            else if (code == 4) k = -6;   // Ctrl+A -> select all
            if (k != 0) { mforms_key(w.managed_app, k); render_all(); }
            return;
        }

        // ---- GUI Terminal window ----
        if (w.app == APP_TERMINAL) {
            if (code == 1) {              // Ctrl+C: copy input line
                if (w.term_input_len > 0) clipboard_set(w.term_input, w.term_input_len);
                render_all();
            } else if (code == 2) {       // Ctrl+V: paste clipboard
                for (int i = 0; i < g_clipboard_len && w.term_input_len < 126; i++) {
                    char c = g_clipboard[i];
                    if (c == '\n' || c == '\r') continue;
                    w.term_input[w.term_input_len++] = c;
                }
                w.term_input[w.term_input_len] = 0;
                render_all();
            } else if (code == 3) {       // Ctrl+Z: undo
                if (w.term_undo_len >= 0) {
                    int n = w.term_undo_len; if (n > 128) n = 128;
                    for (int i = 0; i < n; i++) w.term_input[i] = w.term_undo[i];
                    w.term_input_len = n;
                    w.term_input[w.term_input_len] = 0;
                    w.term_undo_len = -1;
                    render_all();
                }
            } else if (code == 4) {       // Ctrl+A: select all -> copy whole line
                if (w.term_input_len > 0) clipboard_set(w.term_input, w.term_input_len);
                render_all();
            }
            return;
        }

        // ---- Browser URL bar (when focused) ----
        if (w.app == APP_BROWSER && w.browser_url_focused) {
            if (code == 1) {              // Ctrl+C: copy URL
                if (w.browser_url_len > 0) clipboard_set(w.browser_url, w.browser_url_len);
                render_all();
            } else if (code == 2) {       // Ctrl+V: paste
                for (int i = 0; i < g_clipboard_len && w.browser_url_len < 254; i++) {
                    char c = g_clipboard[i];
                    if (c == '\n' || c == '\r') continue;
                    w.browser_url[w.browser_url_len++] = c;
                }
                w.browser_url[w.browser_url_len] = 0;
                serial_puts("[browser] addr="); serial_puts(w.browser_url); serial_puts("\n");
                render_all();
            } else if (code == 3) {       // Ctrl+Z: undo
                if (w.url_undo_len >= 0) {
                    int n = w.url_undo_len; if (n > 256) n = 256;
                    for (int i = 0; i < n; i++) w.browser_url[i] = w.url_undo[i];
                    w.browser_url_len = n;
                    w.browser_url[w.browser_url_len] = 0;
                    w.url_undo_len = -1;
                    serial_puts("[browser] addr="); serial_puts(w.browser_url); serial_puts("\n");
                    render_all();
                }
            } else if (code == 4) {       // Ctrl+A: select all -> copy
                if (w.browser_url_len > 0) clipboard_set(w.browser_url, w.browser_url_len);
                render_all();
            }
            return;
        }

        // ---- Native Win32 app (e.g. 32-bit IE): forward the combo as a
        //      private message (WM_NexOS_CTRL = 0x8001, see winpe/minipe.h).
        //      The PE's window procedure maps code 1..4 to copy/paste/undo/
        //      select-all on its focused field. ----
        if (w.app == APP_WIN32 && w.w32_index >= 0) {
            serial_puts("[gui] win32 handle_ctrl code="); serial_putdec(code); serial_puts("\n");
            win32_window_dispatch(w.w32_index, 0x8001 /*WM_NexOS_CTRL*/, code, 0);
            render_all();
            return;
        }
    }

    // ---- String helper ----
    void strcat_safe(char* dst, const char* src, int dstsize) {
        int dl = strlen_(dst);
        int sl = strlen_(src);
        if (dl + sl >= dstsize) sl = dstsize - dl - 1;
        if (sl <= 0) return;
        memcpy_(dst + dl, src, sl);
        dst[dl + sl] = 0;
    }
};

} // anonymous namespace

// =====================================================================
//  C interface for kernel integration
// =====================================================================
static Win11Desktop g_wm;

// =====================================================================
//  NexOS.Forms host bridge
// ---------------------------------------------------------------------
//  g_wm.gfx exposes its drawing primitives as *member* functions, and
//  both Graphics and Win11Desktop live in an anonymous namespace, so
//  mforms.cpp cannot reach them directly.  These free thunks bounce each
//  call onto the live window manager (plain C linkage so their address
//  fits an MFormsHost slot), and mforms_boot() packs them together with
//  the machine-state callbacks the native apps already use.
// =====================================================================
#if NexOS_HAVE_MFORMS
namespace {

void mh_fill_rect  (int x,int y,int w,int h,uint32_t c){ g_wm.gfx.fill_rect(x,y,w,h,c); }
void mh_fill_round (int x,int y,int w,int h,int r,uint32_t c){ g_wm.gfx.fill_rounded_rect(x,y,w,h,r,c); }
void mh_draw_round (int x,int y,int w,int h,int r,uint32_t c){ g_wm.gfx.draw_rounded_rect(x,y,w,h,r,c); }
void mh_draw_rect  (int x,int y,int w,int h,uint32_t c){ g_wm.gfx.draw_rect(x,y,w,h,c); }
void mh_draw_line  (int x0,int y0,int x1,int y1,uint32_t c){ g_wm.gfx.draw_line(x0,y0,x1,y1,c); }
void mh_fill_grad  (int x,int y,int w,int h,uint32_t t,uint32_t b){ g_wm.gfx.fill_gradient(x,y,w,h,t,b); }
void mh_text       (int x,int y,const char* s,uint32_t fg){ g_wm.gfx.draw_text_utf8_transparent(x,y,s,fg); }
void mh_text_bg    (int x,int y,const char* s,uint32_t fg,uint32_t bg){ g_wm.gfx.draw_text_utf8(x,y,s,fg,bg); }
void mh_fill_circle(int cx,int cy,int r,uint32_t c){ g_wm.gfx.fill_circle(cx,cy,r,c); }
void mh_draw_circle(int cx,int cy,int r,uint32_t c){ g_wm.gfx.draw_circle(cx,cy,r,c); }
void mh_icon       (int x,int y,int sz,uint32_t bg,char letter,uint32_t lc){ g_wm.gfx.draw_icon(x,y,sz,bg,letter,lc); }
void mh_progress   (int x,int y,int w,int h,int pct,uint32_t c){ g_wm.gfx.draw_progress(x,y,w,h,pct,c); }

// Pixel width of a UTF-8 string: ASCII glyphs advance 8px, 3-byte CJK
// 16px, 2-byte sequences carry no glyph (matches draw_text_utf8).
int mh_measure(const char* s){
    int w = 0;
    if (!s) return 0;
    while (*s){
        unsigned char c = (unsigned char)*s;
        if (c < 0x80)                                      { w += 8;  s += 1; }
        else if ((c & 0xF0)==0xE0 && (s[1]&0xC0)==0x80 &&
                                      (s[2]&0xC0)==0x80)   { w += 16; s += 3; }
        else if ((c & 0xE0)==0xC0 && (s[1]&0xC0)==0x80)    {          s += 2; }
        else                                               {          s += 1; }
    }
    return w;
}

// Return-type adapters: g_cb hands these back as bool / uint32_t, but the
// MFormsHost slots are int.
int mh_is_64bit(void){ return (g_cb.is_64bit && g_cb.is_64bit()) ? 1 : 0; }
int mh_pci_count(void){ return g_cb.get_pci_count ? (int)g_cb.get_pci_count() : 0; }

// Monotonic milliseconds for managed double-click detection.  Uses the
// PIT-calibrated TSC rate when calibrate_tsc() succeeded; otherwise falls
// back to a frame counter (~16 ms/frame is plenty for a 500 ms window).
uint32_t mh_tick_ms(void) { return g_wm.tick_ms_now(); }

// Managed code (e.g. the File Explorer's "Open"/"Edit" actions) asks the
// kernel to open or focus an application of a managed Kind.
void mh_open_app(int kind) { g_wm.launch_app(g_wm.app_for_managed_kind(kind)); }
void mh_close_app(int kind) {
    AppType a = g_wm.app_for_managed_kind(kind);
    if (a != APP_NONE) g_wm.close_window_for_kind(a);
}
void mh_exit_gui(void) { gui_exit(); }

// ---- Texture cache: load SFS .tex files once (tools/tex_pack.py) ----
static bool g_tex_loaded = false;

static inline uint32_t tex_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool tex_load_one(int id, const char* file) {
    if (id < 0 || id >= TEX_MAX) return false;
    TexRec& t = g_tex[id];
    if (t.loaded) return true;
    t.loaded = false; t.data = 0; t.w = t.h = 0; t.fmt = 0;
    if (!g_cb.read_file) return false;
    const int kMaxFile = 2 * 1024 * 1024;
    uint8_t* buf = (uint8_t*)kmalloc(kMaxFile);
    if (!buf) return false;
    int n = g_cb.read_file(1, file, buf, kMaxFile);
    if (n >= 24 && buf[0] == 'T' && buf[1] == 'E' && buf[2] == 'X' && buf[3] == '1') {
        const uint8_t* p = buf + 4;
        uint32_t ver   = tex_rd32(p); p += 4;
        uint32_t w     = tex_rd32(p); p += 4;
        uint32_t h     = tex_rd32(p); p += 4;
        uint32_t fmt   = tex_rd32(p); p += 4;
        uint32_t bytes = tex_rd32(p); p += 4;
        if (ver == 1 && w > 0 && h > 0 && w <= 4096 && h <= 4096 &&
            fmt <= 1 && bytes <= (uint32_t)n - 24) {
            uint8_t* data = (uint8_t*)kmalloc(bytes ? bytes : 1);
            if (data) {
                for (uint32_t i = 0; i < bytes; i++) data[i] = p[i];
                t.w = (uint16_t)w; t.h = (uint16_t)h; t.fmt = (uint8_t)fmt;
                t.data = data; t.loaded = true;
            }
        }
    }
    kfree(buf);
    if (!t.loaded) {
        serial_puts("[TEX] load failed: "); serial_puts(file); serial_puts("\n");
    }
    return t.loaded;
}

static void tex_load_all(void) {
    if (g_tex_loaded) return;
    g_tex_loaded = true;
    static const char* names[] = {
        "tex_wall.tex", "tex_task.tex", "tex_menu.tex",
        "tex_chrome.tex", "tex_winbg.tex",
    };
    for (int i = 0; i < 5 && i < TEX_MAX; i++) tex_load_one(i, names[i]);
    static const char* icons[] = {
        "tex_k0.tex", "tex_k1.tex", "tex_k2.tex", "tex_k3.tex", "tex_k4.tex",
        "tex_k5.tex", "tex_k6.tex", "tex_k7.tex", "tex_k8.tex",
    };
    for (int i = 0; i < 9; i++) {
        int id = TEX_ICON + i;
        if (id < TEX_MAX) tex_load_one(id, icons[i]);
    }
}

void mh_image(int id, int x, int y, int w, int h) {
    if (id < 0 || id >= TEX_MAX || !g_tex[id].loaded) return;
    g_wm.gfx.draw_image(g_tex[id], x, y, w, h);
}
int mh_has_image(int id) {
    return (id >= 0 && id < TEX_MAX && g_tex[id].loaded) ? 1 : 0;
}

// Managed code (File Explorer double-click, desktop shortcut, "Run" in a
// context menu) asks the kernel to EXECUTE a real Windows PE image.  This
// is the same code path as typing `winapp foo.exe` at the prompt: the PE
// loader maps the image, resolves its imports against the Win32 API that
// win32.cpp implements, calls the entry point, and any window the program
// created is surfaced on the desktop.
int mh_run_exe(const char* filename) {
    if (!filename || !filename[0]) return -1;
    return g_wm.launch_pe_exe(filename);
}

} // namespace

// Load the managed shell (shell.mex) and publish the host table.  Idempotent
// and cheap after the first success; a failed start is not retried so a
// missing image cannot stall every frame.  Requires clr_ensure_init() to
// have run and gui_set_callbacks() to have filled g_cb (both happen in
// cmd_gui before gui_enter()).
static bool g_mforms_booted = false;

static void mforms_boot(void) {
    if (g_mforms_booted) return;
    g_mforms_booted = true;

    tex_load_all();     // SFS textures (fallback: flat colours if absent)

    MFormsHost h;
    // ---- drawing ----
    h.fill_rect    = mh_fill_rect;
    h.fill_round   = mh_fill_round;
    h.draw_round   = mh_draw_round;
    h.draw_rect    = mh_draw_rect;
    h.draw_line    = mh_draw_line;
    h.fill_grad    = mh_fill_grad;
    h.text         = mh_text;
    h.text_bg      = mh_text_bg;
    h.fill_circle  = mh_fill_circle;
    h.draw_circle  = mh_draw_circle;
    h.icon         = mh_icon;
    h.progress     = mh_progress;
    h.has_image    = mh_has_image;
    h.image        = mh_image;
    h.measure      = mh_measure;
    h.screen_w     = g_wm.gfx.width;
    h.screen_h     = g_wm.gfx.height;

    // ---- machine state (same callbacks the legacy native apps read) ----
    h.mem_total_kb     = g_cb.get_total_mem_kb;
    h.mem_free_pages   = g_cb.get_free_pages;
    h.mem_used_pages   = g_cb.get_used_pages;
    h.mem_total_pages  = g_cb.get_total_pages;
    h.heap_alloc_bytes = g_cb.get_heap_alloc_bytes;
    h.heap_free_bytes  = g_cb.get_heap_free_bytes;
    h.heap_alloc_count = g_cb.get_heap_alloc_count;
    h.heap_free_count  = g_cb.get_heap_free_count;
    h.optimize_memory  = g_cb.optimize_memory;
    h.tick_ms          = mh_tick_ms;
    h.list_files       = g_cb.list_files;
    h.read_file        = g_cb.read_file;
    h.get_time         = g_cb.get_time;
    h.mkdir            = g_cb.mkdir;
    h.remove           = g_cb.remove;
    h.rename           = g_cb.rename;
    h.http_get         = g_cb.http_get;
    h.os_name          = g_cb.get_os_name;
    h.cpu_vendor       = g_cb.get_cpu_vendor;
    h.disk_model       = g_cb.get_disk_model;
    h.disk_size_mb     = g_cb.get_disk_size_mb;
    h.is_64bit         = mh_is_64bit;
    h.pci_count        = mh_pci_count;
    h.nic_present      = g_cb.get_nic_present;
    h.exec_command     = g_cb.exec_command;
    h.shutdown         = g_cb.shutdown;
    h.reboot           = g_cb.reboot;
    h.open_app         = mh_open_app;
    h.close_app        = mh_close_app;
    h.exit_gui         = mh_exit_gui;
    h.run_exe          = mh_run_exe;
    h.login_check      = g_cb.login_check;
    h.login_uid        = g_cb.login_uid;
    h.user_count       = g_cb.user_count;
    h.user_name        = g_cb.user_name;

    mforms_init(&h);
    mforms_start();
    serial_puts("[MFORMS] ");
    serial_puts(mforms_report());
    serial_puts("\n");
}
#else   // !NexOS_HAVE_MFORMS  (64-bit kernel: native drawers only)
static void mforms_boot(void) {}
#endif

// ---- Reinitialize VGA text mode (fix striped screen on GUI exit) ----
static void vga_set_text_mode(void) {
    // Miscellaneous Output
    outb(0x3C2, 0x67);

    // Sequencer (0x3C4/0x3C5): 5 regs
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);
    outb(0x3C4, 0x01); outb(0x3C5, 0x01);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);
    outb(0x3C4, 0x04); outb(0x3C5, 0x02);

    // CRTC (0x3D4/0x3D5): unlock reg 0x11 first, then 25 regs
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    uint8_t crtc_regs[25] = {0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F, 0x00, 0x4F, 0x0E, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF};
    for (int i = 0; i < 25; i++) {
        outb(0x3D4, (uint8_t)i); outb(0x3D5, crtc_regs[i]);
    }

    // Graphics (0x3CE/0x3CF): 9 regs
    uint8_t gfx_regs[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF};
    for (int i = 0; i < 9; i++) {
        outb(0x3CE, (uint8_t)i); outb(0x3CF, gfx_regs[i]);
    }

    // Attribute (0x3C0): read 0x3DA first, then 21 regs, then finalize
    inb(0x3DA);
    uint8_t attr_regs[21] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x0C, 0x00, 0x0F, 0x08, 0x00};
    for (int i = 0; i < 21; i++) {
        outb(0x3C0, attr_regs[i]);
    }
    outb(0x3C0, 0x20);
}

extern "C" {

// Forward declarations for framebuffer console functions (defined later)
void fb_console_render(void);
void fb_console_clear(void);
void fb_console_force_redraw(void);

void gui_set_callbacks(const GuiCallbacks* cb) {
    if (cb) g_cb = *cb;
}

int gui_init(void) {
    g_wm.init();
    return g_wm.gfx.initialized ? 0 : -1;
}

extern "C" void gui_set_startup_app(int id) { g_startup_app_id = id; }
extern "C" int  gui_app_browser_id(void) { return (int)APP_BROWSER; }

// Resolve a startup-app keyword to an AppType index, or -1 if unknown.
// Lets `gui <name>` open straight into an app (handy for the managed apps).
extern "C" int gui_app_id_by_name(const char* n) {
    if (!n || !n[0]) return -1;
    struct { const char* k; AppType a; } map[] = {
        { "control",  APP_CONTROL_PANEL }, { "cp",     APP_CONTROL_PANEL },
        { "files",    APP_FILE_EXPLORER }, { "explorer", APP_FILE_EXPLORER },
        { "tasks",    APP_TASK_MANAGER },  { "taskmgr", APP_TASK_MANAGER },
        { "terminal", APP_TERMINAL },      { "term",   APP_TERMINAL },
        { "calc",     APP_CALCULATOR },    { "calculator", APP_CALCULATOR },
        { "about",    APP_ABOUT },
        { "memory",   APP_MEM_OPTIMIZER }, { "mem",    APP_MEM_OPTIMIZER },
        { "browser",  APP_BROWSER },
        { "notepad",  APP_NOTEPAD },       { "text",   APP_NOTEPAD },
        { "editor",   APP_NOTEPAD },
        { "agent",    APP_AIAGENT },
    };
    for (unsigned i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        const char* a = n; const char* b = map[i].k;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b) return (int)map[i].a;
    }
    return -1;
}

// ---- Lightweight VBE probe: only writes 0x5000 VBE info (no kmalloc) ----
// Called early at boot (after heap_init). gfx.init() runs the full init later
// when the user actually types `gui`.
extern "C" void gui_probe_vbe(void){
    volatile VbeInfo* info = (volatile VbeInfo*)0x5000;
    if (info->vbe_ok == 1) return;                 // BIOS/UEFI already filled it
    if (!bga_detect()) return;                       // no BGA fallback available
    info->vbe_ok           = 1;
    info->vbe_mode_set     = 1;                     // will be set via BGA later
    info->width            = 1024;
    info->height           = 768;
    info->bpp              = 32;
    info->pitch            = 1024 * 4;
    info->mode_number      = 0x117;
    info->pixel_format     = PXF_BGRX32;
    info->framebuffer_phys64 = 0xE0000000ULL;
    info->framebuffer_phys   = 0xFD000000;
    // Mark shadow_buffer non-zero so Graphics::init does NOT replace lfb with a
    // heap shadow. The VMM already identity-maps the BGA LFB (0xE0000000) into
    // the page directory, so the kernel can write it directly.
    info->shadow_buffer    = 1;
    serial_puts("[GUI] BGA fallback: synthesized VBE info (1024x768x32, BGRX32)\n");
}

int gui_available(void) {
    return g_wm.gfx.initialized ? 1 : 0;
}

#ifdef FB_DIAG
// =====================================================================
//  Framebuffer diagnostic (white-line hunter)
//  Bypasses the backbuffer/compositor and writes the LFB directly so we can
//  isolate framebuffer *addressing* (fb_base, pitch, pixel format) from the
//  GUI renderer.  Runs once at GUI entry, then halts so the screen can be
//  inspected / captured on real hardware.
//
//  Build variants (make uefi-diag FB_TEST=n):
//     (none)  vertical rainbow bars via REAL pitch  -> reveals fb_base/pitch/format at once
//     FB_TEST=1  red square top-left (test 1: is fb_base writable?)
//     FB_TEST=2  full-screen solid blue (test 3: range/pitch sanity)
//     FB_TEST=3  bars via width*4 pitch (test 2: does the white line vanish?)
// =====================================================================
static void fb_diag_setp(int x, int y, uint32_t rgb, uint32_t eff_pitch) {
    Graphics& g = g_wm.gfx;
    if (!g.lfb || x < 0 || y < 0 ||
        (uint32_t)x >= g.width || (uint32_t)y >= g.height) return;
    uint8_t* p = (uint8_t*)g.lfb + (uint32_t)y * eff_pitch;
    if (g.pixel_format == PXF_RGBX32) {
        p[x*4]   = (uint8_t)((rgb >> 16) & 0xFF);
        p[x*4+1] = (uint8_t)((rgb >> 8)  & 0xFF);
        p[x*4+2] = (uint8_t)( rgb        & 0xFF);
        p[x*4+3] = 0;
    } else if (g.pixel_format == PXF_RGB24 || g.bpp == 24) {
        p[x*3]   = (uint8_t)( rgb        & 0xFF);
        p[x*3+1] = (uint8_t)((rgb >> 8)  & 0xFF);
        p[x*3+2] = (uint8_t)((rgb >> 16) & 0xFF);
    } else if (g.pixel_format == PXF_RGB565 || g.bpp == 16) {
        uint16_t* q = (uint16_t*)p;
        uint32_t c = rgb;
        q[x] = (uint16_t)((((c >> 19) & 0x1F) << 11) |
                          (((c >> 10) & 0x3F) << 5) | ((c >> 3) & 0x1F));
    } else {
        ((uint32_t*)p)[x] = rgb;   /* BGRX32 (most common) */
    }
}

static void gui_fb_diag(void) {
    Graphics& g = g_wm.gfx;
    if (!g.initialized || !g.lfb) {
        serial_puts("[DIAG] framebuffer not initialized - abort\n");
        return;
    }
    serial_puts("[DIAG] === framebuffer diagnostic ===\n");
    serial_puts("[DIAG] lfb=0x"); serial_puthex32((uint32_t)g.lfb);
    serial_puts(" w="); serial_putdec((int)g.width);
    serial_puts(" h="); serial_putdec((int)g.height);
    serial_puts(" pitch="); serial_putdec((int)g.pitch);
    serial_puts(" w*4="); serial_putdec((int)g.width * 4);
    serial_puts(" bpp="); serial_putdec((int)g.bpp);
    serial_puts(" fmt="); serial_putdec((int)g.pixel_format);
    serial_puts("\n");

    uint32_t eff_pitch = g.pitch;
    int variant = 0;
#ifdef FB_TEST
    variant = FB_TEST;
#endif
    if (variant == 3) {
        eff_pitch = (uint32_t)g.width * 4u;
        serial_puts("[DIAG] variant=3 (test2) override pitch=width*4\n");
    } else if (variant == 1) {
        serial_puts("[DIAG] variant=1 (test1) red square top-left\n");
    } else if (variant == 2) {
        serial_puts("[DIAG] variant=2 (test3) full-screen solid blue\n");
    } else {
        serial_puts("[DIAG] variant=0 (default) vertical rainbow bars\n");
    }

    /* Clear to black first. */
    for (uint32_t y = 0; y < g.height; y++)
        for (uint32_t x = 0; x < g.width; x++)
            fb_diag_setp((int)x, (int)y, 0x000000, eff_pitch);

    if (variant == 1) {
        /* Test 1: red 80x80 square at top-left to confirm fb_base is writable. */
        uint32_t s = g.width < 80 ? g.width : 80;
        if (g.height < s) s = g.height;
        for (uint32_t y = 0; y < s; y++)
            for (uint32_t x = 0; x < s; x++)
                fb_diag_setp((int)x, (int)y, 0xFF0000, eff_pitch);
    } else if (variant == 2) {
        /* Test 3: full-screen solid blue to expose pitch/range artifacts. */
        for (uint32_t y = 0; y < g.height; y++)
            for (uint32_t x = 0; x < g.width; x++)
                fb_diag_setp((int)x, (int)y, 0x0000FF, eff_pitch);
    } else {
        /* Default + test 2: vertical rainbow bars. */
        static const uint32_t bars[8] = {
            0xFF0000, 0xFF8800, 0xFFFF00, 0x00FF00,
            0x00FFFF, 0x0000FF, 0x8800FF, 0xFF00FF
        };
        uint32_t bw = g.width / 8;
        if (bw < 1) bw = 1;
        for (uint32_t y = 0; y < g.height; y++) {
            for (uint32_t x = 0; x < g.width; x++) {
                uint32_t bi = x / bw;
                if (bi > 7) bi = 7;
                fb_diag_setp((int)x, (int)y, bars[bi], eff_pitch);
            }
        }
    }

    serial_puts("[DIAG] pattern drawn - halting for inspection\n");
    for (;;) asm volatile("hlt" ::: "memory");
}
#endif /* FB_DIAG */

void gui_enter(void) {
#ifdef FB_DIAG
    gui_fb_diag();          // draw raw test pattern and halt for inspection
#endif
    mforms_boot();          // bring the managed (C#) shell online before drawing
    g_wm.enter_gui();
}

int gui_is_active(void) {
    return g_wm.gui_mode ? 1 : 0;
}

void gui_mouse_move(int dx, int dy) {
    g_wm.handle_mouse_move(dx, dy);
}

void gui_mouse_down(void) {
    g_wm.handle_mouse_down();
}

void gui_mouse_up(void) {
    g_wm.handle_mouse_up();
}

void gui_mouse_down_right(void) {
    g_wm.handle_mouse_down_right();
}

int gui_handle_key(char ch) {
    return g_wm.handle_key(ch) ? 1 : 0;
}

void gui_handle_ctrl(int code) {
    g_wm.handle_ctrl(code);
}

// Toggle Chinese/English input mode (bound to the Shift key).
void gui_toggle_ime(void) {
    g_wm.gui_toggle_ime();
}

int gui_get_width(void)  { return g_wm.gfx.width; }
int gui_get_height(void) { return g_wm.gfx.height; }

void gui_create_window(int x, int y, int w, int h, const char* title) {
    g_wm.create_window(x, y, w, h, title, APP_NONE);
    g_wm.render_all();
}

void gui_draw_text(int x, int y, const char* text) {
    if (!g_wm.gui_mode) return;
    g_wm.gfx.draw_text_transparent(x, y, text, C_WIN_TEXT);
}

void gui_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_wm.gui_mode) return;
    g_wm.gfx.fill_rect(x, y, w, h, color);
}

// ---- Tick: update internal time but no longer draw the top-bar clock ----
// The clock was removed from the top-right corner by request; the language
// indicator (中 / EN) is drawn by draw_topbar() and refreshed on IME toggle.
void gui_tick(void) {
    if (!g_wm.gui_mode) return;
    g_wm.update_clock();
}

void gui_animate_frame(void) {
    if (!g_wm.gui_mode) return;
    g_wm.animate_frame();
}

// ---- Render VGA text buffer to framebuffer (for text mode overlay) ----
void gui_render_text_mode(void) {
    if (!g_wm.gfx.initialized || g_wm.gui_mode) return;
    static const uint32_t vga_pal[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
    };
    g_wm.gfx.clear_screen(COLOR_BLACK);
    volatile uint16_t* vga = (volatile uint16_t*)0xB8000;
    int text_w = 80 * 8;
    int text_h = 25 * 16;
    int off_x = (g_wm.gfx.width - text_w) / 2;
    int off_y = (g_wm.gfx.height - text_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;
    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < 80; col++) {
            uint16_t entry = vga[row * 80 + col];
            char ch = (char)(entry & 0xFF);
            uint8_t attr = (entry >> 8) & 0xFF;
            uint32_t fg = vga_pal[attr & 0x0F];
            uint32_t bg = vga_pal[(attr >> 4) & 0x0F];
            g_wm.gfx.draw_char(off_x + col * 8, off_y + row * 16, ch, fg, bg);
        }
    }
}

// ---- Session persistence ----------------------------------------------
// On exit, the list of running windows is written to durable storage (the
// MKFS data disk via the kernel callback); on the next GUI entry they are
// reopened at the same position.  This is the lightweight "hibernate" the
// shell offers: quit the GUI -> reboot -> the apps come back.
extern "C" int gui_session_save(void) {
    if (!g_cb.session_save) return -1;
    uint8_t buf[2 + MAX_WINDOWS * 9];
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_wm.windows[i].visible && g_wm.windows[i].launch_kind != APP_NONE)
            n++;
    if (n == 0) return 0;              // nothing running: no session to keep
    buf[0] = 'S';
    buf[1] = (uint8_t)n;
    int p = 2;
    for (int i = 0; i < MAX_WINDOWS && p + 9 <= (int)sizeof(buf); i++) {
        Win11Window& w = g_wm.windows[i];
        if (!w.visible || w.launch_kind == APP_NONE) continue;
        buf[p++] = (uint8_t)w.launch_kind;
        buf[p++] = (uint8_t)(w.x & 0xFF); buf[p++] = (uint8_t)((w.x >> 8) & 0xFF);
        buf[p++] = (uint8_t)(w.y & 0xFF); buf[p++] = (uint8_t)((w.y >> 8) & 0xFF);
        buf[p++] = (uint8_t)(w.w & 0xFF); buf[p++] = (uint8_t)((w.w >> 8) & 0xFF);
        buf[p++] = (uint8_t)(w.h & 0xFF); buf[p++] = (uint8_t)((w.h >> 8) & 0xFF);
    }
    int r = g_cb.session_save("session", buf, p);
    serial_puts("[SESS] saved "); serial_putdec(n); serial_puts(" windows\n");
    return r;
}

extern "C" int gui_session_restore(void) {
    if (!g_cb.session_load) return -1;
    uint8_t buf[2 + MAX_WINDOWS * 9];
    int rd = g_cb.session_load("session", buf, sizeof(buf));
    if (rd < 3 || buf[0] != 'S') return -1;
    int n = buf[1], p = 2, restored = 0;
    for (int i = 0; i < n && p + 9 <= rd; i++) {
        int kind = buf[p++];
        int x = buf[p] | ((int)buf[p + 1] << 8); p += 2;
        int y = buf[p] | ((int)buf[p + 1] << 8); p += 2;
        int ww = buf[p] | ((int)buf[p + 1] << 8); p += 2;
        int hh = buf[p] | ((int)buf[p + 1] << 8); p += 2;
        // The Terminal shortcut exits the GUI by design -- never restore it.
        if (kind == (int)APP_TERMINAL) continue;
        int before = g_wm.window_count;
        g_wm.launch_app((AppType)kind);
        if (g_wm.window_count > before) {
            Win11Window& w = g_wm.windows[g_wm.window_count - 1];
            w.x = x; w.y = y; w.w = ww; w.h = hh;
            restored++;
        }
    }
    serial_puts("[SESS] restored "); serial_putdec(restored); serial_puts(" windows\n");
    return restored;
}

// ---- Exit GUI mode ----
void gui_exit(void) {
    if (!g_wm.gui_mode) return;
    g_wm.cursor.hide(g_wm.gfx);
    g_wm.gui_mode = false;
    g_wm.drag_window = -1;
    g_wm.start_menu_open = false;
    // Keep windows but they're not visible in text mode

    // Persist the running-app list so a reboot can reopen them.
    gui_session_save();

    // Keep the VBE graphics mode + framebuffer console active and render the
    // text terminal onto the LFB, exactly as during boot (VGA_MEMORY already
    // points at the shadow buffer).  Disabling Bochs VBE and switching to raw
    // VGA text mode leaves a black screen on QEMU, so we no longer do that.
    // The dirty-cell renderer must be told to repaint everything: the LFB
    // still holds the desktop while the text snapshot matches VGA_MEMORY,
    // so without a forced redraw the terminal would never appear until some
    // cell changed (e.g. the mouse flipped a character).
    fb_console_force_redraw();
    fb_console_render();
    serial_puts("[GUI] Exited GUI mode, fbcon text mode (VBE stays active)\n");
}

// ---- Open a Windows executable inside the GUI (on-demand startup) ----
// Called by the shell's `run <file>` when the file is .exe/.bat/.ps1/.com.
// Initializes the GUI if it is not already active, then shows a window
// for the file.  The kernel otherwise boots to a pure command line.
void gui_open_file(const char* filename, const char* args) {
    (void)args;
    if (!g_wm.gfx.initialized) {
        serial_puts("[GUI] Initializing on demand for: ");
        serial_puts(filename ? filename : "(null)");
        serial_puts("\n");
        if (gui_init() != 0) { serial_puts("[GUI] init failed\n"); return; }
    }
    if (!g_wm.gui_mode) {
        g_wm.enter_gui();
        g_wm.render_all();
    }
    // 在 GUI 桌面中打开文件: 用 winloader 检测类型并创建信息窗口
    g_wm.open_file_in_gui(filename, args);
}

// ---- Force full redraw ----
void gui_render(void) {
    g_wm.render_all();
}

// =====================================================================
//  Launch a native Win32 (PE32) application in the GUI.
//  Called by the shell command `winapp <file.exe>`.
//    1. Boots the GUI if it is not running yet
//    2. win32_run() must already have been called by the caller (so the
//       console report is available); this only surfaces the windows
//  Returns the number of desktop windows created.
// =====================================================================
int gui_launch_win32(const char* filename) {
    if (!g_wm.gfx.initialized) {
        serial_puts("[GUI] Initializing on demand for Win32 app\n");
        if (gui_init() != 0) { serial_puts("[GUI] init failed\n"); return -1; }
    }
    if (!g_wm.gui_mode) {
        // Same bring-up as gui_enter(): the managed (C#) Win11 shell must
        // own the desktop, otherwise clicks land on the legacy portal
        // icons and double-click detection never runs.
        mforms_boot();
        g_wm.enter_gui();
        g_wm.render_all();
    }
    return g_wm.launch_win32_windows(filename);
}

// =====================================================================
//  Launch the real WinPE-style Chrome browser as a genuine PE32+ binary.
//  Mimics WinPE: a real Windows-64 PE image with a reduced Win32 API
//  surface (only the calls win32.cpp actually implements) that draws a
//  Chrome-style window via the GDI display list.  We route the desktop
//  "Browser" icon (and `gui browser`) through the exact same winapp /
//  win64_run path a user would type at the prompt, so the program is
//  loaded and executed by the PE loader rather than the native engine.
//  Returns the number of desktop windows created (0 if the PE failed to
//  load, e.g. iexplore.exe is missing from SFS).
// =====================================================================
int Win11Desktop::launch_pe_exe(const char* filename) {
    if (!filename || !filename[0]) return -1;
    serial_puts("[GUI] executing PE image via the win32/win64 loader: ");
    serial_puts(filename);
    serial_puts("\n");
    int rc = win32_run(filename, "", 0);
    if (rc != 0) {
        serial_puts("[GUI] win32_run(");
        serial_puts(filename);
        serial_puts(") failed rc=");
        serial_putdec(rc);
        serial_puts("\n");
        return rc;               // negative: caller decides on a fallback
    }
    int made = gui_launch_win32(filename);
    serial_puts("[GUI] ");
    serial_puts(filename);
    serial_puts(" surfaced ");
    serial_putdec(made);
    serial_puts(" desktop window(s)\n");
    return made;
}

// =====================================================================
//  Framebuffer Console (for real hardware without BGA)
//  Renders VGA text buffer (0xB8000) to VBE linear framebuffer.
//  Used when stage2.asm sets VBE mode via INT 10h (no BGA ports).
// =====================================================================

static volatile uint32_t* fb_lfb = nullptr;
static uint16_t fb_width = 0;
static uint16_t fb_height = 0;
static uint16_t fb_pitch = 0;
static uint8_t  fb_bpp = 0;
static uint8_t  fb_pixel_format = 0;  // 0=BGRX32, 1=RGBX32, 2=RGB24, 3=RGB565
static bool     fb_console_active = false;

// VGA 16-color palette -> 32-bit RGB
static uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

extern "C" void fb_console_init(void) {
    volatile VbeInfo* info = (volatile VbeInfo*)0x5000;
    if (info->vbe_ok != 1) {
        fb_console_active = false;
        return;
    }
    // Skip if BltOnly (no linear framebuffer)
    if (info->pixel_format == PXF_BLT_ONLY) {
        fb_console_active = false;
        serial_puts("[FBCON] BltOnly - no linear framebuffer\n");
        return;
    }
    fb_lfb = (volatile uint32_t*)info->framebuffer_phys;
    fb_width = info->width;
    fb_height = info->height;
    fb_pitch = info->pitch;
    fb_bpp = info->bpp;
    fb_pixel_format = info->pixel_format;
    fb_console_active = true;
    serial_puts("[FBCON] Framebuffer console initialized\n");
}

extern "C" int fb_console_available(void) {
    return fb_console_active ? 1 : 0;
}

// Offset of the centered terminal in framebuffer pixels.  Used by the
// Terminal mouse code in kernel.cpp so clicks land on the right character.
static int fb_off_x = 0;
static int fb_off_y = 0;

// ---- Dirty-cell rendering state --------------------------------------
// The classic fbcon repainted the WHOLE screen (clear LFB + all 80x25 cells)
// on every term.render(), which made the terminal visibly flicker.  We now
// keep a snapshot of the last-drawn VGA text buffer and only repaint cells
// that changed, clearing the LFB once at startup.
static uint16_t fb_last_cells[80 * 25];
static bool     fb_first_render = true;
static int      fb_last_cursor = -1;   // last software-cursor cell, -1 = none

// Write one 32-bit RGB pixel into the LFB honouring the real pixel format.
static void fb_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= (int)fb_width || y < 0 || y >= (int)fb_height) return;
    volatile uint8_t* pixel = (volatile uint8_t*)fb_lfb + (uint32_t)y * fb_pitch;
    if (fb_bpp == 32) {
        ((volatile uint32_t*)pixel)[x] = color;
    } else if (fb_bpp == 24) {
        pixel[x * 3]     = (uint8_t)(color & 0xFF);
        pixel[x * 3 + 1] = (uint8_t)((color >> 8) & 0xFF);
        pixel[x * 3 + 2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (fb_bpp == 16) {
        uint16_t r5 = (color >> 19) & 0x1F;
        uint16_t g6 = (color >> 10) & 0x3F;
        uint16_t b5 = (color >> 3)  & 0x1F;
        ((volatile uint16_t*)pixel)[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
    }
}

// Draw one 8x16 text cell (optionally inverted) into the LFB.
static void fb_draw_cell(int row, int col, uint16_t entry, bool invert) {
    uint8_t ch  = entry & 0xFF;
    uint8_t attr = (entry >> 8) & 0xFF;
    uint8_t fg  = invert ? ((attr >> 4) & 0x0F) : (attr & 0x0F);
    uint8_t bg  = invert ? (attr & 0x0F)       : ((attr >> 4) & 0x0F);
    uint32_t fg_color = vga_palette[fg];
    uint32_t bg_color = vga_palette[bg];
    const uint8_t* glyph = font8x16[ch];
    int base_x = fb_off_x + col * 8;
    int base_y = fb_off_y + row * 16;
    for (int gy = 0; gy < 16; gy++) {
        uint8_t bits = glyph[gy];
        for (int gx = 0; gx < 8; gx++) {
            uint32_t color = (bits & (0x80 >> gx)) ? fg_color : bg_color;
            fb_put_pixel(base_x + gx, base_y + gy, color);
        }
    }
}

// Force the next fb_console_render() to repaint the whole terminal.  Needed
// when the GUI exits: the LFB holds the desktop, while the VGA text snapshot
// (fb_last_cells) still matches VGA_MEMORY, so the dirty-cell logic would
// repaint nothing and leave the GUI frame on screen.
extern "C" void fb_console_force_redraw(void) {
    fb_first_render = true;   // next render: clear LFB + repaint every cell
    fb_last_cursor = -1;
}

extern "C" void fb_console_render(void) {
    // When the GUI owns the screen (gui_mode), the framebuffer console must
    // never paint: shell term.render() calls after `gui` would otherwise
    // clear the whole LFB to black and draw the text prompt over the fresh
    // desktop, leaving a black screen until the next GUI repaint.
    if (g_wm.gui_mode) return;
    if (!fb_console_active || !fb_lfb) return;

    // Terminal is 80x25 characters, each char is 8x16 pixels
    const int char_w = 8;
    const int char_h = 16;
    const int term_cols = 80;
    const int term_rows = 25;
    const int term_pixel_w = term_cols * char_w;  // 640
    const int term_pixel_h = term_rows * char_h;  // 400

    // Center the 80x25 terminal on the physical screen.
    fb_off_x = (fb_width  - term_pixel_w) / 2;
    fb_off_y = (fb_height - term_pixel_h) / 2;
    if (fb_off_x < 0) fb_off_x = 0;
    if (fb_off_y < 0) fb_off_y = 0;

    // First render: clear the whole LFB so everything outside the terminal
    // stays black; afterwards only changed cells are repainted (no flicker).
    if (fb_first_render) {
        fb_console_clear();
        fb_first_render = false;
        for (int i = 0; i < term_cols * term_rows; i++) fb_last_cells[i] = 0xFFFF;
    }

    // Dirty-cell repaint: draw only the 8x16 blocks whose VGA text entry
    // changed since the last render.
    for (int row = 0; row < term_rows; row++) {
        for (int col = 0; col < term_cols; col++) {
            int idx = row * term_cols + col;
            uint16_t entry = VGA_MEMORY[idx];
            if (entry != fb_last_cells[idx]) {
                fb_last_cells[idx] = entry;
                fb_draw_cell(row, col, entry, false);
            }
        }
    }

    // Software cursor: term.render() writes the hardware cursor position
    // registers; repaint the previous cursor cell normally and the current
    // one inverted.  The hardware cursor itself stays hidden in fbcon mode
    // so QEMU does not render a blinking text-mode block.
    uint16_t cpos = 0;
    outb(0x3D4, 0x0F); cpos = inb(0x3D5);
    outb(0x3D4, 0x0E); cpos |= (uint16_t)((uint16_t)inb(0x3D5) << 8);
    if (fb_last_cursor >= 0 && fb_last_cursor != (int)cpos &&
        fb_last_cursor < term_cols * term_rows) {
        fb_draw_cell(fb_last_cursor / term_cols, fb_last_cursor % term_cols,
                     VGA_MEMORY[fb_last_cursor], false);
    }
    if (cpos < (uint16_t)(term_cols * term_rows)) {
        fb_draw_cell(cpos / term_cols, cpos % term_cols, VGA_MEMORY[cpos], true);
        fb_last_cursor = (int)cpos;
    } else {
        fb_last_cursor = -1;
    }
}

extern "C" void fb_console_clear(void) {
    // Never wipe the framebuffer while the GUI is active (see render()).
    if (g_wm.gui_mode) return;
    if (!fb_console_active || !fb_lfb) return;
    // Clear entire framebuffer to black
    int total_bytes = fb_height * fb_pitch;
    volatile uint8_t* p = (volatile uint8_t*)fb_lfb;
    for (int i = 0; i < total_bytes; i++) p[i] = 0;
}

extern "C" int gui_bga_available(void) {
    return g_bga_available ? 1 : 0;
}

extern "C" int gui_vbe_mode_set_by_bios(void) {
    return g_vbe_mode_set_by_bios ? 1 : 0;
}

} // extern "C"
