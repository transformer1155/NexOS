/* ---------------------------------------------------------------------
 *  iexplore.c  -  Internet Explorer, built as a genuine Win32 PE32 binary
 *
 *  Runs on the 32-bit kernel through win32_run() (PE32 i386).  It is the
 *  desktop Browser: a real PE with a real window procedure, painting
 *  through the MiniPE GDI display list.
 *
 *  WHAT CHANGED (interactivity)
 *  ----------------------------
 *  The first version only handled WM_CREATE / WM_PAINT / WM_DESTROY, so
 *  every click fell through to DefWindowProcA and the page looked frozen.
 *  gui.cpp was already synthesising WM_LBUTTONDOWN / WM_LBUTTONUP with a
 *  client-relative point and repainting afterwards -- nobody was
 *  listening.  This build listens: it keeps UI state in statics, hit
 *  tests the click against a table of rectangles, mutates the state and
 *  lets the host's follow-up WM_PAINT redraw.  Keyboard arrives the same
 *  way (WM_KEYDOWN / WM_CHAR), so the address bar and the two text boxes
 *  are real editable fields.
 *
 *  There is no InvalidateRect in this subsystem: the host repaints after
 *  every message it delivers, so a handler only has to change state.
 *
 *  AGENT BRIDGE
 *  ------------
 *  WM_NexOS_API (0x8000) is a private message the kernel's `webapi`
 *  shell verb posts here so an agent can drive the browser without
 *  synthesising pixels.  wParam is the verb, lParam its argument; the
 *  return value is either a small integer or a `char*` into a static
 *  buffer (kernel and PE share one address space).  The kernel side
 *  refuses to send anything until the caller has authenticated, so the
 *  verbs below are only reachable through that gate.
 *
 *  i386 NOTES
 *  ----------
 *  - Calls are real __stdcall, imports decorate to _Name@n, the entry
 *    symbol is _PeMain.  Link with -Wl,--entry=_PeMain.
 *  - i386 code is not position independent, so the image needs a .reloc
 *    section; build with --dynamicbase.
 *  - No CRT: never let gcc synthesise memset/memcpy.  Every buffer is a
 *    static array and every copy is an explicit loop.
 *
 *  DRAWING BUDGET: W32_MAX_CMDS is 320 and cmd_push() silently drops the
 *  overflow, so paint() counts what it asked for and says so on serial.
 * ------------------------------------------------------------------ */
#include "minipe.h"

#define CW 880          /* client width  */
#define CH 540          /* client height */

/* Classic Internet Explorer / Windows chrome palette */
#define C_FACE     RGB_(0xF0, 0xF0, 0xF0)   /* menu + toolbar face     */
#define C_SURFACE  RGB_(0xFF, 0xFF, 0xFF)   /* document background     */
#define C_FIELD    RGB_(0xFF, 0xFF, 0xFF)   /* edit control interior   */
#define C_BORDER   RGB_(0x7F, 0x9D, 0xB9)   /* classic edit border     */
#define C_FOCUS    RGB_(0xFF, 0x8C, 0x00)   /* focused edit border     */
#define C_HAIRLINE RGB_(0xD4, 0xD0, 0xC8)   /* 3D shadow line          */
#define C_TEXT     RGB_(0x00, 0x00, 0x00)
#define C_TEXT2    RGB_(0x55, 0x55, 0x55)
#define C_LINK     RGB_(0x00, 0x33, 0x99)
#define C_IEBLUE   RGB_(0x1E, 0x6C, 0xB8)   /* the "e"                 */
#define C_IESKY    RGB_(0x2E, 0xA5, 0xE0)   /* highlight on the "e"    */
#define C_GOLD     RGB_(0xFD, 0xB8, 0x13)   /* the orbit ring          */
#define C_GO       RGB_(0x3C, 0x8F, 0x3C)   /* Go button               */
#define C_ASK      RGB_(0x7A, 0x3C, 0xA8)   /* Ask-AI button           */
#define C_PANEL    RGB_(0xF6, 0xF4, 0xFB)   /* AI answer panel         */
#define C_DISABLED RGB_(0xA0, 0xA0, 0xA0)

/* ---- agent verbs (mirrored by cmd_webapi() in kernel.cpp) --------- */
#define API_PING      1     /* -> 0x1E1E1E1E                          */
#define API_GET_URL   2     /* -> char* current URL                   */
#define API_NAVIGATE  3     /* lp = char* url        -> 1             */
#define API_GET_TEXT  4     /* -> char* rendered page text            */
#define API_ASK       5     /* lp = char* question   -> char* answer  */
#define API_CLICK     6     /* lp = link index       -> 1 / 0         */
#define API_LINKS     7     /* -> char* newline separated link list   */
#define API_STATUS    8     /* -> char* one-line status               */

/* ---- which control owns the caret --------------------------------- */
#define FOCUS_NONE   0
#define FOCUS_ADDR   1
#define FOCUS_SEARCH 2
#define FOCUS_AI     3

/* ---- which document is showing ------------------------------------ */
#define PAGE_START  0
#define PAGE_DOC    1       /* a favourite / navigated URL            */
#define PAGE_SEARCH 2
#define PAGE_AI     3

/* ---- home page ------------------------------------------------------
 *  The browser opens on Bing.  There is still no network stack behind
 *  it, so what actually renders is the local start document -- but the
 *  address bar, the Home button and the first favourite all agree on
 *  the same URL, which is what "default home page" has to mean here.
 * -------------------------------------------------------------------- */
#define HOME_URL   "https://www.bing.com/"
#define HOME_TITLE "Bing"

#define NLINKS 4

static const char *g_link_name[NLINKS] = {
    HOME_TITLE, "Search", "Downloads", "Help"
};
static const char *g_link_url[NLINKS] = {
    HOME_URL,
    "http://start.NexOS/search",
    "http://start.NexOS/downloads",
    "http://start.NexOS/help"
};
static const char *g_link_body[NLINKS] = {
    "NexOS - a 32-bit hobby operating system with a managed C# shell.",
    "Type a phrase in the search box and press Go or Enter.",
    "iexplore.exe, hello32.exe, menu32.exe are already on this disk.",
    "Commands: ai init, agent init, webapi help, run <file>, winapp <file>."
};

/* ---- UI state (all statics: the PE has no heap of its own) -------- */
static char g_addr[128];
static char g_search[96];
static char g_ai_q[128];
static char g_ai_a[512];
static char g_status[96];
static char g_title[96];
static char g_body[256];
static char g_scratch[640];

static int  g_focus;
static int  g_page;
static int  g_hist;          /* previous page, for Back               */
static int  g_clicks;        /* how many clicks we have serviced      */
static int  g_cmds;          /* display-list entries we asked for     */
static int  g_ai_up;         /* 1 once NexOS.dll answered            */

/* Single-level undo for Ctrl+Z (snapshot of whichever field is focused). */
static char g_undo[160];
static int  g_undo_len = 0;

/* ---- clipboard / undo helpers ------------------------------------- */

/* Copy the currently focused field into the undo slot.  Call before any
   mutation so Ctrl+Z can restore it.  No-op when nothing is focused. */
static char *focus_buf(int *cap);   /* forward decl; defined below */
static void snap_undo(void)
{
    int cap;
    char *b = focus_buf(&cap);
    int n = 0;
    if (b) {
        while (b[n] && n < (int)sizeof(g_undo) - 1) { g_undo[n] = b[n]; n++; }
    }
    g_undo[n] = 0;
    g_undo_len = n;
}

/* ---- NexOS.dll, resolved at run time -----------------------------
 *  GetProcAddress goes through the loader's export resolver, so the AI
 *  engine is optional: if the kernel was built without it the browser
 *  still runs and just reports that the engine is unavailable.        */
typedef int (WINAPI *PFN_AI_READY)(void);
typedef int (WINAPI *PFN_AI_INIT)(void);
typedef int (WINAPI *PFN_AI_ASK)(const char *, char *, int);

/* Clipboard bridge into the kernel (NexOS.dll MiniClipGet / MiniClipSet),
   so the address bar / search / AI fields share one clipboard with the
   managed shell and the text-mode terminal. */
typedef int  (WINAPI *PFN_CLIP_GET)(char *, int);
typedef void (WINAPI *PFN_CLIP_SET)(const char *, int);
static PFN_CLIP_GET p_clip_get;
static PFN_CLIP_SET p_clip_set;

static PFN_AI_READY p_ai_ready;
static PFN_AI_INIT  p_ai_init;
static PFN_AI_ASK   p_ai_ask;

/* ---- freestanding string helpers ---------------------------------- */

static int slen(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void scpy(char *d, const char *s, int cap)
{
    int i = 0;
    if (!d || cap <= 0) return;
    if (s) while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void scat(char *d, const char *s, int cap)
{
    int n = slen(d), i = 0;
    if (!d || cap <= 0) return;
    if (s) while (s[i] && n + i < cap - 1) { d[n + i] = s[i]; i++; }
    d[n + i] = 0;
}

static void sputc(char *d, char c, int cap)
{
    int n = slen(d);
    if (n < cap - 1) { d[n] = c; d[n + 1] = 0; }
}

static void spop(char *d)
{
    int n = slen(d);
    if (n > 0) d[n - 1] = 0;
}

static void snum(char *d, int v, int cap)
{
    char t[12];
    int n = 0, i;
    if (v == 0) { scat(d, "0", cap); return; }
    if (v < 0)  { scat(d, "-", cap); v = -v; }
    while (v > 0 && n < 11) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = n - 1; i >= 0; i--) sputc(d, t[i], cap);
}

/* ---- drawing helpers (same contract as before) -------------------- */

static void fill(HDC dc, int x, int y, int w, int h, u32 c)
{
    HBRUSH b = CreateSolidBrush(c);
    RECT_ r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b);
    DeleteObject(b);
    g_cmds++;
}

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
    return cx - (slen(s) * 8) / 2;
}

/* A sunken edit control; `focused` swaps the border colour so the user
   can see which box will receive the next keystroke. */
static void field(HDC dc, int x, int y, int w, int h, int focused)
{
    fill(dc, x, y, w, h, focused ? C_FOCUS : C_BORDER);
    fill(dc, x + 1, y + 1, w - 2, h - 2, C_FIELD);
}

/* Draw an editable string, with a caret when the box has focus.  The
   text is clipped from the left so the tail stays visible while typing. */
static void field_text(HDC dc, int x, int y, int w, const char *s,
                       int focused, const char *placeholder)
{
    int cols = (w - 12) / 8;
    int n = slen(s);
    const char *v = s;
    if (n == 0 && !focused) { text(dc, x, y, placeholder, C_TEXT2); return; }
    if (n > cols) { v = s + (n - cols); n = cols; }
    scpy(g_scratch, v, cols + 1);
    if (focused) scat(g_scratch, "_", cols + 2);
    text(dc, x, y, g_scratch, C_TEXT);
}

/* ---- the IE mark --------------------------------------------------
 *  Built from filled discs: a gold ring (gold disc with a white disc
 *  punched out), a blue body, a highlight, a white crossbar and a white
 *  disc that bites the lower right open.
 * ------------------------------------------------------------------ */
static void ie_mark(HDC dc, int cx, int cy, int s)
{
    circle(dc, cx, cy, s, C_GOLD);
    circle(dc, cx, cy, (s * 42) / 52, C_SURFACE);
    circle(dc, cx, cy, (s * 38) / 52, C_IEBLUE);
    circle(dc, cx, cy - (s * 10) / 52, (s * 30) / 52, C_IESKY);
    fill(dc, cx - (s * 30) / 52, cy - (s * 6) / 52,
         (s * 60) / 52, (s * 10) / 52, C_SURFACE);
    circle(dc, cx + (s * 34) / 52, cy + (s * 26) / 52, (s * 20) / 52, C_SURFACE);
}

/* ---- navigation ---------------------------------------------------
 *  There is no network stack behind this: "navigating" means switching
 *  the document model and letting WM_PAINT redraw.  Everything an agent
 *  can ask for (URL, title, body, link list) comes out of these fields.
 * ------------------------------------------------------------------ */

static void set_status(const char *a, const char *b)
{
    g_status[0] = 0;
    scat(g_status, a, sizeof(g_status));
    if (b) scat(g_status, b, sizeof(g_status));
}

static void go_start(void)
{
    g_hist = g_page;
    g_page = PAGE_START;
    scpy(g_addr, HOME_URL, sizeof(g_addr));
    scpy(g_title, HOME_TITLE, sizeof(g_title));
    g_body[0] = 0;
    set_status("Done", 0);
}

static void go_doc(int i)
{
    if (i < 0 || i >= NLINKS) return;
    g_hist = g_page;
    g_page = PAGE_DOC;
    scpy(g_addr,  g_link_url[i],  sizeof(g_addr));
    scpy(g_title, g_link_name[i], sizeof(g_title));
    scpy(g_body,  g_link_body[i], sizeof(g_body));
    set_status("Done - ", g_link_name[i]);
}

/* Navigate to a typed URL.  A URL that matches one of the favourites
   opens that document; anything else gets a generic "not found" page,
   which is honest -- there is no resolver here. */
static int navigate(const char *url)
{
    int i, j;
    if (!url || !url[0]) return 0;
    for (i = 0; i < NLINKS; i++) {
        const char *a = g_link_url[i];
        for (j = 0; a[j] && url[j] && a[j] == url[j]; j++) { }
        if (!a[j] && !url[j]) {
            if (i == 0) { go_start(); scpy(g_addr, url, sizeof(g_addr)); }
            else        go_doc(i);
            return 1;
        }
    }
    g_hist = g_page;
    g_page = PAGE_DOC;
    scpy(g_addr, url, sizeof(g_addr));
    scpy(g_title, "Cannot find server", sizeof(g_title));
    scpy(g_body, "NexOS has no network stack, so this address cannot be "
                 "resolved.  Try one of the favourites.", sizeof(g_body));
    set_status("Cannot find server", 0);
    return 1;
}

static void do_search(void)
{
    if (!g_search[0]) { set_status("Type something to search for", 0); return; }
    g_hist = g_page;
    g_page = PAGE_SEARCH;
    g_addr[0] = 0;
    scat(g_addr, "http://start.NexOS/search?q=", sizeof(g_addr));
    scat(g_addr, g_search, sizeof(g_addr));
    g_title[0] = 0;
    scat(g_title, "Results for ", sizeof(g_title));
    scat(g_title, g_search, sizeof(g_title));
    set_status("Search complete", 0);
}

/* ---- the AI engine ------------------------------------------------
 *  NexOS.dll is resolved lazily: the first question brings the engine
 *  up, which takes a moment, so the status line says so.
 * ------------------------------------------------------------------ */
static int ai_bind(void)
{
    HINSTANCE m;
    if (p_ai_ask) return 1;
    m = LoadLibraryA("NexOS.dll");
    p_ai_ready = (PFN_AI_READY)GetProcAddress(m, "MiniAiReady");
    p_ai_init  = (PFN_AI_INIT) GetProcAddress(m, "MiniAiInit");
    p_ai_ask   = (PFN_AI_ASK)  GetProcAddress(m, "MiniAiAsk");
    p_clip_get = (PFN_CLIP_GET)GetProcAddress(m, "MiniClipGet");
    p_clip_set = (PFN_CLIP_SET)GetProcAddress(m, "MiniClipSet");
    return p_ai_ask ? 1 : 0;
}

/* Returns the answer buffer, always NUL terminated. */
static const char *ai_ask(const char *q)
{
    int n;
    if (!q || !q[0]) {
        scpy(g_ai_a, "Ask a question first.", sizeof(g_ai_a));
        return g_ai_a;
    }
    if (!ai_bind()) {
        scpy(g_ai_a, "NexOS.dll is not available in this kernel build.",
             sizeof(g_ai_a));
        return g_ai_a;
    }
    if (p_ai_ready && !p_ai_ready() && p_ai_init) p_ai_init();
    n = p_ai_ask(q, g_ai_a, sizeof(g_ai_a));
    if (n <= 0) {
        scpy(g_ai_a, "The AI engine did not answer (is it initialised?).",
             sizeof(g_ai_a));
        return g_ai_a;
    }
    g_ai_up = 1;
    OutputDebugStringA("[iexplore] AI answered\n");
    return g_ai_a;
}

/* ---- hit testing --------------------------------------------------
 *  One table, walked by the click handler and reused by the agent's
 *  CLICK verb so both paths take exactly the same route.
 * ------------------------------------------------------------------ */
static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* Toolbar geometry, shared by paint() and on_click() so the picture and
   the hit test can never drift apart. */
#define R_BACK    13, 29, 26, 26
#define R_FWD     45, 29, 26, 26
#define R_STOP    78, 30, 20, 24
#define R_REFRESH 102, 30, 20, 24
#define R_HOME    126, 30, 20, 24
#define R_ADDR    220, 30, 560, 24
#define R_GO      790, 30, 64, 24
/* Start-page geometry */
#define R_SEARCH  200, 172, 420, 30
#define R_SBTN    632, 173, 88, 28
#define R_AIQ     440, 266, 300, 28
#define R_AIBTN   748, 267, 72, 26
#define R_HOMELNK 60, 236, 220, 20

/* ---- the page ----------------------------------------------------- */

/* Draw `s` into `cols`-wide lines starting at (x,y), 16px leading. */
static int wrap_text(HDC dc, int x, int y, int cols, int maxlines,
                     const char *s, u32 c)
{
    int i = 0, line = 0;
    if (!s) return 0;
    while (s[i] && line < maxlines) {
        int n = 0, brk, j;
        while (s[i + n] && n < cols) n++;
        brk = n;
        if (s[i + n]) {
            int k = n;
            while (k > 0 && s[i + k] != ' ') k--;
            if (k > cols / 3) brk = k;
        }
        for (j = 0; j < brk; j++) g_scratch[j] = s[i + j];
        g_scratch[brk] = 0;
        text(dc, x, y + line * 16, g_scratch, c);
        i += brk;
        while (s[i] == ' ') i++;
        line++;
    }
    return line;
}

static void paint_start(HDC dc)
{
    int i;

    ie_mark(dc, 120, 150, 52);                       /* 6 cmds        */
    text(dc, 200, 116, "Internet Explorer", C_TEXT);
    text(dc, 200, 140, "MiniPE subsystem - 32-bit PE32 i386", C_TEXT2);

    /* search row */
    field(dc, R_SEARCH, g_focus == FOCUS_SEARCH);
    field_text(dc, 212, 179, 420, g_search,
               g_focus == FOCUS_SEARCH, "Search the web");
    fill(dc, R_SBTN, C_IEBLUE);
    text(dc, centred(676, "Search"), 179, "Search", C_SURFACE);

    fill(dc, 60, 222, 760, 1, C_HAIRLINE);

    /* favourites (left column) */
    text(dc, 60, 240, "Favorites", C_TEXT);
    for (i = 0; i < NLINKS; i++) {
        int y = 268 + i * 24;
        fill(dc, 64, y + 4, 8, 8, C_GOLD);
        text(dc, 80, y, g_link_name[i], C_LINK);
    }

    /* AI console (right column) */
    text(dc, 440, 240, "Ask NexOS AI", C_TEXT);
    field(dc, R_AIQ, g_focus == FOCUS_AI);
    field_text(dc, 452, 272, 300, g_ai_q,
               g_focus == FOCUS_AI, "Type a question");
    fill(dc, R_AIBTN, C_ASK);
    text(dc, centred(784, "Ask"), 272, "Ask", C_SURFACE);

    fill(dc, 440, 302, 380, 152, C_PANEL);
    fill(dc, 440, 302, 380, 1, C_HAIRLINE);
    if (g_ai_a[0]) wrap_text(dc, 448, 310, 45, 8, g_ai_a, C_TEXT);
    else           text(dc, 448, 310, "The local engine answers here.", C_TEXT2);
}

static void paint_doc(HDC dc)
{
    int i, lines;

    text(dc, 60, 100, g_title, C_TEXT);
    fill(dc, 60, 124, 760, 1, C_HAIRLINE);

    if (g_page == PAGE_SEARCH) {
        for (i = 0; i < NLINKS; i++) {
            int y = 150 + i * 40;
            text(dc, 60, y, g_link_name[i], C_LINK);
            text(dc, 60, y + 16, g_link_url[i], C_TEXT2);
        }
    } else if (g_page == PAGE_AI) {
        lines = wrap_text(dc, 60, 150, 92, 12, g_ai_a, C_TEXT);
        (void)lines;
    } else {
        wrap_text(dc, 60, 150, 92, 6, g_body, C_TEXT);
    }

    fill(dc, 60, 236, 8, 8, C_GOLD);
    text(dc, 80, 232, "Back to the start page", C_LINK);
}

static void paint(HDC dc)
{
    int i;
    static const char *menus[6] = { "File", "Edit", "View",
                                    "Favorites", "Tools", "Help" };
    static const int menu_x[6] = { 12, 56, 100, 148, 232, 284 };

    g_cmds = 0;

    /* --- menu bar --------------------------------------------------- */
    fill(dc, 0, 0, CW, 22, C_FACE);
    for (i = 0; i < 6; i++)
        text(dc, menu_x[i], 3, menus[i], C_TEXT);

    /* --- toolbar ---------------------------------------------------- */
    fill(dc, 0, 22, CW, 40, C_FACE);
    circle(dc, 26, 42, 13, g_page == PAGE_START ? C_DISABLED : C_IEBLUE);
    text(dc, 22, 34, "<", C_SURFACE);
    circle(dc, 58, 42, 13, C_DISABLED);
    text(dc, 54, 34, ">", C_SURFACE);
    text(dc, 82, 34, "X", C_TEXT2);              /* Stop              */
    text(dc, 106, 34, "@", C_TEXT2);             /* Refresh           */
    text(dc, 130, 34, "^", C_TEXT2);             /* Home              */

    /* --- address bar ------------------------------------------------ */
    text(dc, 156, 34, "Address", C_TEXT2);
    field(dc, R_ADDR, g_focus == FOCUS_ADDR);
    fill(dc, 226, 36, 12, 12, C_IESKY);
    field_text(dc, 246, 34, 528, g_addr, g_focus == FOCUS_ADDR, "");
    fill(dc, R_GO, C_GO);
    text(dc, centred(822, "Go"), 34, "Go", C_SURFACE);

    fill(dc, 0, 62, CW, 1, C_HAIRLINE);

    /* --- document --------------------------------------------------- */
    fill(dc, 0, 63, CW, CH - 63 - 24, C_SURFACE);
    if (g_page == PAGE_START) paint_start(dc);
    else                      paint_doc(dc);

    /* --- status bar -------------------------------------------------- */
    fill(dc, 0, CH - 24, CW, 24, C_FACE);
    fill(dc, 0, CH - 24, CW, 1, C_HAIRLINE);
    text(dc, 12, CH - 18, g_status, C_TEXT2);
    text(dc, 660, CH - 18, "Internet | Protected Mode", C_TEXT2);

    OutputDebugStringA(g_cmds <= 320
                       ? "[iexplore] paint within display-list budget\n"
                       : "[iexplore] WARNING display list overflowed\n");
}

/* ---- input --------------------------------------------------------
 *  gui.cpp hands us a client-relative point and repaints afterwards,
 *  so a handler only has to mutate state.
 * ------------------------------------------------------------------ */

static void activate_link(int i)
{
    if (i < 0 || i >= NLINKS) return;
    if (i == 0) go_start();
    else        go_doc(i);
}

static void log_addr(void);

static void on_click(int x, int y)
{
    int i;
    g_clicks++;

    /* --- toolbar ----------------------------------------------------- */
    if (hit(x, y, R_BACK)) {
        if (g_page != PAGE_START) { go_start(); set_status("Back", 0); }
        else set_status("Nothing to go back to", 0);
        return;
    }
    if (hit(x, y, R_FWD))     { set_status("Forward is disabled", 0); return; }
    if (hit(x, y, R_STOP))    { set_status("Stopped", 0); return; }
    if (hit(x, y, R_REFRESH)) { set_status("Refreshed ", g_addr); return; }
    if (hit(x, y, R_HOME))    { go_start(); set_status("Home", 0); return; }
    if (hit(x, y, R_ADDR))    { g_focus = FOCUS_ADDR;
                                set_status("Editing the address", 0);
                                log_addr(); return; }
    if (hit(x, y, R_GO))      { navigate(g_addr); return; }

    /* --- document ---------------------------------------------------- */
    if (g_page == PAGE_START) {
        if (hit(x, y, R_SEARCH)) { g_focus = FOCUS_SEARCH;
                                   set_status("Type a search phrase", 0); return; }
        if (hit(x, y, R_SBTN))   { do_search(); return; }
        if (hit(x, y, R_AIQ))    { g_focus = FOCUS_AI;
                                   set_status("Type a question for the AI", 0); return; }
        if (hit(x, y, R_AIBTN))  { set_status("Thinking...", 0);
                                   ai_ask(g_ai_q);
                                   set_status("AI answered", 0); return; }
        for (i = 0; i < NLINKS; i++)
            if (hit(x, y, 60, 264 + i * 24, 260, 22)) { activate_link(i); return; }
    } else {
        if (hit(x, y, R_HOMELNK)) { go_start(); return; }
        if (g_page == PAGE_SEARCH)
            for (i = 0; i < NLINKS; i++)
                if (hit(x, y, 60, 146 + i * 40, 400, 34)) { activate_link(i); return; }
    }

    g_focus = FOCUS_NONE;
    set_status("Ready", 0);
}

/* Enter in a field performs that field's default action. */
static void on_enter(void)
{
    if (g_focus == FOCUS_ADDR)        navigate(g_addr);
    else if (g_focus == FOCUS_SEARCH) do_search();
    else if (g_focus == FOCUS_AI)     { ai_ask(g_ai_q); set_status("AI answered", 0); }
}

static char *focus_buf(int *cap)
{
    if (g_focus == FOCUS_ADDR)   { *cap = (int)sizeof(g_addr);   return g_addr; }
    if (g_focus == FOCUS_SEARCH) { *cap = (int)sizeof(g_search); return g_search; }
    if (g_focus == FOCUS_AI)     { *cap = (int)sizeof(g_ai_q);   return g_ai_q; }
    *cap = 0;
    return 0;
}

static void log_addr(void)
{
    static const char p[] = "[iexplore] addr=";
    char buf[160];
    int i = 0;
    while (p[i]) { buf[i] = p[i]; i++; }
    int j = 0;
    while (g_addr[j] && i + j < (int)sizeof(buf) - 2) { buf[i + j] = g_addr[j]; j++; }
    buf[i + j] = '\n';
    buf[i + j + 1] = 0;
    OutputDebugStringA(buf);
}

static void on_char(int ch)
{
    int cap;
    char *b = focus_buf(&cap);
    if (!b) return;
    snap_undo();                                 /* enable Ctrl+Z for this edit */
    if (ch == VK_BACK || ch == 8) { spop(b); log_addr(); return; }
    if (ch == '\n' || ch == '\r') return;         /* handled in WM_KEYDOWN */
    if (ch < 0x20 || ch > 0x7E) return;
    sputc(b, (char)ch, cap);
    log_addr();
}

/* Terminal-style shortcut combos posted by the GUI (WM_NexOS_CTRL). */
static void handle_ctrl(int code)
{
    int cap;
    char *b = focus_buf(&cap);
    OutputDebugStringA("[iexplore] ctrl code=");
    { char dbg[8]; int i=0; if (code<0) dbg[i++]='-'; int v=code<0?-code:code; if(v==0)dbg[i++]='0'; else {char t[8];int n=0;while(v){t[n++]='0'+v%10;v/=10;}while(n)dbg[i++]=t[--n];} dbg[i]=0; OutputDebugStringA(dbg); }
    OutputDebugStringA(" clipget=");
    { int v=(int)(p_clip_get!=0); char dbg[8]; int i=0; if(v==0)dbg[i++]='0'; else {char t[8];int n=0;while(v){t[n++]='0'+v%10;v/=10;}while(n)dbg[i++]=t[--n];} dbg[i]=0; OutputDebugStringA(dbg); }
    OutputDebugStringA("\n");
    if (!b) return;                              /* nothing focused: ignore */
    if (code == 1) {                             /* Ctrl+C : copy field -> clipboard */
        if (p_clip_set) p_clip_set(b, slen(b));
        set_status("Copied", 0);
    }
    else if (code == 2) {                        /* Ctrl+V : paste clipboard -> field */
        if (p_clip_get) {
            char tmp[256];
            int n = p_clip_get(tmp, (int)sizeof(tmp) - 1);
            if (n > 0) {
                tmp[n] = 0;
                snap_undo();
                scpy(b, tmp, cap);
                log_addr();
                set_status("Pasted", 0);
            }
        }
    }
    else if (code == 3) {                        /* Ctrl+Z : undo last edit */
        if (g_undo_len > 0) {
            scpy(b, g_undo, cap);
            log_addr();
            set_status("Undo", 0);
        }
    }
    else if (code == 4) {                        /* Ctrl+A : select all (copy whole) */
        if (p_clip_set) p_clip_set(b, slen(b));
        set_status("Selected all", 0);
    }
}

/* ---- the agent bridge --------------------------------------------
 *  Reachable only through the kernel's authenticated `webapi` verb.
 * ------------------------------------------------------------------ */
static char g_api[640];

static u32 on_api(u32 verb, u32 arg)
{
    int i;
    switch (verb) {
    case API_PING:
        return 0x1E1E1E1Eu;

    case API_GET_URL:
        return (u32)g_addr;

    case API_NAVIGATE:
        if (!arg) return 0;
        scpy(g_addr, (const char *)arg, sizeof(g_addr));
        return (u32)navigate(g_addr);

    case API_GET_TEXT:
        g_api[0] = 0;
        scat(g_api, g_title, sizeof(g_api));
        scat(g_api, "\n", sizeof(g_api));
        if (g_page == PAGE_START) {
            scat(g_api, "Internet Explorer - MiniPE subsystem\n", sizeof(g_api));
            for (i = 0; i < NLINKS; i++) {
                scat(g_api, "  * ", sizeof(g_api));
                scat(g_api, g_link_name[i], sizeof(g_api));
                scat(g_api, "\n", sizeof(g_api));
            }
            if (g_ai_a[0]) { scat(g_api, "AI: ", sizeof(g_api));
                             scat(g_api, g_ai_a, sizeof(g_api)); }
        } else if (g_page == PAGE_AI) {
            scat(g_api, g_ai_a, sizeof(g_api));
        } else {
            scat(g_api, g_body, sizeof(g_api));
        }
        return (u32)g_api;

    case API_ASK:
        if (!arg) return 0;
        scpy(g_ai_q, (const char *)arg, sizeof(g_ai_q));
        ai_ask(g_ai_q);
        g_hist = g_page;
        g_page = PAGE_AI;
        g_title[0] = 0;
        scat(g_title, "AI: ", sizeof(g_title));
        scat(g_title, g_ai_q, sizeof(g_title));
        set_status("AI answered (agent)", 0);
        return (u32)g_ai_a;

    case API_CLICK:
        if ((int)arg < 0 || (int)arg >= NLINKS) return 0;
        activate_link((int)arg);
        return 1;

    case API_LINKS:
        g_api[0] = 0;
        for (i = 0; i < NLINKS; i++) {
            snum(g_api, i, sizeof(g_api));
            scat(g_api, "  ", sizeof(g_api));
            scat(g_api, g_link_name[i], sizeof(g_api));
            scat(g_api, "  ", sizeof(g_api));
            scat(g_api, g_link_url[i], sizeof(g_api));
            scat(g_api, "\n", sizeof(g_api));
        }
        return (u32)g_api;

    case API_STATUS:
        g_api[0] = 0;
        scat(g_api, "page=", sizeof(g_api));
        snum(g_api, g_page, sizeof(g_api));
        scat(g_api, " clicks=", sizeof(g_api));
        snum(g_api, g_clicks, sizeof(g_api));
        scat(g_api, " focus=", sizeof(g_api));
        snum(g_api, g_focus, sizeof(g_api));
        scat(g_api, " ai=", sizeof(g_api));
        scat(g_api, g_ai_up ? "up" : "idle", sizeof(g_api));
        scat(g_api, " url=", sizeof(g_api));
        scat(g_api, g_addr, sizeof(g_api));
        return (u32)g_api;
    }
    return 0;
}

/* ---- window procedure --------------------------------------------- */

static int WINAPI WndProc(HWND h, u32 msg, u32 wp, u32 lp)
{
    PAINTSTRUCT_ ps;
    HDC dc;

    switch (msg) {
    case WM_CREATE:
        OutputDebugStringA("[iexplore] WM_CREATE\n");
        go_start();
        OutputDebugStringA("[iexplore] home " HOME_URL "\n");
        g_focus = FOCUS_NONE;
        return 0;

    case WM_PAINT:
        dc = BeginPaint(h, &ps);
        paint(dc);
        EndPaint(h, &ps);
        return 0;

    case WM_LBUTTONDOWN:
        on_click(LOWORD_(lp), HIWORD_(lp));
        OutputDebugStringA("[iexplore] WM_LBUTTONDOWN handled\n");
        return 0;

    case WM_LBUTTONUP:
        return 0;

    case WM_KEYDOWN:
        /* Backspace is deliberately NOT serviced here.  TranslateMessage
           follows every VK_BACK with a WM_CHAR whose wParam is 8, so
           handling both messages popped two characters off the focused
           field for a single keystroke -- the address bar looked like it
           refused to delete anything sane.  Editing lives in WM_CHAR. */
        if (wp == VK_RETURN)      on_enter();
        else if (wp == VK_ESCAPE) { g_focus = FOCUS_NONE; set_status("Ready", 0); }
        return 0;

    case WM_CHAR:
        on_char((int)wp);
        return 0;

    case WM_NexOS_API:
        return (int)on_api(wp, lp);

    case WM_NexOS_CTRL:
        handle_ctrl((int)wp);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* ---- entry point --------------------------------------------------
 *  win32_run calls this as `int rc = fn();` on the kernel stack, so it
 *  must return.  GetMessageA delivers a bounded stream and then reports
 *  0, which terminates the standard message loop on its own.
 * ------------------------------------------------------------------ */
int PeMain(void)
{
    WNDCLASS_ wc;
    MSG_ msg;
    HWND h;

    OutputDebugStringA("[iexplore] MiniPE browser starting\n");

    /* Field by field on purpose: a struct initialiser makes gcc emit a
       memset call and there is no CRT here to satisfy it. */
    wc.style = 0;
    wc.proc = WndProc;
    wc.cbCls = 0;
    wc.cbWnd = 0;
    wc.hInst = 0;
    wc.hIcon = 0;
    wc.hCursor = 0;
    wc.hbrBackground = 0;
    wc.menu = 0;
    wc.name = "IEFrame";

    if (!RegisterClassA(&wc)) {
        OutputDebugStringA("[iexplore] RegisterClassA failed\n");
        return 1;
    }

    h = CreateWindowExA(0, "IEFrame", "Internet Explorer",
                        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        40, 40, CW, CH, 0, 0, 0, 0);
    if (!h) {
        OutputDebugStringA("[iexplore] CreateWindowExA failed\n");
        return 2;
    }

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);                 /* drives the first WM_PAINT */

    while (GetMessageA(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    OutputDebugStringA("[iexplore] MiniPE browser exiting\n");
    return 0;
}
