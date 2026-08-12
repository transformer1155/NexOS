/* =============================================================================
 *  ntbrowser.c  -  a real networked "NT" browser that runs as a native PE32
 *  i386 guest on the NexOS win32/wine loader (win32.cpp win32_run).
 *
 *  Unlike winpe/iexplore.c (which only swaps between locally-stored documents
 *  and has NO network stack), this browser actually fetches pages over HTTP.
 *  It reaches the kernel's HTTP client through the new NexOS.dll bridge
 *  MiniHttpGet, bound at run time via LoadLibraryA("NexOS.dll") +
 *  GetProcAddress, exactly like the AI engine bridge -- so it works on any
 *  kernel build that links net.cpp, and degrades gracefully when networking
 *  is unavailable.
 *
 *  Rendered output is the fetched body as wrapped monochrome text.  GDI goes
 *  through the same display-list surface as iexplore (CreateSolidBrush /
 *  FillRect / TextOutA ... consumed by gui.cpp draw_win32_app), so every
 *  primitive counts against the W32_MAX_CMDS budget; we cap how many lines
 *  we paint instead of overflowing.
 *
 *  Build:  make winpe   (target winpe/ntbrowser.exe, entry _PeMain)
 *  Run:    winapp ntbrowser.exe  (32-bit kernel) or open the Browser icon
 * ============================================================================= */
#include "minipe.h"
#include "minijs.h"      /* tiny integer JS subset for <script> blocks */

/* ---- freestanding helpers minijs.c needs (minijs.c runs in MINIJS_HOST off
        the host test; inside the PE it calls these). ----------------------- */
#include <stddef.h>
void mjs_memmove(void*d, const void*s, int n){ unsigned char*dd=(unsigned char*)d; const unsigned char*ss=(const unsigned char*)s; int i; if(dd<ss){ for(i=0;i<n;i++)dd[i]=ss[i]; } else { for(i=n-1;i>=0;i--)dd[i]=ss[i]; } }
int  mjs_strlen(const char*s){ int n=0; while(s&&s[n])n++; return n; }
int  mjs_strncmp(const char*a,const char*b){ if(!a||!b)return a==b?0:1; while(*a&&*b&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }

/* ---- palette --------------------------------------------------------------- */
#define C_FACE     RGB_(0xF0, 0xF0, 0xF0)   /* toolbar face            */
#define C_SURFACE  RGB_(0xFF, 0xFF, 0xFF)   /* document background     */
#define C_FIELD    RGB_(0xFF, 0xFF, 0xFF)   /* edit control interior   */
#define C_BORDER   RGB_(0x7F, 0x9D, 0xB9)   /* classic edit border     */
#define C_FOCUS    RGB_(0xFF, 0x8C, 0x00)   /* focused edit border     */
#define C_HAIRLINE RGB_(0xD4, 0xD0, 0xC8)   /* 3D shadow line          */
#define C_TEXT     RGB_(0x00, 0x00, 0x00)
#define C_TEXT2    RGB_(0x55, 0x55, 0x55)
#define C_LINK     RGB_(0x00, 0x33, 0x99)
#define C_NTBRO    RGB_(0x1E, 0x6C, 0xB8)   /* NT browser accent       */
#define C_GO       RGB_(0x3C, 0x8F, 0x3C)   /* Go button               */

/* ---- page state ------------------------------------------------------------ */
#define MODE_START 0
#define MODE_DOC   1
static int g_mode   = MODE_START;   /* what the content area shows   */
static int g_hist   = MODE_START;   /* previous mode (Back)          */
static int g_focus  = 0;            /* 1 when addressing             */
static int g_cmds   = 0;            /* display-list budget counter   */
static int g_loaded = 0;            /* last MiniHttpGet byte count   */
static int g_w = 860, g_h = 560;    /* client size (set in WM_PAINT) */

#define MAX_BODY 2304
static char g_addr[160];            /* current address / typed url   */
static char g_title[96];
static char g_status[96];
static char g_body[MAX_BODY];       /* fetched (or local) document   */
static char g_scratch[320];

/* ---- NexOS.dll bridge, resolved at run time --------------------------------
 *  MiniHttpGet(url, out, outsize) -> net_http_get (kernel net.cpp), i.e. a
 *  synchronous fetch.  Returns bytes copied or -1.  Bind lazily.         */
typedef int (WINAPI *PFN_HTTP_GET)(const char *, char *, int);
static PFN_HTTP_GET p_http_get;

static int http_bind(void)
{
    HINSTANCE m;
    if (p_http_get) return 1;
    m = LoadLibraryA("NexOS.dll");
    p_http_get = (PFN_HTTP_GET)GetProcAddress(m, "MiniHttpGet");
    if (p_http_get) OutputDebugStringA("[ntbrowser] MiniHttpGet bridge up\n");
    else            OutputDebugStringA("[ntbrowser] MiniHttpGet bridge MISSING\n");
    return p_http_get ? 1 : 0;
}

/* ---- freestanding string helpers ----------------------------------------- */
static int slen(const char *s){ int n=0; if(!s)return 0; while(s[n])n++; return n; }
static void scpy(char *d, const char *s, int cap){
    int i=0; if(!d||cap<=0)return; if(s)while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0;
}
static void scat(char *d, const char *s, int cap){
    int n=slen(d),i=0; if(!d||cap<=0)return;
    if(s)while(s[i]&&n+i<cap-1){d[n+i]=s[i];i++;} d[n+i]=0;
}
static void sputc(char *d, char c, int cap){
    int n=slen(d); if(n<cap-1){d[n]=c;d[n+1]=0;}
}
static void spop(char *d){ int n=slen(d); if(n>0)d[n-1]=0; }
static void snum(char *d, int v, int cap){
    char t[12]; int n=0,i;
    if(v==0){ scat(d,"0",cap); return; }
    if(v<0){ scat(d,"-",cap); v=-v; }
    while(v>0&&n<11){ t[n++]=(char)('0'+(v%10)); v/=10; }
    for(i=n-1;i>=0;i--) sputc(d,t[i],cap);
}

/* ---- drawing primitives (same display-list contract as iexplore) ---------- */
static void fill(HDC dc, int x, int y, int w, int h, u32 c){
    HBRUSH b = CreateSolidBrush(c); RECT_ r;
    r.left=x; r.top=y; r.right=x+w; r.bottom=y+h;
    FillRect(dc, &r, b); DeleteObject(b); g_cmds++;
}
static void text(HDC dc, int x, int y, const char *s, u32 c){
    SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, x, y, s, -1); g_cmds++;
}
/* Let me draw the accent glyph: a filled roundish "NT" chip. */
static void badge(HDC dc, int x, int y, int s, u32 c){
    fill(dc, x, y, s, s, c);
}
static void field(HDC dc, int x, int y, int w, int h, int focused){
    fill(dc, x, y, w, h, focused ? C_FOCUS : C_BORDER);
    fill(dc, x+1, y+1, w-2, h-2, C_FIELD);
}
static void field_text(HDC dc, int x, int y, int w, const char *s,
                       int focused, const char *placeholder){
    int cols=(w-12)/8, n=slen(s); const char *v=s;
    if(n==0&&!focused){ text(dc,x,y,placeholder,C_TEXT2); return; }
    if(n>cols){ v=s+(n-cols); n=cols; }
    scpy(g_scratch, v, cols+1);
    if(focused) scat(g_scratch, "_", cols+2);
    text(dc,x,y,g_scratch,C_TEXT);
}

/* Wrap `body` into `lines`-worth of 8x16 text starting at (x,y), clipping to
   content width `w`.  Draws at most `maxw` lines to respect the command budget
   (each line is one TextOutA); returns the number of lines actually drawn.  */
static int wrap_draw(HDC dc, int x, int y, int w, int lineh, int maxw,
                     const char *body)
{
    int cols = (w - 8) / 8;      /* glyphs per line */
    if (cols < 4) cols = 4;
    int i = 0, line = 0, drawn = 0;
    while (body[i] && line < maxw)
    {
        int j = i, cur = 0;
        while (body[j]) {
            if (body[j] == '\n') { j++; break; }
            cur++; if (cur >= cols) break;
            j++;
        }
        if (line < maxw) {
            scpy(g_scratch, body + i, cols + 1);
            text(dc, x, y + line * lineh, g_scratch, C_TEXT);
        }
        line++; drawn++; i = j;
    }
    return drawn;
}

/* ---- mini-JS in <script> -------------------------------------------
 *  A fetched page may carry a "<script>...</script>" block.  We run its
 *  contents through the integer JS subset (minijs.c) and append the last
 *  expression value to the rendered body, so a page can compute nothing
 *  interesting visually.  This is a text-only bridge: actual JS DOM
 *  manipulation is out of scope for a monospace terminal browser.        */

static void set_status(const char *a, const char *b)
{
    g_status[0] = 0;
    scat(g_status, a, sizeof(g_status));
    if (b) scat(g_status, b, sizeof(g_status));
}

static void eval_scripts(void)
{
    int i, j;
    const char *const open = "<script";
    int ok = 0;
    for (i = 0; g_body[i]; i++){
        const char *os = &g_body[i];
        int a = 0; while (open[a] && os[a] && os[a]==open[a]) a++;
        if (!open[a]){                     /* found a <script ...>         */
            const char *gt = os + a;
            while (*gt && *gt != '>') gt++;   /* skip attributes           */
            if (*gt != '>') continue;
            const char *start = gt + 1;    /* body starts after '>'       */
            const char *end = start;       /* find "</script"             */
            while (*end && !(end[0]=='<' && end[1]=='/' && end[2]=='s'))
                end++;
            int len = (int)(end - start);
            if (len > 0 && len < (int)sizeof(g_scratch)){
                char js[300];
                for (j = 0; j < len && j < 299; j++) js[j] = start[j];
                js[len < 299 ? len : 299] = 0;
                int res=0; char err[64]; err[0]=0;
                int jsrc = minijs_run(js, &res, err, (int)sizeof(err));
                if (jsrc == 0){
                    /* append a result line to the rendered body */
                    scat(g_body, "\n== script result ==\n", sizeof(g_body));
                    scat(g_body, js, sizeof(g_body));
                    scat(g_body, "\n  -> ", sizeof(g_body));
                    snum(g_body, res, sizeof(g_body));
                    scat(g_body, "\n", sizeof(g_body));
                    OutputDebugStringA("[ntbrowser] script ran -> ");
                    { char t[16]; t[0]=0; snum(t,res,sizeof(t));
                      OutputDebugStringA(t); OutputDebugStringA("\n"); }
                    ok = 1;
                } else {
                    scat(g_body, "\n[script error: ", sizeof(g_body));
                    scat(g_body, err[0]?err:"parse", sizeof(g_body));
                    scat(g_body, "]\n", sizeof(g_body));
                }
            }
            break;
        }
    }
    (void)ok;
}

/* Remote: fetch `url` over real HTTP and switch the content area to it.  The
   address bar always echoes what was asked.  Returns 1 on success. */
static int go_remote(const char *url)
{
    int n;
    if (!http_bind()) {
        scpy(g_title, "No network", sizeof(g_title));
        scpy(g_body, "MiniHttpGet is unavailable in this kernel build. "
                     "No network stack to fetch from.", sizeof(g_body));
        set_status("Offline (no MiniHttpGet)", 0);
        return 0;
    }
    set_status("Fetching ", url);
    n = p_http_get(url, g_body, sizeof(g_body));
    if (n < 0) {
        scpy(g_title, "No route", sizeof(g_title));
        scpy(g_body, "MiniHttpGet failed to fetch that address.\n\n"
             "Reminder: NexOS has no TLS, so https:// (Bing, Google) cannot "
             "be fetched.  Try a plain http:// URL.", sizeof(g_body));
        set_status("Fetch failed", 0);
        OutputDebugStringA("[ntbrowser] fetch FAILED for ");
        OutputDebugStringA(url);
        OutputDebugStringA("\n");
        return 0;
    }
    /* The kernel's httpc strips headers; body may still be wrapped HTML.
       We render it as plain text, which is honest for a text-only browser. */
    scpy(g_title, url, sizeof(g_title));
    g_loaded = n;
    g_hist = g_mode;
    g_mode = MODE_DOC;
    eval_scripts();                        /* run any <script> block      */
    scpy(g_addr, url, sizeof(g_addr));
    g_status[0] = 0;
    scat(g_status, "Loaded ", sizeof(g_status));
    snum(g_status, g_loaded, sizeof(g_status));
    scat(g_status, " bytes (http)", sizeof(g_status));
    OutputDebugStringA("[ntbrowser] fetched ");
    OutputDebugStringA(g_status);
    OutputDebugStringA("\n");
    return 1;
}

/* Local start page (shown when the browser opens and on Home). */
static void go_start(void)
{
    g_hist = g_mode;
    g_mode = MODE_START;
    scpy(g_addr, "http://about:start", sizeof(g_addr));
    scpy(g_title, "NT Browser - start", sizeof(g_title));
    scpy(g_body,
         "NT Browser (NexOS / win32 PE)\n"
         "==================================\n\n"
         "This browser fetches pages over real HTTP through the kernel's\n"
         "httpc client, via the NexOS.dll 'MiniHttpGet' bridge.\n\n"
         "Type an http:// address in the bar and press Enter or click Go.\n\n"
         "Try:\n"
         "  http://10.0.2.2:8137/    (host test server serving /)\n"
         "  http://example.com       (real internet, text body)\n"
         "  http://neverssl.com\n\n"
         "https:// sites cannot load here yet - NexOS has no TLS.\n",
         sizeof(g_body));
    set_status("Start Page (local)", 0);
}

/* Navigate to whatever was typed: http:// -> real fetch; anything else ->
   local start page with guidance.  Returns 1 if a fetch was attempted. */
static int navigate(const char *url)
{
    if (!url || !url[0]) { go_start(); return 0; }
    if (slen(url) >= 7 &&
        (unsigned char)url[0]=='h' && (unsigned char)url[1]=='t' &&
        (unsigned char)url[2]=='t' && (unsigned char)url[3]=='p' &&
        (unsigned char)url[4]==':' && (unsigned char)url[5]=='/' &&
        (unsigned char)url[6]=='/')
    {
        return go_remote(url);          /* real network fetch */
    }
    /* Not an absolute http URL: show the start/help page. */
    g_addr[0] = 0; scat(g_addr, url, sizeof(g_addr));
    g_hist = g_mode; g_mode = MODE_START;
    scpy(g_title, "NT Browser - help", sizeof(g_title));
    scpy(g_body,
         "That address isn't an absolute http:// URL, so there is nothing\n"
         "to fetch.\n\n"
         "Type e.g.  http://10.0.2.2:8137/  and press Enter.\n",
         sizeof(g_body));
    set_status("Not an http URL", 0);
    return 0;
}

/* ---- rendering -------------------------------------------------------------- */
static void paint(HDC dc, int w, int h)
{
    int y;
    g_cmds = 0;

    /* background */
    fill(dc, 0, 0, w, h, C_FACE);

    /* toolbar */
    badge(dc, 8, 8, 18, C_NTBRO);
    text(dc, 8 + 22, 10, "NT", C_SURFACE);

    /* address field + Go */
    field(dc, 150, 8, w - 150 - 62, 24, g_focus);
    field_text(dc, 152, 12, w - 150 - 62 - 8, g_addr, g_focus, "http://  ");

    /* Go button */
    fill(dc, w - 60, 8, 56, 24, C_GO);
    text(dc, w - 60 + 14, 13, "Go", C_SURFACE);

    /* hairline under toolbar */
    for (y = 36; y <= 36; y++) fill(dc, 0, y, w, 1, C_HAIRLINE);

    /* content panel */
    fill(dc, 0, 40, w, h - 40 - 24, C_SURFACE);

    if (g_mode == MODE_DOC) {
        /* title line, then wrapped body */
        text(dc, 8, 46, g_title, C_LINK);
        wrap_draw(dc, 8, 68, w - 16, 18, (h - 40 - 24 - 68) / 18 + 2, g_body);
    } else {
        wrap_draw(dc, 8, 46, w - 16, 18, (h - 40 - 24 - 46) / 18 + 3, g_body);
    }

    /* status bar */
    fill(dc, 0, h - 24, w, 24, C_NTBRO);
    text(dc, 6, h - 19, g_status, C_SURFACE);
}

/* ---- input ------------------------------------------------------------------ */
static int hit(int x, int y, int a, int b, int c, int d)
{ return x >= a && x < a + c && y >= b && y < b + d; }

static void on_click(int x, int y)
{
    int w = g_w;
    if (hit(x, y, 150, 8, w - 150 - 62, 24)) { g_focus = 1; g_addr[0] = 0; return; }
    if (hit(x, y, w - 60, 8, 56, 24))       { g_focus = 0; navigate(g_addr); return; }
    g_focus = 0;                    /* click away leaves the address field */
}

static void on_char(int ch)
{
    /* The kernel synthesises WM_CHAR for text (post-TranslateMessage), so a
       plain ASCII char arrives as itself; Backspace and Enter arrive as the
       control codes after TranslateMessage (Backspace=8, Enter=13). */
    if (ch == 13 || ch == 10) { g_focus = 0; navigate(g_addr); return; }
    if (ch == 8 || ch == 127) { spop(g_addr); return; }        /* backspace */
    if (ch >= 0x20 && ch <= 0x7E) { sputc(g_addr, (char)ch, sizeof(g_addr)); return; }
}

/* ---- window procedure -------------------------------------------------------- */
static int WINAPI WndProc(HWND h, u32 msg, u32 wp, u32 lp)
{
    switch (msg)
    {
    case WM_PAINT: {
        PAINTSTRUCT_ ps; HDC dc = BeginPaint(h, &ps);
        RECT_ rc; GetClientRect(h, &rc);
        g_w = rc.right; g_h = rc.bottom;
        paint(dc, g_w, g_h);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        on_click(LOWORD_(lp), HIWORD_(lp));
        return 0;
    case WM_KEYDOWN:
        /* Backspace arrives in WM_CHAR (TranslateMessage synthesises it),
           so ignore it here to avoid double-deleting. */
        if (wp == (u32)VK_ESCAPE) { g_focus = 0; return 0; }
        return 0;
    case WM_CHAR:
        on_char((int)wp);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(h, msg, wp, lp);
    }
}

/* Pull an explicit "http://..." start URL out of the loader's command line
   (GetCommandLineA -> "ntbrowser.exe http://host:port/...").  If present, the
   browser auto-navigates to it on startup -- lets `winapp ntbrowser.exe <url>`
   drive a real fetch headlessly.  Returns 1 when a URL was consumed. */
static int cmdline_start_url(void)
{
    const char *cl = GetCommandLineA();
    if (!cl) return 0;
    while (*cl && *cl != ' ') cl++;      /* skip the program name        */
    while (*cl == ' ') cl++;
    if (!*cl) return 0;
    if (slen(cl) < 7 ||
        !((unsigned char)cl[0]=='h' && (unsigned char)cl[1]=='t' &&
          (unsigned char)cl[2]=='t' && (unsigned char)cl[3]=='p' &&
          (unsigned char)cl[4]==':' && (unsigned char)cl[5]=='/' &&
          (unsigned char)cl[6]=='/'))
        return 0;
    navigate(cl);
    return 1;
}

/* ---- built-in mini-JS self-test ------------------------------------
 *  Runs a fixed integer-JS program on startup (independent of the HTTP path,
 *  which is exercised separately via eval_scripts).  Proves the minijs
 *  interpreter is linked into the PE browser and executes on the target.
 *  20 + 22 -> minijs evaluates to 42.                                          */
static void js_selftest(void)
{
    static const char script[] =
        "var a = 20; var b = 22; var total = a + b; total";
    int res = -1; char err[64]; err[0] = 0;
    if (minijs_run(script, &res, err, (int)sizeof(err)) == 0){
        OutputDebugStringA("[ntbrowser] js-selftest -> ");
        { char t[16]; t[0]=0; snum(t,res,sizeof(t)); OutputDebugStringA(t); }
        OutputDebugStringA(" (expect 42)\n");
    } else {
        OutputDebugStringA("[ntbrowser] js-selftest ERROR: ");
        OutputDebugStringA(err[0] ? err : "parse");
        OutputDebugStringA("\n");
    }
}

/* ---- entry point ------------------------------------------------------------ */
int PeMain(void)
{
    WNDCLASS_ wc; HWND hwnd; MSG_ m;

    wc.style = 0;
    wc.proc  = WndProc;
    wc.cbCls = 0; wc.cbWnd = 0; wc.hInst = 0;
    wc.hIcon = 0; wc.hCursor = 0; wc.hbrBackground = 4;
    wc.menu = NULL_; wc.name = "NTBrowser";

    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "NTBrowser", "NT Browser",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           240, 60, 860, 560, 0, 0, 0, NULL_);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    OutputDebugStringA("[ntbrowser] window created\n");
    js_selftest();            /* proves minijs runs in the PE browser */

    /* A start URL argument (winapp ntbrowser.exe http://...) triggers a real
       fetch straight away; otherwise show the local start page. */
    if (!cmdline_start_url()) go_start();

    while (GetMessageA(&m, 0, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    return (int)m.wParam;
}
