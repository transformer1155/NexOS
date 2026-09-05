/* winpe/notepad.c -- NexOS Win32 User32/GDI stage-2 demo
 *
 * A REAL 32-bit Windows program (PE32, i386) cross-compiled with
 * i686-w64-mingw32-gcc.  It opens a window with a white "edit" area and
 * two BUTTON controls, and is fully interactive through the NexOS Win32
 * subsystem:
 *
 *   - typing keys        -> gui.cpp synthesises WM_CHAR -> WndProc appends
 *   - clicking "Echo"     (id 100) -> WM_COMMAND -> serial dump of buffer
 *   - clicking "Clear"    (id 101) -> WM_COMMAND -> empties the buffer
 *
 * It exercises genuine user32 (RegisterClass/CreateWindowEx/BeginPaint/
 * GetMessage) and gdi32 (TextOutA/FillRect/CreateSolidBrush) APIs, proving
 * the L1->L2 jump: a real 32-bit GUI program driving widgets, not a splash.
 *
 * Build:
 *   i686-w64-mingw32-gcc -O2 -nostdlib -ffreestanding \
 *       -Wl,--entry=_WinMainCRTStartup -Wl,--dynamicbase \
 *       -o notepad.exe notepad.c -lkernel32 -luser32 -lgdi32
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef void*          HWND;
typedef void*          HDC;
typedef void*          HBRUSH;
typedef void*          HINSTANCE;

#define WINAPI __attribute__((stdcall))

#define WM_CREATE   0x0001
#define WM_DESTROY  0x0002
#define WM_PAINT    0x000F
#define WM_CLOSE    0x0010
#define WM_CHAR     0x0102
#define WM_COMMAND  0x0111

#define WS_VISIBLE  0x10000000UL
#define SW_SHOW     5
#define TRANSPARENT 1

typedef int (WINAPI *WNDPROC)(HWND, u32, u32, u32);

/* Layout mirrors struct W32WndClassA in win32.cpp exactly (32-bit tokens). */
typedef struct {
    u32        style;
    WNDPROC    proc;
    int        cbCls, cbWnd;
    u32        hInst, hIcon, hCursor, hbrBackground;
    const char* menu;
    const char* name;
} WNDCLASS_;

typedef struct { long left, top, right, bottom; } RECT_;
typedef struct { HWND hwnd; u32 message, wParam, lParam, time; long px, py; } MSG_;
typedef struct {
    HDC  hdc;
    int  fErase;
    RECT_ rc;
    int  restore, incUpdate;
    u8   rgb[32];
} PAINTSTRUCT_;

/* ---- kernel32 ---- */
__declspec(dllimport) void WINAPI OutputDebugStringA(const char*);
__declspec(dllimport) void WINAPI ExitProcess(u32);
/* ---- user32 ---- */
__declspec(dllimport) u16  WINAPI RegisterClassA(const WNDCLASS_*);
__declspec(dllimport) HWND WINAPI CreateWindowExA(u32, const char*, const char*,
                                                  u32, int, int, int, int,
                                                  HWND, u32, HINSTANCE, void*);
__declspec(dllimport) int  WINAPI ShowWindow(HWND, int);
__declspec(dllimport) int  WINAPI UpdateWindow(HWND);
__declspec(dllimport) int  WINAPI GetClientRect(HWND, RECT_*);
__declspec(dllimport) int  WINAPI GetMessageA(MSG_*, HWND, u32, u32);
__declspec(dllimport) int  WINAPI TranslateMessage(const MSG_*);
__declspec(dllimport) int  WINAPI DispatchMessageA(const MSG_*);
__declspec(dllimport) int  WINAPI DefWindowProcA(HWND, u32, u32, u32);
__declspec(dllimport) HDC  WINAPI BeginPaint(HWND, PAINTSTRUCT_*);
__declspec(dllimport) int  WINAPI EndPaint(HWND, const PAINTSTRUCT_*);
__declspec(dllimport) int  WINAPI InvalidateRect(HWND, const RECT_*, int);
__declspec(dllimport) int  WINAPI FillRect(HDC, const RECT_*, HBRUSH);
/* ---- gdi32 ---- */
__declspec(dllimport) HBRUSH WINAPI CreateSolidBrush(u32);
__declspec(dllimport) int    WINAPI SetBkMode(HDC, int);
__declspec(dllimport) int    WINAPI TextOutA(HDC, int, int, const char*, int);

#define CAP 1200
static char   g_text[CAP];
static int    g_len = 0;
static HWND   g_hwnd = 0;
static HBRUSH g_white = 0;

static void dbg(const char* s){ OutputDebugStringA(s); }

/* Echo the current buffer to the serial console and leave a marker in the
   editor so the click is visibly acknowledged. */
static void do_echo(void){
    static char buf[700];
    int p = 0, k;
    const char* pre = "[app] NOTEPAD echo: ";
    while (*pre && p < 640) buf[p++] = *pre++;
    int n = g_len; if (n > 560) n = 560;
    for (k = 0; k < n; k++) buf[p++] = g_text[k];
    buf[p++] = '\n';
    buf[p] = 0;
    OutputDebugStringA(buf);

    if (g_len < CAP - 8){
        g_text[g_len++] = '\n';
        g_text[g_len++] = '[';
        g_text[g_len++] = 'e';
        g_text[g_len++] = 'c';
        g_text[g_len++] = 'h';
        g_text[g_len++] = 'o';
        g_text[g_len++] = ']';
        g_text[g_len++] = '\n';
    }
    InvalidateRect(g_hwnd, 0, 1);
}

static void paint(HWND hwnd){
    PAINTSTRUCT_ ps;
    RECT_ rc;
    GetClientRect(hwnd, &rc);
    int cw = (int)rc.right;
    int ch = (int)rc.bottom;
    HDC hdc = BeginPaint(hwnd, &ps);
    SetBkMode(hdc, TRANSPARENT);

    /* title / hint strip */
    TextOutA(hdc, 8, 4, "NexOS Notepad - type, Echo/Clear", 30);

    /* white "edit" area */
    RECT_ r;
    r.left = 6; r.top = 22; r.right = cw - 6; r.bottom = ch - 40;
    FillRect(hdc, &r, g_white);

    /* buffered text, line by line, wrapped at 46 columns */
    int y = 28, i = 0;
    while (i < g_len && y < ch - 44){
        int start = i;
        while (i < g_len && g_text[i] != '\n') i++;
        int linelen = i - start, off = 0;
        while (off < linelen && y < ch - 44){
            int chunk = linelen - off;
            if (chunk > 46) chunk = 46;
            TextOutA(hdc, 12, y, g_text + start + off, chunk);
            y += 16;
            off += chunk;
        }
        i++; /* skip newline */
    }

    EndPaint(hwnd, &ps);
}

static int WINAPI WndProc(HWND hwnd, u32 msg, u32 wp, u32 lp){
    int c;
    switch (msg){
    case WM_CREATE:
        g_hwnd = hwnd;
        g_white = CreateSolidBrush(0x00FFFFFFUL);
        dbg("[app] NOTEPAD create\n");
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_CHAR:
        c = (int)(wp & 0xFF);
        if (c == 8){                       /* backspace */
            if (g_len > 0) g_len--;
        } else if (c == 13){               /* enter */
            if (g_len < CAP - 1) g_text[g_len++] = '\n';
        } else if (c >= 32 && c < 127){    /* printable */
            if (g_len < CAP - 1) g_text[g_len++] = (char)c;
        }
        InvalidateRect(hwnd, 0, 1);
        return 0;
    case WM_COMMAND:
        if (wp == 100){                    /* Echo button */
            do_echo();
        } else if (wp == 101){             /* Clear button */
            g_len = 0;
            dbg("[app] NOTEPAD cleared\n");
            InvalidateRect(hwnd, 0, 1);
        }
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

void WinMainCRTStartup(void){
    WNDCLASS_ cls;
    int x;
    for (x = 0; x < (int)sizeof(cls); x++) ((u8*)&cls)[x] = 0;
    cls.style = 0;
    cls.proc  = WndProc;
    cls.hbrBackground = 0;
    cls.name  = "NOTEPAD";
    RegisterClassA(&cls);

    HWND w = CreateWindowExA(0, "NOTEPAD", "NexOS Notepad", WS_VISIBLE,
                             60, 60, 420, 300, 0, 0, 0, 0);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);

    /* child controls: two real BUTTONs (control id = menu param) */
    CreateWindowExA(0, "BUTTON", "Echo",  WS_VISIBLE,
                    12, 266, 90, 26, w, 100, 0, 0);
    CreateWindowExA(0, "BUTTON", "Clear", WS_VISIBLE,
                    112, 266, 90, 26, w, 101, 0, 0);

    dbg("[app] NOTEPAD launched\n");

    MSG_ m;
    while (GetMessageA(&m, 0, 0, 0)){
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    ExitProcess(0);
}
