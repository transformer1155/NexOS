/* ---------------------------------------------------------------------
 *  chrome.c  -  NexOS browser, built as a genuine Win64 PE32+ binary
 *
 *  This replaces the kernel-native browser (gui.cpp::namespace browser)
 *  with a real Windows executable: the file on disk is a PE32+ image
 *  with an MZ/PE header, an import table and an ms_abi entry point, and
 *  it reaches the screen only through the Win32 API that win32.cpp
 *  exports.  The kernel does not special-case it in any way -- it goes
 *  through exactly the same win64_run path as any other PE.
 *
 *  DRAWING BUDGET
 *  --------------
 *  win32.h caps a window at W32_MAX_CMDS = 72 display-list entries and
 *  cmd_push() silently drops anything past that, so an over-budget
 *  layout loses its *last* elements rather than failing loudly.  The
 *  layout below is 39 commands; the count is asserted at the end of
 *  paint() through the debug string so a regression is visible on the
 *  serial log instead of only on screen.
 *
 *  Every colour is the real Chrome light-theme value (Google Material):
 *  #DEE1E6 tab strip, #F1F3F4 omnibox, #DADCE0 hairline, #202124 text,
 *  #5F6368 secondary text, and the four brand colours.
 * ------------------------------------------------------------------ */
#include "minipe.h"

#define CW 880          /* client width  */
#define CH 540          /* client height */

/* Chrome light theme */
#define C_TABBAR   RGB_(0xDE, 0xE1, 0xE6)
#define C_SURFACE  RGB_(0xFF, 0xFF, 0xFF)
#define C_OMNIBOX  RGB_(0xF1, 0xF3, 0xF4)
#define C_HAIRLINE RGB_(0xDA, 0xDC, 0xE0)
#define C_TEXT     RGB_(0x20, 0x21, 0x24)
#define C_TEXT2    RGB_(0x5F, 0x63, 0x68)
#define C_RED      RGB_(0xEA, 0x43, 0x35)
#define C_YELLOW   RGB_(0xFB, 0xBC, 0x05)
#define C_GREEN    RGB_(0x34, 0xA8, 0x53)
#define C_BLUE     RGB_(0x42, 0x85, 0xF4)

static int g_cmds;      /* display-list entries we asked for */

/* ---- tiny drawing helpers ---------------------------------------- */

static void fill(HDC dc, int x, int y, int w, int h, u32 c)
{
    HBRUSH b = CreateSolidBrush(c);
    RECT_ r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b);
    DeleteObject(b);
    g_cmds++;
}

/* Ellipse() takes its colour from the brush currently selected into the
   DC, not from an argument, so the select/restore dance is required. */
static void circle(HDC dc, int cx, int cy, int r, u32 c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ old = SelectObject(dc, b);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, old);
    DeleteObject(b);
    g_cmds++;
}

static void text(HDC dc, int x, int y, const char *s, u32 c)
{
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, x, y, s, -1);
    g_cmds++;
}

/* The console font is a fixed 8x16 cell, so centring is exact. */
static int centred(int cx, const char *s)
{
    return cx - (lstrlenA(s) * 8) / 2;
}

/* A rounded "pill" the way Chrome draws the omnibox: a bar with a disc
   welded onto each end.  Three commands, no rounded-rect primitive
   needed (GDI here has none). */
static void pill(HDC dc, int x, int y, int w, int h, u32 c)
{
    int r = h / 2;
    circle(dc, x + r,     y + r, r, c);
    circle(dc, x + w - r, y + r, r, c);
    fill(dc, x + r, y, w - 2 * r, h, c);
}

/* ---- the page ----------------------------------------------------- */

static void paint(HDC dc)
{
    int i;
    static const u32 tile_col[4] = { C_BLUE, C_RED, C_GREEN, C_YELLOW };
    static const char *tile_txt[4] = { "Docs", "Files", "Term", "Store" };

    g_cmds = 0;

    /* --- tab strip ------------------------------------------------- */
    fill(dc, 0, 0, CW, 38, C_TABBAR);
    fill(dc, 8, 6, 230, 32, C_SURFACE);          /* active tab       */
    circle(dc, 26, 22, 7, C_BLUE);               /* favicon          */
    text(dc, 42, 15, "NexOS Start", C_TEXT);
    text(dc, 252, 15, "Docs", C_TEXT2);          /* background tab   */
    text(dc, 332, 15, "+", C_TEXT2);

    /* --- toolbar --------------------------------------------------- */
    fill(dc, 0, 38, CW, 42, C_SURFACE);
    text(dc, 18, 52, "<", C_TEXT2);
    text(dc, 46, 52, ">", C_TEXT2);
    text(dc, 74, 52, "@", C_TEXT2);              /* reload           */

    pill(dc, 100, 45, 670, 28, C_OMNIBOX);       /* omnibox: 3 cmds  */
    fill(dc, 128, 53, 8, 12, C_TEXT2);           /* padlock          */
    text(dc, 148, 52, "NexOS://start", C_TEXT);

    circle(dc, 800, 59, 13, C_BLUE);             /* profile avatar   */
    text(dc, 838, 52, ":", C_TEXT2);             /* overflow menu    */

    fill(dc, 0, 80, CW, 1, C_HAIRLINE);

    /* --- content --------------------------------------------------- */
    fill(dc, 0, 81, CW, CH - 81, C_SURFACE);

    /* Chrome mark: three brand discs arranged on a circle, with the
       white ring and blue core laid over them.  Real Chrome uses 120
       degree sectors; discs are the closest match available when the
       only primitive is a filled ellipse. */
    circle(dc, 440, 146, 30, C_RED);
    circle(dc, 419, 186, 30, C_GREEN);
    circle(dc, 461, 186, 30, C_YELLOW);
    circle(dc, 440, 170, 26, C_SURFACE);
    circle(dc, 440, 170, 20, C_BLUE);

    text(dc, centred(440, "NexOS Browser"), 222, "NexOS Browser", C_TEXT);

    pill(dc, 240, 256, 400, 40, C_OMNIBOX);      /* search box: 3    */
    text(dc, 288, 268, "Search or type a URL", C_TEXT2);

    /* --- shortcut tiles -------------------------------------------- */
    for (i = 0; i < 4; i++) {
        int cx = 320 + i * 80;
        circle(dc, cx, 360, 24, tile_col[i]);
        text(dc, centred(cx, tile_txt[i]), 396, tile_txt[i], C_TEXT2);
    }

    /* --- status bar ------------------------------------------------ */
    fill(dc, 0, CH - 24, CW, 24, C_OMNIBOX);
    text(dc, 12, CH - 18, "Ready - MiniPE subsystem", C_TEXT2);

    OutputDebugStringA(g_cmds <= 72
                       ? "[chrome] paint within display-list budget\n"
                       : "[chrome] WARNING display list overflowed, tail dropped\n");
}

/* ---- window procedure --------------------------------------------- */

static int WINAPI WndProc(HWND h, u32 msg, u32 wp, u32 lp)
{
    PAINTSTRUCT_ ps;
    HDC dc;

    switch (msg) {
    case WM_CREATE:
        OutputDebugStringA("[chrome] WM_CREATE\n");
        return 0;
    case WM_PAINT:
        dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* ---- entry point --------------------------------------------------
 *  win64_run calls this directly as `int rc = fn();` on the kernel
 *  stack, so it must return rather than loop forever.  GetMessageA in
 *  win32.cpp delivers a bounded stream and then reports 0, which makes
 *  the standard message loop terminate on its own.
 * ------------------------------------------------------------------ */
int PeMain(void)
{
    WNDCLASS_ wc;
    MSG_ msg;
    HWND h;

    OutputDebugStringA("[chrome] MiniPE browser starting\n");

    /* Field-by-field, deliberately: a struct initialiser would make gcc
       emit a memset call and there is no CRT to satisfy it. */
    wc.style = 0;
    wc.proc = WndProc;
    wc.cbCls = 0;
    wc.cbWnd = 0;
    wc.hInst = 0;
    wc.hIcon = 0;
    wc.hCursor = 0;
    wc.hbrBackground = 0;
    wc.menu = 0;
    wc.name = "ChromeWndClass";

    if (!RegisterClassA(&wc)) {
        OutputDebugStringA("[chrome] RegisterClassA failed\n");
        return 1;
    }

    h = CreateWindowExA(0, "ChromeWndClass", "Chrome",
                        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        40, 40, CW, CH, 0, 0, 0, 0);
    if (!h) {
        OutputDebugStringA("[chrome] CreateWindowExA failed\n");
        return 2;
    }

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);                 /* drives the first WM_PAINT */

    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    OutputDebugStringA("[chrome] MiniPE browser exiting\n");
    return 0;
}
