/* ---------------------------------------------------------------------
 *  minipe.h  -  the NexOS "MiniPE" subsystem SDK
 *
 *  This is the contract between a native Win64 PE32+ program and the
 *  loader in win32.cpp.  It plays the same role that the Windows SDK
 *  headers play on a real machine, but it only declares what NexOS
 *  actually implements, so a program that compiles against this header
 *  cannot accidentally import a function the loader would refuse.
 *
 *  WHY NOT <windows.h>
 *  -------------------
 *  Two reasons, and the second one is the one that bites.
 *
 *  1. Size.  windows.h drags in the whole CRT startup and thousands of
 *     declarations; we build with -nostdlib and a 192 KiB image budget.
 *
 *  2. ABI.  win32.cpp keeps a single set of structures shared by its
 *     32-bit and 64-bit loader paths, so handles here are uint32_t and
 *     wParam/lParam are 32-bit -- on real Win64 they are 64-bit.  A
 *     program built against the genuine SDK would therefore hand the
 *     loader a WNDCLASSA whose fields sit at the wrong offsets and the
 *     window procedure pointer would be read out of padding.
 *
 *  That divergence is a known debt: until win64_run gets its own
 *  LLP64-correct structure set, "MiniPE" is a real PE32+ that speaks a
 *  32-bit-handle dialect of Win32.  Everything else -- the file format,
 *  the import table, the ms_abi calling convention, the entry point --
 *  is genuine, which is why these binaries run through the same
 *  win64_run path as any other PE32+.
 * ------------------------------------------------------------------ */
#ifndef MINIPE_H
#define MINIPE_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef int                i32;

/* Handles are 32-bit tokens, not pointers - see the ABI note above. */
typedef u32 HWND;
typedef u32 HDC;
typedef u32 HBRUSH;
typedef u32 HPEN;
typedef u32 HFONT;
typedef u32 HICON;
typedef u32 HCURSOR;
typedef u32 HINSTANCE;
typedef u32 HGDIOBJ;

#define WINAPI __stdcall          /* ignored on x86-64; ms_abi is implied */
#define NULL_  0

/* ---- messages the loader can actually deliver --------------------- */
#define WM_CREATE   0x0001
#define WM_DESTROY  0x0002
#define WM_SIZE     0x0005
#define WM_PAINT    0x000F
#define WM_CLOSE    0x0010
#define WM_QUIT     0x0012
#define WM_TIMER    0x0113

/* Input.  gui.cpp synthesises these from the real mouse and keyboard
   and delivers them through win32_window_dispatch(), then repaints the
   window, so a program only has to update state and redraw in WM_PAINT
   -- there is no InvalidateRect in this subsystem.
   lParam of the mouse messages packs the CLIENT-relative point as
   (y << 16) | x, exactly like Windows. */
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_CHAR         0x0102
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202

/* Virtual keys forwarded by the host (see gui.cpp key routing). */
#define VK_BACK    0x08
#define VK_RETURN  0x0D
#define VK_ESCAPE  0x1B

#define LOWORD_(l) ((int)((l) & 0xFFFF))
#define HIWORD_(l) ((int)(((l) >> 16) & 0xFFFF))

/* Private message the NexOS agent bridge posts into a window so an
   external caller (the `webapi` shell verb) can drive the program
   without synthesising pixels.  wParam is the verb, lParam its
   argument; the return value is the program's answer.  See the verb
   table in iexplore.c and cmd_webapi() in kernel.cpp. */
#define WM_NexOS_API 0x8000

/* Private message the GUI posts into a native Win32 window when the user
   presses a terminal-style shortcut (Ctrl+C / V / Z / A) while that window
   is focused.  wParam is the combo code:
       1 = Ctrl+C (copy focused field to clipboard)
       2 = Ctrl+V (paste clipboard into focused field)
       3 = Ctrl+Z (undo last edit)
       4 = Ctrl+A (select all: copy whole focused field)
   Must stay in sync with gui.cpp's handle_ctrl() dispatch. */
#define WM_NexOS_CTRL 0x8001

/* ---- window styles ------------------------------------------------ */
#define WS_OVERLAPPEDWINDOW 0x00CF0000
#define WS_VISIBLE          0x10000000
#define SW_SHOW             5

/* ---- GDI ---------------------------------------------------------- */
#define TRANSPARENT 1
#define OPAQUE      2

#define RGB_(r, g, b) ((u32)((u8)(r) | ((u16)(u8)(g) << 8) | ((u32)(u8)(b) << 16)))

typedef int(WINAPI *WNDPROC)(HWND, u32, u32, u32);

typedef struct { i32 left, top, right, bottom; } RECT_;
typedef struct { HWND hwnd; u32 message, wParam, lParam, time; i32 px, py; } MSG_;
typedef struct {
    HDC  hdc;
    i32  fErase;
    RECT_ rc;
    i32  restore, incUpdate;
    u8   rgb[32];
} PAINTSTRUCT_;

/* Layout mirrors struct W32WndClassA in win32.cpp exactly.  The four
   handle fields are 32-bit, which is what puts `menu` at offset 40. */
typedef struct {
    u32       style;
    WNDPROC   proc;
    i32       cbCls, cbWnd;
    HINSTANCE hInst;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    const char *menu;
    const char *name;
} WNDCLASS_;

/* ---- kernel32 ----------------------------------------------------- */
__declspec(dllimport) void WINAPI ExitProcess(u32);
__declspec(dllimport) void WINAPI OutputDebugStringA(const char *);
__declspec(dllimport) const char *WINAPI GetCommandLineA(void);
__declspec(dllimport) u32  WINAPI GetTickCount(void);
__declspec(dllimport) i32  WINAPI lstrlenA(const char *);
__declspec(dllimport) char *WINAPI lstrcpyA(char *, const char *);
__declspec(dllimport) char *WINAPI lstrcatA(char *, const char *);
__declspec(dllimport) HINSTANCE WINAPI LoadLibraryA(const char *);
__declspec(dllimport) void *WINAPI GetProcAddress(HINSTANCE, const char *);

/* ---- user32 ------------------------------------------------------- */
__declspec(dllimport) u16  WINAPI RegisterClassA(const WNDCLASS_ *);
__declspec(dllimport) HWND WINAPI CreateWindowExA(u32, const char *, const char *,
                                                  u32, int, int, int, int,
                                                  HWND, u32, HINSTANCE, void *);
__declspec(dllimport) int  WINAPI ShowWindow(HWND, int);
__declspec(dllimport) int  WINAPI UpdateWindow(HWND);
__declspec(dllimport) int  WINAPI GetClientRect(HWND, RECT_ *);
__declspec(dllimport) int  WINAPI GetMessageA(MSG_ *, HWND, u32, u32);
__declspec(dllimport) int  WINAPI TranslateMessage(const MSG_ *);
__declspec(dllimport) int  WINAPI DispatchMessageA(const MSG_ *);
__declspec(dllimport) void WINAPI PostQuitMessage(int);
__declspec(dllimport) int  WINAPI DefWindowProcA(HWND, u32, u32, u32);
__declspec(dllimport) HDC  WINAPI BeginPaint(HWND, PAINTSTRUCT_ *);
__declspec(dllimport) int  WINAPI EndPaint(HWND, const PAINTSTRUCT_ *);
__declspec(dllimport) int  WINAPI FillRect(HDC, const RECT_ *, HBRUSH);
__declspec(dllimport) int  WINAPI SetWindowTextA(HWND, const char *);

/* ---- gdi32 -------------------------------------------------------- */
__declspec(dllimport) HBRUSH  WINAPI CreateSolidBrush(u32);
__declspec(dllimport) HGDIOBJ WINAPI SelectObject(HDC, HGDIOBJ);
__declspec(dllimport) int     WINAPI DeleteObject(HGDIOBJ);
__declspec(dllimport) u32     WINAPI SetTextColor(HDC, u32);
__declspec(dllimport) u32     WINAPI SetBkColor(HDC, u32);
__declspec(dllimport) int     WINAPI SetBkMode(HDC, int);
__declspec(dllimport) int     WINAPI TextOutA(HDC, int, int, const char *, int);
__declspec(dllimport) int     WINAPI Rectangle(HDC, int, int, int, int);
__declspec(dllimport) int     WINAPI Ellipse(HDC, int, int, int, int);
__declspec(dllimport) int     WINAPI MoveToEx(HDC, int, int, void *);
__declspec(dllimport) int     WINAPI LineTo(HDC, int, int);
__declspec(dllimport) HPEN    WINAPI CreatePen(int, int, u32);

/* ---- NexOS.dll: the parts of NexOS that are not Win32 -----------
 *  Everything above imitates Windows.  This one does not: it is the
 *  local AI engine (ai_engine.cpp) exposed to PE programs, which is
 *  how the browser start page answers a question without a network.
 *
 *      int MiniAiReady(void)   1 once the engine is up, 0 before.
 *      int MiniAiInit(void)    bring it up (idempotent); 0 on success.
 *      int MiniAiAsk(const char *prompt, char *out, int outsz)
 *                              generate a completion into `out`;
 *                              returns bytes written, < 0 on error.
 *
 *  There is also a networking bridge, used by winpe/ntbrowser.c so a native
 *  PE browser can do a REAL HTTP fetch (winpe/iexplore.c has no network):
 *
 *      int MiniHttpGet(const char *url, char *out, int outsz)
 *                              synchronous GET (drives net.cpp net_http_get
 *                              to completion); copies the response body,
 *                              NUL-terminated, into `out`.  Returns bytes
 *                              copied, or -1 on a bad/busy URL.  No TLS, so
 *                              only http:// is reachable.
 *
 *  These are deliberately NOT declared as __declspec(dllimport): there
 *  is no import library for NexOS.dll on the build host, and a static
 *  import would also make the browser refuse to load on a kernel built
 *  without the AI engine.  Bind them at run time instead --
 *
 *      HINSTANCE m = LoadLibraryA("NexOS.dll");
 *      PFN f = (PFN)GetProcAddress(m, "MiniAiAsk");
 *
 *  which the loader satisfies from its own export table (win32.cpp).
 * ------------------------------------------------------------------ */

#endif /* MINIPE_H */
