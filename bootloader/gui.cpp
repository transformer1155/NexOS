// =====================================================================
//  gui.cpp  -  Win11-style Graphical User Interface for MiniOS
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
static bool g_vbe_mode_set_by_bios = false;  // true if mode set by BIOS/UEFI

// ---- Serial debug ----
static void serial_puts(const char* s) {
    while (*s) outb(0x3F8, (uint8_t)*s++);
}
static void serial_putc(char c) { outb(0x3F8, (uint8_t)c); }

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
    const char* (*get_os_name)(void);
    bool     (*is_64bit)(void);
    // Browser callbacks
    int      (*browser_navigate)(const char* url);  // start navigation, returns 0 on success
    int      (*browser_status)(void);               // 0=idle,1=connecting,2=loading,3=done,-1=error
    int      (*browser_get_page)(char* buf, int bufsize); // get page body
    void     (*browser_reset)(void);                // reset browser state
    // Terminal command execution callback
    void     (*exec_command)(const char* cmd, char* output, int outsize); // execute shell command
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
};

static GuiCallbacks g_cb;

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

        // ---- Shadow framebuffer fallback ----
        // QEMU's VBE LFB (0xFD000000) or GOP framebuffer (0x80000000) may sit
        // outside QEMU's emulated RAM when running with -m 64M / -m 128M.
        // The 32-bit kernel writes silently fail on those addresses, leaving
        // the framebuffer looking like random BIOS residue.  When we detect a
        // high address that won't survive a 32-bit flat memory map, we
        // allocate a low-address shadow from the kernel heap and let the
        // kernel write there.  QEMU's VBE adapter still scans the real LFB,
        // but at least the kernel sees a working framebuffer.
        if ((uintptr_t)lfb >= 0x80000000UL && info->shadow_buffer == 0) {
            uint32_t fb_bytes = (uint32_t)height * pitch;
            uint32_t* shadow = (uint32_t*)kmalloc(fb_bytes);
            if (shadow) {
                // Zero the shadow so old framebuffer residue does not leak
                for (uint32_t i = 0; i < fb_bytes / 4; i++) shadow[i] = 0;
                lfb = shadow;
                serial_puts("[GUI] Allocated shadow framebuffer at low address\n");
            }
        }

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
        // The backbuffer stores colors as 0x00RRGGBB (R in bits 16-23).
        // On x86 (little-endian), writing this as uint32_t to memory gives
        // [BB, GG, RR, 00] = BGRX, which is correct for BGRX32 framebuffers.
        // For RGBX32, we need to swap R and B.
        if (pixel_format == PXF_RGBX32) {
            // RGBX32: need to swap R and B bytes
            volatile uint8_t* dst8 = (volatile uint8_t*)lfb;
            uint8_t* src8 = (uint8_t*)backbuffer;
            int count = width * height;
            for (int i = 0; i < count; i++) {
                dst8[i*4]   = src8[i*4+2];  // R <- B (swap)
                dst8[i*4+1] = src8[i*4+1];  // G <- G
                dst8[i*4+2] = src8[i*4];    // B <- R (swap)
                dst8[i*4+3] = 0;            // X
            }
        } else if (pixel_format == PXF_RGB24 || bpp == 24) {
            // 24-bit packed RGB (BGR in memory on x86)
            volatile uint8_t* dst = (volatile uint8_t*)lfb;
            uint8_t* src = (uint8_t*)backbuffer;
            int count = width * height;
            for (int i = 0; i < count; i++) {
                dst[i*3]   = src[i*4];      // B
                dst[i*3+1] = src[i*4+1];    // G
                dst[i*3+2] = src[i*4+2];    // R
            }
        } else if (pixel_format == PXF_RGB565 || bpp == 16) {
            // 16-bit RGB565
            volatile uint16_t* dst = (volatile uint16_t*)lfb;
            uint32_t* src = backbuffer;
            int count = width * height;
            for (int i = 0; i < count; i++) {
                uint32_t c = src[i];  // 0x00RRGGBB
                uint16_t r = (c >> 19) & 0x1F;
                uint16_t g = (c >> 10) & 0x3F;
                uint16_t b = (c >> 3) & 0x1F;
                dst[i] = (r << 11) | (g << 5) | b;
            }
        } else {
            // BGRX32 (most common UEFI): direct copy is correct on x86 LE
            // Also used as fallback for unknown formats
            volatile uint32_t* dst = lfb;
            uint32_t* src = backbuffer;
            int count = width * height;
            for (int i = 0; i < count; i++) dst[i] = src[i];
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

    void draw_rect(int x, int y, int w, int h, Color c) {
        for (int i = x; i < x + w; i++) { put_pixel(i, y, c); put_pixel(i, y + h - 1, c); }
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

    // Control Panel category view state
    int cp_category;        // -1 = category list, 0-7 = selected category
    // Task Manager search state
    char tm_search[64];     // search text
    int tm_search_len;      // search text length
    bool tm_search_focused; // search bar focused
    int tm_selected_proc;   // selected process index (-1 = none)
    // Portal desktop state
    int portal_tab;         // selected navigation tab (0=home,1=apps,2=system,3=tools)

    bool contains(int px, int py) {
        return px >= x && px < x + w && py >= y && py < y + h;
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
            {"About MiniOS",  APP_ABOUT,         0x0099BC, 'i'},
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
        windows[id].visible = false;
        if (active_window == id) {
            active_window = -1;
            for (int i = window_count - 1; i >= 0; i--) {
                if (windows[i].visible) { windows[i].active = true; active_window = i; break; }
            }
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
                                  "Search MiniOS...", C_WIN_TEXT_SEC);

        // MiniOS logo text above search (with Chinese welcome)
        const char* logo = "MiniOS";
        int logo_w = strlen_(logo) * FONT_W * 2; // larger spacing
        gfx.draw_text_transparent(dx + (dw - logo_w)/2, sy - 28, logo, C_ACCENT);
        gfx.draw_text_utf8_transparent(dx + (dw - 144)/2, sy - 12, "欢迎使用迷你操作系统", C_WIN_TEXT_SEC);

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
            gfx.draw_text_centered(cx, card_y + 60, card_w, "MiniOS Desktop", C_WIN_TEXT_SEC);
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

        // Right side: system tray + clock
        int rx = gfx.width - 8;
        // Chinese welcome label (CJK test)
        gfx.draw_text_utf8(rx - 160, 8, "欢迎", C_TOPBAR_TEXT, C_TOPBAR_BG);
        // Clock (HH:MM)
        char clock_str[8];
        clock_str[0] = '0' + (clock_h / 10);
        clock_str[1] = '0' + (clock_h % 10);
        clock_str[2] = ':';
        clock_str[3] = '0' + (clock_m / 10);
        clock_str[4] = '0' + (clock_m % 10);
        clock_str[5] = 0;
        int cw = 5 * FONT_W + 8;
        gfx.draw_text(rx - cw, 8, clock_str, C_TOPBAR_TEXT, C_TOPBAR_BG);
        rx -= cw + 8;

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
        int mw = 280;
        int mh = start_item_count * 44 + 40;
        int mx = 8;
        int my = TOPBAR_H + 2;

        // Shadow
        gfx.fill_rect(mx + 3, my + 3, mw, mh, 0x40000000);
        // Background
        gfx.fill_rounded_rect(mx, my, mw, mh, 8, C_STARTMENU_BG);
        gfx.draw_rounded_rect(mx, my, mw, mh, 8, C_STARTMENU_BORDER);

        // Header
        gfx.draw_text(mx + 12, my + 8, "MiniOS Start", C_TOPBAR_TEXT, C_STARTMENU_BG);

        // Items
        for (int i = 0; i < start_item_count; i++) {
            int iy = my + 28 + i * 44;
            Color ibg = C_STARTMENU_BG;
            if (mouse_x >= mx + 4 && mouse_x < mx + mw - 4 &&
                mouse_y >= iy && mouse_y < iy + 40) {
                ibg = C_STARTMENU_HOVER;
            }
            gfx.fill_rounded_rect(mx + 4, iy, mw - 8, 40, 6, ibg);
            // Icon
            gfx.draw_icon(mx + 12, iy + 4, 32, start_items[i].color,
                          start_items[i].letter, COLOR_WHITE);
            // Label
            gfx.draw_text(mx + 52, iy + 10, start_items[i].label, C_TOPBAR_TEXT, ibg);
        }
    }

    void draw_window(int id) {
        Win11Window& win = windows[id];
        if (!win.visible) return;

        // Window shadow
        for (int i = 0; i < 4; i++) {
            gfx.fill_rect(win.x + 3 + i, win.y + 3 + i, win.w, win.h, 0x20000000);
        }

        // Window background
        gfx.fill_rounded_rect(win.x, win.y, win.w, win.h, 6, C_WIN_BG);

        // Title bar
        Color tb_color = win.active ? C_WIN_TITLEBAR_ACT : C_WIN_TITLEBAR;
        gfx.fill_rounded_rect(win.x, win.y, win.w, TITLE_BAR_H, 6, tb_color);
        // Fill the bottom part of title bar (square corners at bottom)
        gfx.fill_rect(win.x, win.y + TITLE_BAR_H - 6, win.w, 6, tb_color);

        // Title text (centered)
        int tw = strlen_(win.title) * FONT_W;
        gfx.draw_text(win.x + (win.w - tw) / 2, win.y + 8, win.title,
                      C_WIN_TEXT, tb_color);

        // Close button (top right)
        int cbx = win.x + win.w - 28;
        int cby = win.y + 4;
        bool close_hover = (mouse_x >= cbx && mouse_x < cbx + 24 &&
                           mouse_y >= cby && mouse_y < cby + 24);
        Color cbg = close_hover ? C_CLOSE_HOVER : tb_color;
        gfx.fill_rounded_rect(cbx, cby, 24, 24, 4, cbg);
        // X symbol
        gfx.draw_text(cbx + 8, cby + 4, "X", close_hover ? COLOR_WHITE : C_CLOSE_TEXT, cbg);

        // Border
        Color border = win.active ? C_WIN_BORDER_ACT : C_WIN_BORDER;
        gfx.draw_rounded_rect(win.x, win.y, win.w, win.h, 6, border);

        // Separator line below title bar
        gfx.draw_line(win.x + 1, win.y + TITLE_BAR_H, win.x + win.w - 2, win.y + TITLE_BAR_H, C_WIN_BORDER);

        // Draw app content
        draw_app_content(id);
    }

    // ---- App content rendering ----
    void draw_app_content(int id) {
        Win11Window& win = windows[id];
        int cx = win.x + 8;
        int cy = win.content_y() + 8;
        int cw = win.w - 16;
        int ch = win.content_h() - 16;

        switch (win.app) {
            case APP_CONTROL_PANEL:   draw_control_panel(id, cx, cy, cw, ch); break;
            case APP_FILE_EXPLORER:   draw_file_explorer(id, cx, cy, cw, ch); break;
            case APP_TASK_MANAGER:    draw_task_manager(id, cx, cy, cw, ch); break;
            case APP_MEM_OPTIMIZER:   draw_mem_optimizer(id, cx, cy, cw, ch); break;
            case APP_CALCULATOR:      draw_calculator(id, cx, cy, cw, ch); break;
            case APP_TERMINAL:        draw_terminal(id, cx, cy, cw, ch); break;
            case APP_ABOUT:           draw_about(id, cx, cy, cw, ch); break;
            case APP_BROWSER:         draw_browser(id, cx, cy, cw, ch); break;
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
                    strcpy_(buf, "OS: MiniOS");
                }
                gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;

                if (g_cb.is_64bit) {
                    strcpy_(buf, "Arch: ");
                    strcat_safe(buf, g_cb.is_64bit() ? "x86-64 (Long Mode)" : "x86 (Protected Mode)", sizeof(buf));
                    gfx.draw_text_transparent(x, y, buf, C_WIN_TEXT); y += 18;
                    if (!g_cb.is_64bit() && g_cb.get_cpu_64bit_capable && g_cb.get_cpu_64bit_capable()) {
                        gfx.draw_text_transparent(x, y, "  64-bit ready (type 'switch')", C_MEM_GOOD); y += 18;
                    }
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
                    strcpy_(buf, "  MiniOS Shell (PID 1)");
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
            procs[nproc].name = "MiniOS Shell"; procs[nproc].status = "Running";
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

        // Terminal output
        char* p = win.term_buf;
        int ty = y;
        int max_lines = h / 16;
        int line = 0;
        while (*p && line < max_lines) {
            char t[81];
            int ti = 0;
            while (*p && *p != '\n' && ti < 80) t[ti++] = *p++;
            t[ti] = 0;
            if (*p == '\n') p++;
            gfx.draw_text_transparent(x, ty, t, 0xCCCCCC);
            ty += 16;
            line++;
        }

        // Input line
        gfx.draw_text_transparent(x, ty, "> ", 0x00FF66);
        gfx.draw_text_transparent(x + 16, ty, win.term_input, 0xCCCCCC);
        // Cursor
        int cx = x + 16 + win.term_input_len * FONT_W;
        gfx.fill_rect(cx, ty, 8, 16, 0xCCCCCC);
    }

    // ---- About ----
    void draw_about(int id, int x, int y, int w, int h) {
        gfx.draw_text_transparent(x, y, "About MiniOS", C_WIN_TEXT);
        y += 28;

        gfx.draw_text_transparent(x, y, "MiniOS v2.0", C_ACCENT); y += 20;
        gfx.draw_text_transparent(x, y, "Win11-style GUI Desktop", C_WIN_TEXT_SEC); y += 20;

        gfx.draw_text_transparent(x, y, "Features:", C_WIN_TEXT); y += 20;
        gfx.draw_text_transparent(x, y, "  - 32/64-bit kernel", C_WIN_TEXT_SEC); y += 16;
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
            // Show switch hint if in 32-bit mode but CPU supports 64-bit
            if (!g_cb.is_64bit() && g_cb.get_cpu_64bit_capable && g_cb.get_cpu_64bit_capable()) {
                gfx.draw_text_transparent(x, y, "Type 'switch' in terminal", C_WIN_TEXT_SEC);
            }
        }
    }

    // ---- Web Browser ----
    void draw_browser(int id, int x, int y, int w, int h) {
        Win11Window& win = windows[id];

        // Check for browser status updates (async)
        if (win.browser_status == 1 || win.browser_status == 2) {
            if (g_cb.browser_status) {
                int s = g_cb.browser_status();
                if (s != win.browser_status) {
                    win.browser_status = s;
                    if (s == 3) {
                        // Page complete - fetch it
                        if (g_cb.browser_get_page) {
                            win.browser_page_len = g_cb.browser_get_page(
                                win.browser_page, sizeof(win.browser_page) - 1);
                            if (win.browser_page_len < 0) win.browser_page_len = 0;
                            win.browser_page[win.browser_page_len] = 0;
                            win.browser_scroll = 0;
                        }
                    } else if (s == -1) {
                        // Error
                        const char* err = "Error: Could not load page.\nCheck URL or network connection.";
                        int el = strlen_(err);
                        memcpy_(win.browser_page, err, el);
                        win.browser_page[el] = 0;
                        win.browser_page_len = el;
                    }
                }
            }
        }

        // ---- URL bar ----
        int url_bar_h = 28;
        int btn_w = 60;

        // Back/Go button
        bool go_hover = (mouse_x >= x + w - btn_w && mouse_x < x + w &&
                        mouse_y >= y && mouse_y < y + url_bar_h);
        Color go_bg = go_hover ? C_BTN_HOVER : C_ACCENT;
        gfx.fill_rounded_rect(x + w - btn_w, y, btn_w, url_bar_h, 4, go_bg);
        gfx.draw_text(x + w - btn_w + 18, y + 6, "Go", COLOR_WHITE, go_bg);

        // URL input field
        int url_field_w = w - btn_w - 8;
        gfx.fill_rounded_rect(x, y, url_field_w, url_bar_h, 4, COLOR_WHITE);
        gfx.draw_rounded_rect(x, y, url_field_w, url_bar_h, 4, C_BTN_BORDER);

        // URL text
        if (win.browser_url_len > 0) {
            gfx.draw_text(x + 8, y + 6, win.browser_url, 0x1A1A1A, COLOR_WHITE);
        } else if (win.browser_url_focused) {
            // Show cursor
            gfx.fill_rect(x + 8, y + 8, 2, 16, 0x1A1A1A);
        } else {
            gfx.draw_text(x + 8, y + 6, "Enter URL (e.g. example.com)...", 0x999999, COLOR_WHITE);
        }

        // Cursor if focused
        if (win.browser_url_focused && win.browser_url_len > 0) {
            int cx = x + 8 + win.browser_url_len * FONT_W;
            if (cx < x + url_field_w - 4)
                gfx.fill_rect(cx, y + 8, 2, 16, 0x1A1A1A);
        }

        y += url_bar_h + 4;
        h -= url_bar_h + 4;

        // ---- Status bar ----
        const char* status_text;
        Color status_color;
        switch (win.browser_status) {
            case 0:  status_text = "Ready"; status_color = C_WIN_TEXT_SEC; break;
            case 1:  status_text = "Connecting..."; status_color = C_ACCENT; break;
            case 2:  status_text = "Loading..."; status_color = C_ACCENT; break;
            case 3:  status_text = "Done"; status_color = C_MEM_GOOD; break;
            case -1: status_text = "Error"; status_color = C_MEM_BAD; break;
            default: status_text = ""; status_color = C_WIN_TEXT_SEC; break;
        }

        // Content area background (white)
        int content_h = h - 20;
        gfx.fill_rect(x, y, w, content_h, COLOR_WHITE);

        // Draw content border
        gfx.draw_rect(x, y, w, content_h, C_BTN_BORDER);

        // ---- Render page content ----
        if (win.browser_page_len > 0 && win.browser_status == 3) {
            // Simple HTML text rendering
            char* p = win.browser_page;
            int py = y + 4 - win.browser_scroll;
            int px = x + 8;
            int max_x = x + w - 12;
            int max_lines = content_h / 16 + 2;
            int line = 0;

            while (*p && line < max_lines) {
                // Skip HTML tags
                if (*p == '<') {
                    // Check for <br>, <p>, </p>, <h1>-<h6>, <li>, etc.
                    if ((p[1] == 'b' && (p[2] == 'r' || p[2] == 'R')) ||
                        (p[1] == 'B' && (p[2] == 'r' || p[2] == 'R'))) {
                        py += 16;
                        px = x + 8;
                        line++;
                    } else if (p[1] == 'p' || p[1] == 'P' || p[1] == '/') {
                        py += 16;
                        px = x + 8;
                        line++;
                    } else if ((p[1] == 'h' || p[1] == 'H') && p[2] >= '1' && p[2] <= '6') {
                        py += 20;
                        px = x + 8;
                        line++;
                    } else if (p[1] == 'l' || p[1] == 'L') {
                        py += 4;
                        px = x + 16;
                    }
                    // Skip to end of tag
                    while (*p && *p != '>') p++;
                    if (*p == '>') p++;
                    continue;
                }

                // Handle special HTML entities
                if (*p == '&') {
                    if (p[1] == 'l' && p[2] == 't' && p[3] == ';') {
                        // &lt; = <
                        if (px + FONT_W < max_x && py >= y && py < y + content_h)
                            gfx.draw_char(px, py, '<', 0x1A1A1A, COLOR_WHITE);
                        px += FONT_W;
                        p += 4;
                        continue;
                    } else if (p[1] == 'g' && p[2] == 't' && p[3] == ';') {
                        if (px + FONT_W < max_x && py >= y && py < y + content_h)
                            gfx.draw_char(px, py, '>', 0x1A1A1A, COLOR_WHITE);
                        px += FONT_W;
                        p += 4;
                        continue;
                    } else if (p[1] == 'a' && p[2] == 'm' && p[3] == 'p' && p[4] == ';') {
                        if (px + FONT_W < max_x && py >= y && py < y + content_h)
                            gfx.draw_char(px, py, '&', 0x1A1A1A, COLOR_WHITE);
                        px += FONT_W;
                        p += 5;
                        continue;
                    } else if (p[1] == 'n' && p[2] == 'b' && p[3] == 's' && p[4] == 'p' && p[5] == ';') {
                        px += FONT_W * 3;
                        p += 6;
                        continue;
                    } else if (p[1] == 'q' && p[2] == 'u' && p[3] == 'o' && p[4] == 't' && p[5] == ';') {
                        if (px + FONT_W < max_x && py >= y && py < y + content_h)
                            gfx.draw_char(px, py, '"', 0x1A1A1A, COLOR_WHITE);
                        px += FONT_W;
                        p += 6;
                        continue;
                    }
                }

                // Handle newlines
                if (*p == '\n') {
                    py += 16;
                    px = x + 8;
                    line++;
                    p++;
                    continue;
                }

                // Handle tab
                if (*p == '\t') {
                    px += FONT_W * 4;
                    p++;
                    continue;
                }

                // Handle carriage return
                if (*p == '\r') {
                    p++;
                    continue;
                }

                // Regular character
                if (*p >= 32 && *p < 127) {
                    if (px + FONT_W < max_x) {
                        if (py >= y && py < y + content_h)
                            gfx.draw_char(px, py, *p, 0x1A1A1A, COLOR_WHITE);
                        px += FONT_W;
                    } else {
                        // Line wrap
                        py += 16;
                        px = x + 8;
                        line++;
                    }
                }
                p++;
            }

            // Scroll indicator
            int total_lines = py + win.browser_scroll - y;
            if (total_lines > content_h) {
                int scrollbar_h = content_h * content_h / total_lines;
                if (scrollbar_h < 10) scrollbar_h = 10;
                int scrollbar_y = y + (content_h - scrollbar_h) * win.browser_scroll / (total_lines - content_h + 1);
                gfx.fill_rect(x + w - 4, scrollbar_y, 4, scrollbar_h, 0xCCCCCC);
            }
        } else if (win.browser_status == 1 || win.browser_status == 2) {
            // Loading indicator
            const char* loading = "Loading...";
            gfx.draw_text_transparent(x + w/2 - 40, y + content_h/2, loading, C_ACCENT);
        } else if (win.browser_status == -1) {
            // Error display
            char* p = win.browser_page;
            int py = y + 8;
            while (*p && py < y + content_h) {
                char t[81]; int ti = 0;
                while (*p && *p != '\n' && ti < 80) t[ti++] = *p++;
                t[ti] = 0;
                if (*p == '\n') p++;
                gfx.draw_text_transparent(x + 8, py, t, C_MEM_BAD);
                py += 16;
            }
        } else {
            // Welcome page
            gfx.draw_text_transparent(x + w/2 - 60, y + 40, "MiniOS Web Browser", C_ACCENT);
            gfx.draw_text_transparent(x + 20, y + 80, "Enter a URL above and press Go", C_WIN_TEXT_SEC);
            gfx.draw_text_transparent(x + 20, y + 100, "or press Enter to navigate.", C_WIN_TEXT_SEC);
            gfx.draw_text_transparent(x + 20, y + 130, "Examples:", C_WIN_TEXT);
            gfx.draw_text_transparent(x + 30, y + 150, "example.com", C_ACCENT);
            gfx.draw_text_transparent(x + 30, y + 168, "10.0.2.2:8080", C_ACCENT);
            gfx.draw_text_transparent(x + 30, y + 186, "http://example.com/path", C_ACCENT);
        }

        // ---- Status bar at bottom ----
        int status_y = y + content_h + 2;
        gfx.fill_rect(x, status_y, w, 18, 0xF0F0F0);
        gfx.draw_text_transparent(x + 4, status_y + 1, status_text, status_color);

        // Show page size if loaded
        if (win.browser_status == 3 && win.browser_page_len > 0) {
            char info[32];
            strcpy_(info, "  |  ");
            uint_to_str(win.browser_page_len, info + strlen_(info));
            strcat_safe(info, " bytes", sizeof(info));
            gfx.draw_text_transparent(x + 100, status_y + 1, info, C_WIN_TEXT_SEC);
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

    // ---- Rendering ----
    void render_all() {
        if (!gfx.initialized) return;

        draw_wallpaper();
        draw_topbar();
        draw_portal_desktop();

        // Draw windows (inactive first, then active on top)
        for (int i = 0; i < window_count; i++) {
            if (windows[i].visible && !windows[i].active) draw_window(i);
        }
        for (int i = 0; i < window_count; i++) {
            if (windows[i].visible && windows[i].active) draw_window(i);
        }

        if (start_menu_open) draw_start_menu();

        // Redraw cursor on top without hide/show flicker
        // Invalidate saved background so it's re-captured from the freshly-drawn screen
        if (cursor.visible) {
            cursor.saved_valid = false;
            cursor.save_bg(gfx);
            cursor.draw(gfx);
        }

        // Flip backbuffer to screen atomically
        gfx.present();
    }

    void enter_gui() {
        if (!gfx.initialized) { serial_puts("[GUI] No VBE\n"); return; }
        gfx.enable_vbe_mode();  // Sets BGA mode (emulator) or uses BIOS-set mode (real HW)
        gfx.force_clear_lfb();  // wipe bootuefi/OVMF residue
        gui_mode = true;
        cursor.visible = true;       // Make cursor visible on entry
        cursor.saved_valid = false;  // Force background re-capture
        render_all();   // draws everything + cursor + presents to screen
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
            // Just move the cursor, no topbar redraw
            gfx.present();
        }
    }

    void handle_mouse_down() {
        if (!gui_mode) return;
        mouse_left = true;
        start_menu_open = false; // close by default, reopen if clicked

        // Check top bar (Start button)
        if (mouse_y < TOPBAR_H) {
            if (mouse_x >= 8 && mouse_x < 52) {
                start_menu_open = true;
                render_all();
                return;
            }
            // Check running app buttons in top bar
            int ax = 52 + 16;
            for (int i = 0; i < window_count; i++) {
                if (!windows[i].visible) continue;
                int aw = strlen_(windows[i].title) * FONT_W + 24;
                if (mouse_x >= ax && mouse_x < ax + aw) {
                    for (int j = 0; j < window_count; j++) windows[j].active = false;
                    windows[i].active = true;
                    active_window = i;
                    render_all();
                    return;
                }
                ax += aw + 4;
            }
            render_all();
            return;
        }

        // Check start menu items
        if (start_menu_open) {
            // Re-open since we closed it above... actually let's handle differently
            start_menu_open = true;
            int mw = 280;
            int mx = 8;
            int my = TOPBAR_H + 2;
            for (int i = 0; i < start_item_count; i++) {
                int iy = my + 28 + i * 44;
                if (mouse_x >= mx + 4 && mouse_x < mx + mw - 4 &&
                    mouse_y >= iy && mouse_y < iy + 40) {
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

        // Check windows (top to bottom)
        for (int i = window_count - 1; i >= 0; i--) {
            if (!windows[i].visible) continue;
            // Title bar: drag or close
            if (windows[i].title_contains(mouse_x, mouse_y)) {
                // Close button?
                int cbx = windows[i].x + windows[i].w - 28;
                int cby = windows[i].y + 4;
                if (mouse_x >= cbx && mouse_x < cbx + 24 &&
                    mouse_y >= cby && mouse_y < cby + 24) {
                    close_window(i);
                    render_all();
                    return;
                }
                // Drag
                drag_window = i;
                drag_off_x = mouse_x - windows[i].x;
                drag_off_y = mouse_y - windows[i].y;
                for (int j = 0; j < window_count; j++) windows[j].active = false;
                windows[i].active = true;
                active_window = i;
                render_all();
                return;
            }
            // Content click
            if (windows[i].contains(mouse_x, mouse_y)) {
                for (int j = 0; j < window_count; j++) windows[j].active = false;
                windows[i].active = true;
                active_window = i;
                handle_app_click(i);
                render_all();
                return;
            }
        }
    }

    void handle_mouse_up() {
        mouse_left = false;
        drag_window = -1;
    }

    void handle_app_click(int win_id) {
        Win11Window& win = windows[win_id];
        int x = win.x + 8;
        int y = win.content_y() + 8;
        int w = win.w - 16;
        int h = win.content_h() - 16;

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
            int url_bar_h = 28;
            int btn_w = 60;

            // Check Go button click
            if (mouse_x >= x + w - btn_w && mouse_x < x + w &&
                mouse_y >= y && mouse_y < y + url_bar_h) {
                // Navigate
                if (win.browser_url_len > 0 && g_cb.browser_navigate) {
                    g_cb.browser_reset();
                    win.browser_status = 1;
                    win.browser_page_len = 0;
                    win.browser_page[0] = 0;
                    g_cb.browser_navigate(win.browser_url);
                }
                return;
            }

            // Check URL bar click (focus it)
            int url_field_w = w - btn_w - 8;
            if (mouse_x >= x && mouse_x < x + url_field_w &&
                mouse_y >= y && mouse_y < y + url_bar_h) {
                win.browser_url_focused = true;
                return;
            }

            // Click in content area - unfocus URL bar and check for scroll
            win.browser_url_focused = false;
            int content_y = y + url_bar_h + 4;
            int content_h = h - url_bar_h - 4 - 20;
            if (mouse_x >= x && mouse_x < x + w &&
                mouse_y >= content_y && mouse_y < content_y + content_h) {
                // Click on scrollbar area
                if (mouse_x >= x + w - 8) {
                    // Scroll down
                    if (mouse_y > content_y + content_h / 2) {
                        win.browser_scroll += 32;
                    } else {
                        win.browser_scroll -= 32;
                        if (win.browser_scroll < 0) win.browser_scroll = 0;
                    }
                }
            }
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

    void launch_app(AppType app) {
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
                ww = 360; wh = 360; title = "About MiniOS"; break;
            case APP_BROWSER:
                ww = 600; wh = 440; title = "Web Browser"; break;
            default: return;
        }

        wx = (gw - ww) / 2;
        wy = (gh - wh) / 2;
        // Offset for cascading
        wy += TOPBAR_H + 10;
        if (wy + wh > gh - 10) wy = gh - wh - 10;

        int id = create_window(wx, wy, ww, wh, title, app);
        if (id >= 0 && app == APP_TERMINAL) {
            // Initialize terminal
            Win11Window& win = windows[id];
            strcpy_(win.term_buf, "MiniOS Terminal v2.0\nType 'help' for commands.\n\n");
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
        const char* hdr = "=== MiniOS Windows App Launcher ===\n\nFile: ";
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

    bool handle_key(char ch) {
        if (!gui_mode) return false;
        if (ch == 27) { // ESC
            gui_mode = false;
            cursor.hide(gfx);
            serial_puts("[GUI] Exited GUI mode\n");
            return true;
        }
        // Handle terminal input
        if (active_window >= 0 && windows[active_window].app == APP_TERMINAL) {
            Win11Window& win = windows[active_window];
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
                    win.term_input_len--;
                    win.term_input[win.term_input_len] = 0;
                    render_all();
                }
                return true;
            } else if (ch >= 32 && ch < 127 && win.term_input_len < 126) {
                win.term_input[win.term_input_len++] = ch;
                win.term_input[win.term_input_len] = 0;
                render_all();
                return true;
            }
        }
        // Handle browser URL input
        else if (active_window >= 0 && windows[active_window].app == APP_BROWSER) {
            Win11Window& win = windows[active_window];
            if (win.browser_url_focused) {
                if (ch == '\n' || ch == 0x0D) { // Enter = navigate
                    if (win.browser_url_len > 0 && g_cb.browser_navigate) {
                        g_cb.browser_reset();
                        win.browser_status = 1;
                        win.browser_page_len = 0;
                        win.browser_page[0] = 0;
                        g_cb.browser_navigate(win.browser_url);
                    }
                    render_all();
                    return true;
                } else if (ch == 0x08) { // Backspace
                    if (win.browser_url_len > 0) {
                        win.browser_url_len--;
                        win.browser_url[win.browser_url_len] = 0;
                        render_all();
                    }
                    return true;
                } else if (ch >= 32 && ch < 127 && win.browser_url_len < 254) {
                    win.browser_url[win.browser_url_len++] = ch;
                    win.browser_url[win.browser_url_len] = 0;
                    render_all();
                    return true;
                }
            }
        }
        return false;
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
void fb_console_clear(void);

void gui_set_callbacks(const GuiCallbacks* cb) {
    if (cb) g_cb = *cb;
}

int gui_init(void) {
    g_wm.init();
    return g_wm.gfx.initialized ? 0 : -1;
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

void gui_enter(void) {
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

int gui_handle_key(char ch) {
    return g_wm.handle_key(ch) ? 1 : 0;
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

// ---- Update clock (only redraw clock area, not entire top bar) ----
void gui_tick(void) {
    if (!g_wm.gui_mode) return;
    g_wm.update_clock();

    // Only redraw the clock area on the right side of the top bar
    int rx = g_wm.gfx.width - 8;
    int cw = 5 * FONT_W + 8;        // "HH:MM" = 5 chars
    int cx = rx - cw;                // clock text x
    int cy = 8;                      // clock text y

    // Save cursor background if cursor overlaps clock area
    bool cursor_over = g_wm.cursor.visible &&
        g_wm.cursor.x + 16 > cx && g_wm.cursor.x < cx + cw + 8 &&
        g_wm.cursor.y + 16 > 0 && g_wm.cursor.y < TOPBAR_H;
    if (cursor_over) g_wm.cursor.hide(g_wm.gfx);

    // Clear clock background
    g_wm.gfx.fill_rect(cx - 4, 0, cw + 12, TOPBAR_H, C_TOPBAR_BG);

    // Draw clock text
    char clock_str[8];
    clock_str[0] = '0' + (g_wm.clock_h / 10);
    clock_str[1] = '0' + (g_wm.clock_h % 10);
    clock_str[2] = ':';
    clock_str[3] = '0' + (g_wm.clock_m / 10);
    clock_str[4] = '0' + (g_wm.clock_m % 10);
    clock_str[5] = 0;
    g_wm.gfx.draw_text(cx, cy, clock_str, C_TOPBAR_TEXT, C_TOPBAR_BG);

    if (cursor_over) g_wm.cursor.show(g_wm.gfx);

    // Flip updated clock area to screen
    g_wm.gfx.present();
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

// ---- Exit GUI mode ----
void gui_exit(void) {
    if (!g_wm.gui_mode) return;
    g_wm.cursor.hide(g_wm.gfx);
    g_wm.gui_mode = false;
    g_wm.drag_window = -1;
    g_wm.start_menu_open = false;
    // Keep windows but they're not visible in text mode

    if (g_bga_available) {
        // Emulator path: disable BGA and restore VGA text mode
        g_wm.gfx.disable_vbe_mode();
        vga_set_text_mode();
        serial_puts("[GUI] Exited GUI mode, VBE disabled, text mode restored\n");
    } else {
        // Real hardware path: can't switch back to text mode (no INT 10h in PM)
        // VBE mode stays active; framebuffer console will re-render terminal
        // Just clear the screen to black
        fb_console_clear();
        serial_puts("[GUI] Exited GUI mode, VBE stays active (framebuffer console)\n");
    }
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

// VGA text buffer (80x25, each entry: byte char + byte color)
static volatile uint16_t* const FB_VGA_TEXT = (volatile uint16_t*)0xB8000;

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

extern "C" void fb_console_render(void) {
    if (!fb_console_active || !fb_lfb) return;

    // Terminal is 80x25 characters, each char is 8x16 pixels
    const int char_w = 8;
    const int char_h = 16;
    const int term_cols = 80;
    const int term_rows = 25;
    const int term_pixel_w = term_cols * char_w;  // 640
    const int term_pixel_h = term_rows * char_h;  // 400

    // Clear the screen to black first (only the terminal area)
    for (int y = 0; y < fb_height && y < term_pixel_h; y++) {
        volatile uint8_t* row = (volatile uint8_t*)fb_lfb + y * fb_pitch;
        for (int x = 0; x < fb_width && x < term_pixel_w; x++) {
            if (fb_bpp == 32) {
                ((volatile uint32_t*)row)[x] = 0x000000;
            } else if (fb_bpp == 24) {
                row[x*3] = 0; row[x*3+1] = 0; row[x*3+2] = 0;
            } else if (fb_bpp == 16) {
                ((volatile uint16_t*)row)[x] = 0;
            }
        }
    }

    // Render each character from VGA text buffer
    for (int row = 0; row < term_rows; row++) {
        for (int col = 0; col < term_cols; col++) {
            uint16_t entry = FB_VGA_TEXT[row * term_cols + col];
            uint8_t ch = entry & 0xFF;
            uint8_t attr = (entry >> 8) & 0xFF;
            uint8_t fg = attr & 0x0F;
            uint8_t bg = (attr >> 4) & 0x0F;
            uint32_t fg_color = vga_palette[fg];
            uint32_t bg_color = vga_palette[bg];

            const uint8_t* glyph = font8x16[ch];
            int base_x = col * char_w;
            int base_y = row * char_h;

            for (int gy = 0; gy < char_h; gy++) {
                uint8_t bits = glyph[gy];
                for (int gx = 0; gx < char_w; gx++) {
                    int px = base_x + gx;
                    int py = base_y + gy;
                    if (px >= fb_width || py >= fb_height) continue;

                    uint32_t color = (bits & (0x80 >> gx)) ? fg_color : bg_color;
                    volatile uint8_t* pixel = (volatile uint8_t*)fb_lfb + py * fb_pitch;

                    if (fb_bpp == 32) {
                        ((volatile uint32_t*)pixel)[px] = color;
                    } else if (fb_bpp == 24) {
                        pixel[px*3]   = color & 0xFF;         // B
                        pixel[px*3+1] = (color >> 8) & 0xFF;  // G
                        pixel[px*3+2] = (color >> 16) & 0xFF; // R
                    } else if (fb_bpp == 16) {
                        // RGB565: RRRRRGGGGGGBBBBB
                        uint16_t c16 = ((color >> 19) << 11) | ((color >> 10) << 5) | (color >> 3);
                        ((volatile uint16_t*)pixel)[px] = c16;
                    }
                }
            }
        }
    }
}

extern "C" void fb_console_clear(void) {
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
