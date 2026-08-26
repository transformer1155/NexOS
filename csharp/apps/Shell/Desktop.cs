// =====================================================================
//  Desktop.cs  -  the Windows 11 shell surface, written in C#
// ---------------------------------------------------------------------
//  Everything the user sees before a window opens lives here: the Bloom
//  wallpaper, the desktop icon grid, the centred taskbar and the Start
//  menu.  gui.cpp keeps window chrome, dragging and input routing; the
//  shell surface itself is entirely managed code.
//
//  Paint order the host drives each frame:
//      Shell.PaintDesktop(w,h)   wallpaper + icons     (behind windows)
//      <native window chrome>
//      Shell.PaintOverlay(w,h)   taskbar + Start menu  (above windows)
//
//  Shell.DesktopClick returns:
//      >= 0   a Kind the host should launch (or focus, if already open)
//      -1     consumed by the shell (Start toggled, power pressed, ...)
//      -2     nothing here - the host should hit-test its own windows
//
//  Interpreter rules obeyed (see Forms.cs): no static initialisers, no
//  floats, arrays are 4-byte slots, Paint() may allocate freely because
//  the heap is rewound after every frame.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public static class Desktop
    {
        // Height of the taskbar strip.  gui.cpp mirrors this constant
        // (MANAGED_TASKBAR_H) so a click on the bar is never swallowed
        // by a window that happens to reach the bottom of the screen.
        public const int TaskH = 48;

        const int Cell   = 92;    // desktop icon cell (square)
        const int IcoSz  = 44;    // desktop icon glyph
        const int Margin = 22;    // desktop grid inset
        const int BtnSz  = 40;    // taskbar button
        const int BtnGap = 6;

        // ---- palette (theme-aware; assigned by ShellTheme each frame) ----
        static uint WallTop  = 0x218FD9;   // Win11 blue wallpaper gradient top
        static uint WallBot  = 0x05216B;   // Win11 deep-blue wallpaper base
        static uint Glow0    = 0x16314E;   // bloom rim
        static uint Glow1    = 0x2C86D6;   // bloom mid
        static uint GlowHi   = 0xBFE4FF;   // bloom core
        static uint IconHot  = 0x1D5A96;   // desktop icon hover
        static uint Bar      = 0x2B2B2B;   // taskbar fill (dark theme)
        static uint BarLine  = 0x3A3A3A;
        static uint BarHot   = 0x3F3F3F;
        static uint Ink      = 0xF3F3F3;
        static uint TaskBarBg    = 0xD21A1A1C;   // rgba(26,26,28,0.82) Win11 dark acrylic taskbar
        static uint TaskBarInk   = 0xFFE8EBF2;   // clock text on the bar
        static uint TaskBarLine  = 0xFF1A1A1C;   // top hairline hidden (matches bg)
        static uint MenuBg   = 0xF21C1C1F;   // start menu rgba(28,28,31,0.95)
        static uint MenuFoot = 0x2A2A2A;
        static uint FieldBg  = 0x2A2A2A;
        static uint FieldEdge= 0x3A3A3A;
        static uint Ghost    = 0x9AA0A8;
        static uint DeskInk  = 0xFFFFFFFF; // desktop icon label ink
        static uint DeskHalo = 0x10243C;   // desktop icon label halo
        static uint Accent   = 0x0078D4;

        // Recompute the shell palette from Theme.Dark so the wallpaper glow,
        // taskbar, Start menu, desktop-icon labels and right-click menus switch
        // together.  Called at the top of every Paint / PaintOverlay frame.
        static void ShellTheme()
        {
            if (Theme.Dark != 0) {
                WallTop = 0x218FD9; WallBot = 0x05216B; Glow0 = 0x145DA8; Glow1 = 0x2C86D6; GlowHi = 0x8CD1FF;
                IconHot = 0x1D5A96;
                Bar = 0x2B2B2B; BarLine = 0x3A3A3A; BarHot = 0x3F3F3F; Ink = 0xF3F3F3;
                TaskBarBg = 0xFF202020; TaskBarInk = 0xFFE8EBF2; TaskBarLine = 0xFF3A3F4B;
                MenuBg = 0x202020; MenuFoot = 0x2A2A2A; FieldBg = 0x2A2A2A; FieldEdge = 0x3A3A3A; Ghost = 0x9AA0A8;
                DeskInk = 0xFFFFFFFF; DeskHalo = 0x10243C;
            } else {
                WallTop = 0x5B86C4; WallBot = 0xCFE3FF; Glow0 = 0x9DC3F0; Glow1 = 0x2C86D6; GlowHi = 0xFFF4D6;
                IconHot = 0x2C6FB0;
                Bar = 0xF3F3F3; BarLine = 0xD5DDE8; BarHot = 0xE1EAF6; Ink = 0x1B1B1B;
                TaskBarBg = 0xFFF3F3F3; TaskBarInk = 0xFF1B1B1B; TaskBarLine = 0xFFD5DDE8;
                MenuBg = 0xF7F9FC; MenuFoot = 0xEBF0F7; FieldBg = 0xFFFFFF; FieldEdge = 0xCCD4E0; Ghost = 0x8A93A0;
                DeskInk = 0x1B1B1B; DeskHalo = 0xFFFFFFFF;
            }
            Accent = Theme.Accent;
        }

        // ---- desktop icons (backed by the "Desktop" folder in MKFS) --
        // dKind/dName/dCol/dLet are rebuilt from fs==3 (the Desktop
        // directory) by SyncFromFs(); each entry is a .lnk shortcut file
        // whose body is the managed Kind it launches.
        static int[]    dKind;
        static string[] dName;
        static int[]    dCol;
        static int[]    dLet;
        static int      dN;

        // ---- taskbar pins --------------------------------------------
        static int[]    tKind;
        static int[]    tCol;
        static int[]    tLet;
        static int      tN;

        // ---- installed-app registry ----------------------------------
        // A kind is "installed" when it shows up in the Start menu / taskbar
        // / desktop.  Uninstalling removes it from all of those and closes
        // any open window of that kind; the .mex itself stays on the SFS
        // (the disk is read-only) so it can always be re-installed.  The
        // truth lives in a MKFS file ("installed.cfg") so the Control Panel
        // "Apps" page and the shell share one source of state across boots.
        static bool[]   Installed;        // index == Kind value (0..11)
        const  int     KINDS = 12;
        const  int     CFG_FS = 3;        // same volume the Desktop folder uses
        const  string  CFG_NAME = "installed.cfg";

        static bool menuOpen;
        static int  CurrentDesktop = 0;   // 0 = default desktop (icons/right-click)

        // ---- Start menu state -----------------------------------------
        static int startView = 0;         // 0 = Pinned, 1 = All apps
        static int startMenuX, startMenuY, startMenuW, startMenuH;
        static int startRClickIdx = -1;   // tile / all-apps row under right-click
        const int START_KEY = 0x51ED270B; // stable Anim key for Start-menu open/close

        // ---- AI virtual-desktop chat (feature B) ----
        // No static initialisers under MiniCLR, so the buffers are allocated
        // in Desktop.Init(); AiEnsure() also lazily guards every entry point.
        static string[] aiHist = null;
        static int       aiHistN = 0;
        // User input is stored as Unicode codepoints.  MiniCLR strings are
        // UTF-8 byte arrays, so editing byte-by-byte would split CJK chars.
        static int[]     aiCode = null;
        static int       aiCodeN = 0;
        static int       aiFocus = 0;
        static int       aiReady = 0;
        // Typewriter reveal of the latest AI reply.  aiTypePos is a BYTE
        // offset into aiTypeFull (MiniCLR strings are UTF-8), advanced a few
        // codepoints per frame via U8Next so a CJK char is never split
        // mid-sequence.  Drawn with U.Sub(aiTypeFull, 0, aiTypePos).
        static string    aiTypeFull = null;
        static int       aiTypePos = 0;     // revealed byte offset
        static int       aiTypeActive = 0;  // 1 while a reply is typing out
        static int       aiTypeFrames = 0;  // frames since typewriter started
        // "Thinking" state: while 1, the agent run is deferred to the next
        // paint frame so the "思考中…" line can render first.  aiPendingGoal
        // holds the goal to run once the deferred call fires.
        static int       aiThinking = 0;
        static string    aiPendingGoal = null;
        // Input length cap.  AiCodeToString rebuilds the box text once per
        // frame with a per-codepoint concat, which costs O(n^2) bytes on the
        // CLR bump heap -- 96 CJK codepoints is ~41 KB/frame, which the
        // 512 KB heap absorbs; 200 was ~180 KB/frame and could fault.
        // It is also wider than the input box can display.
        const int AiCodeMax = 96;
        // Typewriter cadence: reveal AI_TYPE_STEP_CP codepoints every
        // AI_TYPE_STEP_FRAMES paint frames.  Frame-based timing is immune to
        // the CPU-frequency-sensitive Host.Ticks() (rdtsc >> 10) and gives a
        // consistent visual speed at the GUI's render rate.  At 60 FPS,
        // 2 codepoints every 2 frames is ~60 codepoints/sec, so a 480-byte
        // trimmed reply types out in roughly 8 seconds.
        const int AI_TYPE_STEP_CP = 2;
        const int AI_TYPE_STEP_FRAMES = 2;

        static void AiEnsure() { if (aiHist == null) AiInit(); }
        static void AiInit() {
            aiHist   = new string[40];
            aiHistN  = 0;
            aiCode   = new int[200];
            aiCodeN  = 0;
            aiFocus  = 0;
            aiReady  = 0;
        }
        static void AiHistAdd(string s) {
            if (s == null) s = "";
            if (aiHistN < 40) { aiHist[aiHistN] = s; aiHistN++; }
            else { for (int i = 0; i < 39; i++) aiHist[i] = aiHist[i + 1]; aiHist[39] = s; }
        }
        // Advance the typewriter based on paint frames.  Called once per
        // frame from AiDesktopPaint.  Each step pushes the byte cursor
        // aiTypePos forward by AI_TYPE_STEP_CP codepoints (via U8Next, so the
        // cut point is always a codepoint boundary -- never inside a CJK
        // byte sequence).  aiTypeFull.Length is a codepoint count on this
        // runtime, so we compare the byte cursor against Sys.StrLen (bytes).
        static void AiTypeTick() {
            if (aiTypeActive == 0) return;
            if (aiTypeFull == null) { aiTypeActive = 0; return; }
            aiTypeFrames++;
            if ((aiTypeFrames % AI_TYPE_STEP_FRAMES) != 0) return;
            int fullBytes = NexOS.Sys.StrLen(aiTypeFull);
            for (int j = 0; j < AI_TYPE_STEP_CP && aiTypePos < fullBytes; j++) {
                aiTypePos = U8Next(aiTypeFull, aiTypePos);
            }
            if (aiTypePos >= fullBytes) {
                aiTypeActive = 0;
                Host.SetAnim(0);     // reveal finished, stop requesting repaints
            }
        }
        static int AiPw(int w){ return (w * 64) / 100; }
        static int AiPx(int w){ return (w - AiPw(w)) / 2; }
        static int AiPy(int h){ return (h * 18) / 100; }
        static int AiPh(int h){ return (h * 64) / 100; }
        static int AiInX(int w){ return AiPx(w) + 16; }
        static int AiInY(int h){ int py = AiPy(h), ph = AiPh(h); return py + ph - 52; }
        static int AiInW(int w){ return AiPw(w) - 32 - 110; }
        static int AiInH(){ return 38; }
        static int AiBtnX(int w){ return AiPx(w) + 16 + (AiPw(w) - 32 - 110) + 8; }
        static int AiBtnY(int h){ return AiInY(h); }
        static int AiBtnW(){ return 94; }
        static int AiBtnH(){ return 38; }

        // ---- UTF-8 helpers for CJK text entry/rendering ----------------
        // Decode one UTF-8 codepoint starting at i.  Returns the index just
        // after it and stores the decoded codepoint in g_u8cp.
        //
        // IMPORTANT: the MiniCLR interpreter has NO `out`/`ref` support --
        // opcode 0x12 (ldloca.s, the address of a local passed by ref) is
        // unimplemented and faults the whole managed shell.  So we return the
        // codepoint through a static scratch field instead of an `out` arg.
        static int g_u8cp;
        static int U8Next(string s, int i) {
            int n = s.Length;
            if (i >= n) { g_u8cp = 0; return n; }
            int b0 = (int)s[i];
            if (b0 < 0x80) { g_u8cp = b0; return i + 1; }
            if ((b0 & 0xE0) == 0xC0 && i + 1 < n) {
                g_u8cp = ((b0 & 0x1F) << 6) | ((int)s[i + 1] & 0x3F);
                return i + 2;
            }
            if ((b0 & 0xF0) == 0xE0 && i + 2 < n) {
                g_u8cp = ((b0 & 0x0F) << 12) | (((int)s[i + 1] & 0x3F) << 6) | ((int)s[i + 2] & 0x3F);
                return i + 3;
            }
            if ((b0 & 0xF8) == 0xF0 && i + 3 < n) {
                g_u8cp = ((b0 & 0x07) << 18) | (((int)s[i + 1] & 0x3F) << 12) | (((int)s[i + 2] & 0x3F) << 6) | ((int)s[i + 3] & 0x3F);
                return i + 4;
            }
            g_u8cp = b0; return i + 1;
        }
        static int CpWidth(int cp) { return (cp >= 0x80) ? 16 : 8; }

        // NOTE: still a per-codepoint concat (the codepoints live in an int[],
        // and array-typed internal calls are not supported by the MEX
        // compiler).  It runs once per frame from AiDesktopPaint, so the
        // O(n^2) byte cost is bounded by AiCodeMax below -- keep that cap low.
        static string AiCodeToString(int start, int count) {
            string r = "";
            int lim = start + count;
            if (lim > aiCodeN) lim = aiCodeN;
            for (int i = start; i < lim; i++) r = U.Cat(r, Host.CharStr(aiCode[i]));
            return r;
        }

        // Number of 18px lines a message needs when wrapped to maxw (capped at 6).
        static int AiMsgLines(int maxw, string s) {
            if (s == null) return 0;
            int i = 0, line = 0, x = 0;
            while (i < s.Length && line < 6) {
                int next = U8Next(s, i);
                int w = CpWidth(g_u8cp);
                if (x + w > maxw && x > 0) { line++; if (line >= 6) break; x = 0; }
                x += w;
                i = next;
            }
            return (x > 0 && line < 6) ? (line + 1) : line;
        }

        // Draw one message wrapped to maxw, top-left at (x,y).  Capped at 6 lines.
        //
        // Each visual line is sliced out with a single U.Sub instead of being
        // accumulated one codepoint at a time with U.Cat.  The old version
        // allocated ~n^2/2 bytes per line and ran for every visible message
        // EVERY FRAME: with a full chat history that was >400 KB of the
        // 512 KB managed heap per frame, so the paint faulted and the shell
        // fell back to the plain native desktop.
        static void AiDrawMsgAt(int x, int y, int maxw, string s, uint c) {
            if (s == null) return;
            int i = 0, line = 0, xoff = 0, start = 0;
            while (i < s.Length && line < 6) {
                int next = U8Next(s, i);
                int w = CpWidth(g_u8cp);
                if (xoff + w > maxw && xoff > 0) {
                    Gfx.Text(x, y + line * 18, U.Sub(s, start, i - start), c);
                    start = i; xoff = 0; line++;
                    if (line >= 6) break;
                }
                xoff += w; i = next;
            }
            if (xoff > 0 && line < 6)
                Gfx.Text(x, y + line * 18, U.Sub(s, start, i - start), c);
        }

        static void AiSend() {
            AiEnsure();
            Host.Log(U.Cat("[AIDESK] AiSend called codeN=", U.I(aiCodeN)));
            if (aiCodeN == 0) return;
            string goal = AiCodeToString(0, aiCodeN);
            AiHistAdd(U.Cat("You: ", goal));
            aiCodeN = 0;
            aiFocus = 0;
            // Defer the (blocking) agent run to the next paint frame and show
            // a "思考中…" placeholder first, so the user gets feedback instead
            // of a frozen/blank screen.  AiRunPending() performs the actual
            // Host.Exec and starts the typewriter once it returns.
            AiHistAdd("AI: 思考中…");
            aiPendingGoal = goal;
            aiThinking = 1;
            // Ask the host to keep repainting while the thinking dots / the
            // typewriter are running -- the GUI loop only repaints on input
            // events, so without this the screen would freeze on the
            // placeholder and the reveal would never progress.
            Host.SetAnim(1);
        }

        // Performs the deferred agent call.  Called ONCE from AiDesktopPaint
        // -- at the END, AFTER the "思考中…" line has already been painted --
        // so the indicator is visible during the (blocking) agent run.  The
        // GUI freezes for the run's duration, but the last painted frame keeps
        // showing "思考中…" until the typewriter kicks in on the next frame.
        static void AiRunPending() {
            string goal = aiPendingGoal;
            aiPendingGoal = null;
            aiThinking = 0;            // clear first so a fault cannot retry-loop
            if (goal == null) return;
            Host.Log(U.Cat("[AIDESK] run: ", goal, "\n"));
            if (aiReady == 0) { Host.Exec("agent init"); aiReady = 1; }
            string res = Host.Exec(U.Cat("agent run ", goal));
            if (res == null) res = "";
            Host.Log(U.Cat("[AIDESK] raw len=", U.I(NexOS.Sys.StrLen(res)), " head=\"", U.Sub(res, 0, 32), "\""));
            // Trim FIRST, then flatten -- three allocations total.
            //
            // This used to be two per-character `U.Cat` loops.  On the CLR's
            // bump heap (512 KB, no GC) that costs ~n^2/2 BYTES: a 2 KB
            // `agent run` transcript allocated ~2 MB, exhausted the heap and
            // faulted mid-handler.  The fault retired the whole managed
            // shell, so the screen lost all managed text, the taskbar
            // disappeared and Ctrl+Left/Right desktop switching died.
            // 480 bytes is also all AiDrawMsgAt can ever show (6 lines).
            if (res.Length > 480) {
                int cut = 480;
                // never split a UTF-8 sequence
                while (cut > 0 && ((int)res[cut] & 0xC0) == 0x80) cut--;
                res = U.Cat(U.Sub(res, 0, cut), "...");
            }
            res = U.Flat(res);                  // CR/LF/TAB -> space
            Host.Log(U.Cat("[AIDESK] flat len=", U.I(NexOS.Sys.StrLen(res)), " head=\"", U.Sub(res, 0, 32), "\""));
            if (res.Length == 0) res = "(no output)";
            string reply = U.Cat("AI: ", res);
            Host.Log(U.Cat("[AIDESK] reply bytes=", U.I(NexOS.Sys.StrLen(reply)), " head=\"", U.Sub(reply, 0, 32), "\""));
            // Replace the "思考中…" placeholder with the real reply.
            if (aiHistN > 0) aiHist[aiHistN - 1] = reply;
            else AiHistAdd(reply);
            // Begin the typewriter reveal of this reply instead of dumping
            // the whole thing at once.
            aiTypeFull = reply;
            aiTypePos = 0;
            aiTypeFrames = 0;
            aiTypeActive = 1;
        }

        static void AiDesktopKey(int ch) {
            AiEnsure();
            Host.Log(U.Cat("[AIDESK] AiDesktopKey ch=", U.I(ch), " focus=", U.I(aiFocus)));
            if (ch == 8 || ch == -1) {                          // Backspace
                if (aiFocus == 1 && aiCodeN > 0) aiCodeN--;
                return;
            }
            if (ch == 10 || ch == 13 || ch == -2) { if (aiFocus == 1) AiSend(); return; }  // Enter
            if (ch == 27) { aiFocus = 0; return; }              // Esc
            if (ch >= 0x20 && ch != 0x7F && ch < 0x110000) {    // printable Unicode
                if (aiFocus == 0) aiFocus = 1;
                if (aiCodeN < AiCodeMax) aiCode[aiCodeN++] = ch;
                return;
            }
        }

        // ---- system tray cluster (left of the clock) ----------------
        // Buttons: 0 = background tasks, 1 = voice input, 2 = network.
        const int TrayBtn = 34;
        const int TrayGap = 4;
        static int[] trayRect;          // [x, y, btn, gap], laid out per frame
        static int sortMode;            // desktop icon sort (0..3)

        // ---- portal desktop geometry (recomputed each frame) --------
        static int pTileW, pTileH, pTileGap, pCols, pGridX, pGridY;
        static int pSearchX, pSearchY, pSearchW, pSearchH, pTabY, pCardY;
        static int portalTab;            // selected nav tab (Home=0)

        // ---- context-menu action codes (dispatched by HandleAction) --
        public const int A_SORT_NAME  = 1, A_SORT_SIZE = 2, A_SORT_TYPE = 3, A_SORT_DATE = 4;
        public const int A_REFRESH    = 5, A_PERSONALIZE = 6, A_TERMINAL = 7, A_NEWFOLDER = 8;
        public const int A_TASKMGR    = 9, A_TASKBAR = 10;
        public const int A_VOICE      = 11, A_NET_ETH = 12, A_NET_WIFI = 13, A_NET_SETTINGS = 14;
        public const int A_TASKS_BASE = 100;   // + kind for a running app
        public const int A_F_OPEN     = 200, A_F_TERM = 201, A_F_COPY = 202, A_F_DEL = 203;
        public const int A_F_RENAME   = 204, A_F_PROPS = 205, A_F_MKDIR = 206;
        public const int A_F_EDIT     = 207, A_F_OPENWITH = 208;
        // "Open with... > Notepad": forces the text viewer even for a .exe,
        // whose default action is now to RUN it through the PE loader.
        public const int A_F_NOTEPAD  = 209;
        // File Explorer "New file": create an empty text file and drop into
        // the inline rename editor, exactly like "New folder".
        public const int A_F_NEWFILE  = 214;
        // Win11-style file menu (新版): a Cut/Copy/Paste/Rename/Share/Delete
        // cluster and a "Show more options" escape hatch into the classic
        // (Windows-10-style) menu.  Cut == copy-then-delete is overkill for
        // the shell, so Cut simply copies; Paste/Share are no-ops with a log.
        public const int A_F_CUT     = 215, A_F_PASTE = 216, A_F_SHARE = 217;
        public const int A_F_CLASSIC = 218;   // "Show more options" -> classic menu
        // Desktop layout toggle (Simple vs Busy).  The labels in the
        // context menu read the current Theme.DesktopMode, so both codes
        // simply force the requested value.
        public const int A_MODE_SIMPLE = 210, A_MODE_BUSY = 211;
        public const int A_DESK_AI = 212, A_DESK_DEF = 213;  // virtual desktop switch
        // Start menu app/tile actions (right-click on a tile or All-apps row).
        public const int A_START_PIN = 80, A_START_UNPIN = 81, A_START_ALLAPPS = 82;
        public const int A_START_BACK = 83, A_START_SETTINGS = 84;
        // Taskbar window menu (close window / end process).
        public const int A_WIN_CLOSE  = 20, A_WIN_END = 21;
        // Desktop "Open with..." submenu targets (open the app directly).
        public const int A_DESK_OPEN_NOTEPAD = 22, A_DESK_OPEN_TERM = 23;
        // ---- Win11 desktop context-menu expansions (Canvas reference) ----
        // "View >" / "New >" are cascading sentinel codes: clicking them
        // closes the current menu and opens the matching flyout (like the
        // existing A_F_OPENWITH sub-menu).  "More" opens the classic menu.
        public const int A_VIEW       = 30, A_NEW = 31, A_DISPLAY = 32, A_MORE = 33;
        public const int A_VIEW_LRG   = 34, A_VIEW_MED = 35, A_VIEW_SM = 36;
        public const int A_VIEW_ICONS = 37, A_VIEW_AUTO = 38, A_VIEW_GRID = 39;
        public const int A_NEW_FOLDER = 40, A_NEW_SHC = 41;       // desktop "New folder" / "New shortcut"
        // ---- Win+X power-user menu (right-click Start / taskbar) ----
        public const int A_WINX_BASE  = 500;   // + index into the WinX entry table
        // ---- taskbar jump-list actions (File Explorer pin) ----
        public const int A_JUMP_UNPIN   = 60, A_JUMP_NEWWIN = 61, A_JUMP_CLOSEALL = 62;
        public const int A_JUMP_RECENT  = 63;   // + recent index (0..3)
        public const int OWNER_DESKTOP = 0, OWNER_TASKBAR = 1, OWNER_TRAY = 2, OWNER_FILE = 3, OWNER_WIN = 4;
        public const int OWNER_DESKTOP_FILE = 5;   // a right-clicked desktop shortcut
        public const int OWNER_WINX = 6, OWNER_JUMP = 7;   // Win+X menu / taskbar jump list
        public const int OWNER_START = 8;          // Start menu app / tile popup

        static string[] kName;         // canonical name per kind (unsorted)
        static int fileOwner = -1, fileFs = 0, fileSel = -1;
        static int fileMenuX = 0, fileMenuY = 0;   // popup origin for "Open with..." sub-menu
        // Desktop view preferences (toggled via the "View" flyout).
        static int deskViewMode  = 1;   // 0 large / 1 medium / 2 small
        static int deskShowIcons = 1;   // show desktop icons
        static int deskAuto      = 0;   // auto-arrange
        static int deskGrid      = 1;   // align to grid
        static int jumpRecentSel = -1;  // last jump-list recent index (unused placeholder)
        static int winOwner = -1;      // app that opened the generic window menu
        static int taskWinKind = -1;   // kind of the window the taskbar menu targets
        static int jumpKind = -1;      // kind of the window the jump list targets
        static string deskClip;        // last "copied" desktop shortcut name
        static int   renameIdx = -1;  // desktop icon being renamed (-1 = none)
        static string renameBuf;       // current text in the inline editor
        static string renameOld;       // original .lnk name (rename source)
        static string renameUndo;      // single-level undo snapshot of renameBuf
        static uint gCol;              // scratch for KindStyle
        static int  gLet;              // scratch for KindStyle

        // -------------------------------------------------------------
        //  Init.  Static field initialisers never run under MiniCLR, so
        //  every table is built here (Shell.Init calls us once).
        // -------------------------------------------------------------
        public static void Init()
        {
            dKind = new int[16];
            dName = new string[16];
            dCol  = new int[16];
            dLet  = new int[16];
            dN    = 0;

            // Desktop icons are read from the "Desktop" folder on MKFS.
            SyncFromFs();

            tN    = 7;
            tKind = new int[7];
            tCol  = new int[7];
            tLet  = new int[7];
            Pin(0, Kind.FileExplorer, 0xFFC83D, 'P');
            Pin(1, Kind.Terminal,     0x2F3A45, '>');
            Pin(2, Kind.Calculator,   0x00A3A3, '=');
            Pin(3, Kind.TaskManager,  0x0F7B0F, 'T');
            Pin(4, Kind.ControlPanel, 0x0078D4, 'S');
            Pin(5, Kind.About,        0xD8541B, 'i');
            Pin(6, Kind.Browser,      0x1A73E8, 'B');

            trayRect = new int[4];
            sortMode = 0;
            menuOpen = false;
            renameIdx = -1;

            kName = new string[12];
            kName[0] = "Settings";     kName[1] = "This PC";    kName[2] = "Terminal";
            kName[3] = "Calculator";   kName[4] = "Task Mgr";   kName[5] = "Optimizer";
            kName[6] = "Notepad";      kName[7] = "About";      kName[8] = "Browser";
            kName[9] = "AI Setup";     kName[10] = "AI Agent";  kName[11] = "Demo";

            Popup.Init();

            // Rehydrate the installed-app set from the MKFS data disk (falls
            // back to "all installed" when the file is absent on first boot).
            LoadInstalled();

            // AI desktop chat buffers (MiniCLR: no static initialisers).
            aiHist   = new string[40];
            aiHistN  = 0;
            aiCode   = new int[200];
            aiCodeN  = 0;
            aiFocus  = 0;
            aiReady  = 0;
        }

        // Rebuild the desktop icon table from the "Desktop" folder (fs==3).
        // Each .lnk file's body is a single decimal digit: the managed
        // Kind it launches.  Falls back to built-in defaults if the
        // folder is empty (e.g. seeding has not run yet).
        static void SyncFromFs()
        {
            int n = Host.FileCount(3);
            if (n <= 0) { LoadDefaults(); return; }
            if (n > 16) n = 16;
            dN = n;
            for (int i = 0; i < n; i++)
            {
                string nm = Host.FileName(3, i);
                dName[i] = StripLnk(nm);
                int kind = ParseKind(Host.ReadText(3, nm));
                dKind[i] = kind;
                KindStyle(kind);
                dCol[i] = (int)gCol;
                dLet[i] = gLet;
            }
        }

        // Built-in desktop set, used only as a fallback before seeding.
        static void LoadDefaults()
        {
            dN = 12;
            Put(0, Kind.FileExplorer, "This PC",    0xFFC83D, 'P');
            Put(1, Kind.Terminal,     "Terminal",   0x2F3A45, '>');
            Put(2, Kind.Calculator,   "Calculator", 0x00A3A3, '=');
            Put(3, Kind.TaskManager,  "Task Mgr",   0x0F7B0F, 'T');
            Put(4, Kind.ControlPanel, "Settings",   0x0078D4, 'S');
            Put(5, Kind.MemOptimizer, "Optimizer",  0x8256D0, 'M');
            Put(6, Kind.Notepad,      "Notepad",    0x4A6FA5, 'N');
            Put(7, Kind.About,        "About",      0xD8541B, 'i');
            Put(8, Kind.Browser,      "Browser",    0x1A73E8, 'B');
            Put(9, Kind.AiSetup,      "AI Setup",   0x6A3EA1, 'A');
            Put(10, Kind.AiAgent,     "AI Agent",   0x8A5CF6, 'R');
            Put(11, Kind.Demo,        "Demo",       0x8A5CF6, 'D');
        }

        static void Put(int i, int k, string n, int c, int l)
        { dKind[i] = k; dName[i] = n; dCol[i] = c; dLet[i] = l; }

        static void Pin(int i, int k, int c, int l)
        { tKind[i] = k; tCol[i] = c; tLet[i] = l; }

        public static int IsMenuOpen() { return menuOpen ? 1 : 0; }

        // Resolve a managed Kind to its desktop glyph colour + letter.
        static void KindStyle(int kind)
        {
            if (kind == Kind.FileExplorer) { gCol = 0xFFC83D; gLet = 'P'; }
            else if (kind == Kind.Terminal) { gCol = 0x2F3A45; gLet = '>'; }
            else if (kind == Kind.Calculator) { gCol = 0x00A3A3; gLet = '='; }
            else if (kind == Kind.TaskManager) { gCol = 0x0F7B0F; gLet = 'T'; }
            else if (kind == Kind.ControlPanel) { gCol = 0x0078D4; gLet = 'S'; }
            else if (kind == Kind.MemOptimizer) { gCol = 0x8256D0; gLet = 'M'; }
            else if (kind == Kind.Notepad) { gCol = 0x4A6FA5; gLet = 'N'; }
            else if (kind == Kind.About) { gCol = 0xD8541B; gLet = 'i'; }
            else if (kind == Kind.Browser) { gCol = 0x1A73E8; gLet = 'B'; }
            else if (kind == Kind.AiSetup) { gCol = 0x6A3EA1; gLet = 'A'; }
            else if (kind == Kind.AiAgent) { gCol = 0x8A5CF6; gLet = 'R'; }
            else if (kind == Kind.Demo)   { gCol = 0x8A5CF6; gLet = 'D'; }
            else { gCol = 0x888888; gLet = '?'; }
        }

        // Drop a trailing ".lnk" (case-insensitive) from a shortcut name.
        static string StripLnk(string s)
        {
            int n = s.Length;
            if (n > 4)
            {
                char c1 = s[n - 1], c2 = s[n - 2], c3 = s[n - 3], c4 = s[n - 4];
                if (c4 == '.' &&
                    (c3 == 'l' || c3 == 'L') && (c2 == 'n' || c2 == 'N') && (c1 == 'k' || c1 == 'K'))
                {
                    string r = "";
                    for (int i = 0; i < n - 4; i++) r = U.Cat(r, Host.CharStr((int)s[i]));
                    return r;
                }
            }
            return s;
        }

        // Parse the first decimal integer from a string like "4".
        static int ParseKind(string s)
        {
            int n = s.Length, v = 0, started = 0;
            for (int i = 0; i < n; i++)
            {
                int d = (int)s[i] - '0';
                if (d >= 0 && d <= 9) { v = v * 10 + d; started = 1; }
                else if (started != 0) break;
            }
            return v;
        }

        public static string KindName(int k)
        {
            if (k == Kind.FileExplorer) return "This PC";
            if (k == Kind.Terminal) return "Terminal";
            if (k == Kind.Calculator) return "Calculator";
            if (k == Kind.TaskManager) return "Task Manager";
            if (k == Kind.ControlPanel) return "Settings";
            if (k == Kind.MemOptimizer) return "Optimizer";
            if (k == Kind.Notepad) return "Notepad";
            if (k == Kind.About) return "About";
            if (k == Kind.Browser) return "Browser";
            if (k == Kind.AiSetup) return "AI Setup";
            if (k == Kind.AiAgent) return "AI Agent";
            if (k == Kind.Demo)    return "Demo";
            return "Unknown";
        }

        // App metadata used by the Control Panel "Apps" page.  Version and a
        // one-line description per kind; colour/letter reuse KindStyle so the
        // list matches the desktop glyphs.
        public static string AppVersion(int k)
        {
            if (k == Kind.FileExplorer) return "1.0";
            if (k == Kind.Terminal)     return "1.2";
            if (k == Kind.Calculator)   return "1.4";
            if (k == Kind.TaskManager)  return "1.1";
            if (k == Kind.ControlPanel) return "1.0";
            if (k == Kind.MemOptimizer) return "1.0";
            if (k == Kind.Notepad)      return "1.3";
            if (k == Kind.About)        return "1.0";
            if (k == Kind.Browser)     return "1.0";
            if (k == Kind.AiSetup)     return "1.0";
            if (k == Kind.AiAgent)     return "1.0";
            if (k == Kind.Demo)        return "1.0";
            return "0.0";
        }

        public static string AppDesc(int k)
        {
            if (k == Kind.FileExplorer) return "Browse files on the disk";
            if (k == Kind.Terminal)     return "Command shell";
            if (k == Kind.Calculator)   return "Arithmetic calculator";
            if (k == Kind.TaskManager)  return "Processes & memory";
            if (k == Kind.ControlPanel) return "System settings";
            if (k == Kind.MemOptimizer) return "Tune memory usage";
            if (k == Kind.Notepad)      return "Plain-text editor";
            if (k == Kind.About)        return "System information";
            if (k == Kind.Browser)      return "Web browser";
            if (k == Kind.AiSetup)      return "Configure the AI engine";
            if (k == Kind.AiAgent)      return "Chat with the AI agent";
            if (k == Kind.Demo)         return "Demo application";
            return "";
        }

        // ---- installed-app persistence (MKFS "installed.cfg") ----------
        // Layout: KINDS bytes, each '1' (installed) or '0' (uninstalled).
        public static int IsInstalled(int k)
        {
            if (k < 0 || k >= KINDS) return 1;
            return Installed[k] ? 1 : 0;
        }

        public static void SetInstalled(int k, int v)
        {
            if (k < 0 || k >= KINDS) return;
            if (Installed[k] == (v != 0)) return;     // no change
            Installed[k] = (v != 0);
            SaveInstalled();
            if (v == 0) {
                // Uninstall: drop from taskbar and close any open windows.
                for (int i = 0; i < tN; i++)
                    if (tKind[i] == k) { tKind[i] = -1; tCol[i] = 0; tLet[i] = 0; }
                Shell.CloseKind(k);
            } else {
                // Install: re-pin to the taskbar if there is room.
                int have = 0;
                for (int i = 0; i < tN; i++) if (tKind[i] == k) have = 1;
                if (have == 0 && tN < 7)
                {
                    KindStyle(k);
                    Pin(tN, k, (int)gCol, gLet);
                    tN++;
                }
            }
        }

        static void LoadInstalled()
        {
            Installed = new bool[KINDS];
            for (int i = 0; i < KINDS; i++) Installed[i] = true;   // default: all present
            string s = Host.ReadText(CFG_FS, CFG_NAME);
            if (s == null) return;
            int n = s.Length;
            for (int i = 0; i < KINDS && i < n; i++)
                Installed[i] = ((int)s[i] == '1');
        }

        static void SaveInstalled()
        {
            string s = "";
            for (int i = 0; i < KINDS; i++) s = U.Cat(s, Installed[i] ? "1" : "0");
            Host.WriteText(CFG_FS, CFG_NAME, s);
        }

        // Hit-test a desktop icon; returns its index or -1.
        // The portal lays tiles out in a centered grid, so this must use
        // the same geometry PortalGeom computes for painting.
        public static int IconAt(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();
            if (CurrentDesktop == 1) return -1;   // AI desktop has no icons
            if (Theme.DesktopMode == 0)
            {
                int perCol = PerCol(h);
                for (int i = 0; i < dN; i++)
                {
                    int r = ClassicRect(i, w, h);
                    int x = (r >> 16) & 0xFFFF;
                    int y = r & 0xFFFF;
                    if (U.In(mx, my, x, y, Cell, Cell)) return i;
                }
                return -1;
            }
            PortalGeom(w, h);
            for (int i = 0; i < dN; i++)
            {
                int col = i % pCols, row = i / pCols;
                int x = pGridX + col * (pTileW + pTileGap);
                int y = pGridY + row * (pTileH + pTileGap);
                if (U.In(mx, my, x, y, pTileW, pTileH)) return i;
            }
            return -1;
        }

        // Public accessor for the desktop icon's file name (WinHost use).
        public static string DesktopIconName(int i)
        {
            if (i < 0 || i >= dN) return "";
            return dName[i];
        }

        // Public accessor for the managed Kind a desktop icon launches.
        public static int DesktopKind(int i)
        {
            if (i < 0 || i >= dN) return -1;
            return dKind[i];
        }

        // ---- desktop arrangement / theme hooks (driven by the host) ---
        public static void SortBy(int mode)
        {
            if (mode < 0 || mode > 3) return;
            sortMode = mode;
            for (int i = 0; i < dN; i++)
                for (int j = i + 1; j < dN; j++)
                    if (StrCmp(dName[i], dName[j]) > 0)
                    {
                        int tk = dKind[i]; dKind[i] = dKind[j]; dKind[j] = tk;
                        string tn = dName[i]; dName[i] = dName[j]; dName[j] = tn;
                        int tc = dCol[i]; dCol[i] = dCol[j]; dCol[j] = tc;
                        int tl = dLet[i]; dLet[i] = dLet[j]; dLet[j] = tl;
                    }
        }

        public static int GetSortMode() { return sortMode; }

        // Re-read the Desktop folder (also invalidates the host cache).
        public static void Refresh() { Host.FileRefresh(); SyncFromFs(); }

        public static void SetVoice(int v) { Theme.VoiceOn = v != 0 ? 1 : 0; }
        public static int  GetVoice()      { return Theme.VoiceOn; }

        // MiniCLR-safe lexical compare (no string.Compare available).
        static int StrCmp(string a, string b)
        {
            int n = a.Length, m = b.Length;
            int lim = n < m ? n : m;
            for (int i = 0; i < lim; i++)
            {
                int ca = (int)a[i], cb = (int)b[i];
                if (ca != cb) return ca < cb ? -1 : 1;
            }
            if (n < m) return -1;
            if (n > m) return 1;
            return 0;
        }

        // =============================================================
        //  Layer 1 - wallpaper + icons (painted behind every window)
        // =============================================================
        public static void Paint(int w, int h)
        {
            ShellTheme();
            if (CurrentDesktop == 1) { AiDesktopPaint(w, h); return; }
            Wallpaper(w, h);
            if (Theme.DesktopMode == 0) ClassicIcons(w, h);
            else                         PortalSurface(w, h);
        }

        // Win11 "Bloom": a deep blue gradient with a radial glow.
        static void Wallpaper(int w, int h)
        {
            // Detailed wallpaper image when the SFS texture pack is present;
            // otherwise fall back to the gradient + bloom (flat look).
            if (Gfx.HasImage(Tex.Wall) != 0)
            {
                Gfx.Image(Tex.Wall, 0, 0, w, h);
                return;
            }

            // Win11 blue bloom: three-stop vertical gradient (#218FD9 -> #0D57B3
            // @55% -> #05216B) plus a centred radial glow.  Matches win11-ui.
            int midY = (h * 55) / 100;
            Gfx.Gradient(0, 0,      w, midY + 1, 0x218FD9, 0x0D57B3);
            Gfx.Gradient(0, midY,   w, h - midY, 0x0D57B3, 0x05216B);

            int cx = w / 2;
            int cy = h / 2;

            int lim = cx;
            if (cy < lim) lim = cy;
            if (w - cx - 2 < lim) lim = w - cx - 2;
            if (h - cy - 2 < lim) lim = h - cy - 2;

            int r = h / 3;
            if (r > w / 3) r = w / 3;
            if (r > lim) r = lim;
            if (r < 48) return;

            Gfx.FillCircle(cx, cy, r,                 Mix(Glow0, WallBot, 110));
            Gfx.FillCircle(cx, cy, (r * 72) / 100,    Mix(Glow0, Glow1,    80));
            Gfx.FillCircle(cx, cy, (r * 46) / 100,    Mix(Glow0, Glow1,   165));
            Gfx.FillCircle(cx, cy, (r * 22) / 100,    Mix(Glow1, GlowHi,   90));

            for (int i = 0; i < 7; i++)
            {
                int rr = (r * (30 + i * 11)) / 100;
                if (rr >= r) break;
                Gfx.DrawCircle(cx, cy, rr, Mix(Glow1, GlowHi, 210 - i * 26));
            }
        }

        // =============================================================
        //  Portal surface - replaces the scattered Win11 icon grid.
        //  Mirrors gui.cpp::draw_portal_desktop(): a centered search bar,
        //  the NexOS wordmark, a shortcut tile grid, Home/Apps/System/
        //  Tools nav tabs and three live content cards.  The tile grid is
        //  still backed by the Desktop folder (dKind/dName/dCol/dLet), so
        //  right-click / rename / launch all keep working unchanged.
        // =============================================================
        static void PortalSurface(int w, int h)
        {
            PortalGeom(w, h);
            Brand(w);
            SearchBar(w);
            PortalTiles(w, h);
            NavTabs(w);
            PortalCards(w, h);
        }

        // Recompute every portal rectangle from the screen size.  Shared
        // by Paint and the hit-testers so they can never disagree.
        static void PortalGeom(int w, int h)
        {
            pSearchW = 420; if (pSearchW > w - 48) pSearchW = w - 48;
            pSearchH = 40;
            pSearchX = (w - pSearchW) / 2;
            pSearchY = 30;

            pTileW = 92; pTileH = 66; pTileGap = 14; pCols = 8;
            int gw = pCols * pTileW + (pCols - 1) * pTileGap;
            if (gw > w - 32)
            {
                pCols = (w - 32) / (pTileW + pTileGap);
                gw = pCols * pTileW + (pCols - 1) * pTileGap;
            }
            pGridX = (w - gw) / 2;
            pGridY = pSearchY + pSearchH + 30;

            int rows = (dN + pCols - 1) / pCols;
            if (rows < 1) rows = 1;
            pTabY = pGridY + rows * (pTileH + pTileGap) + 18;

            pCardY = pTabY + 28 + 16;
            int cardH = 104;
            if (pCardY + cardH > h - TaskH - 8)
                pCardY = (h - TaskH - 8) - cardH;
            if (pCardY < pTabY + 40) pCardY = pTabY + 40;
        }

        // Voice command (Chinese) for a desktop shortcut Kind.  Returns ""
        // for kinds that are intentionally NOT voice-enabled, which is the
        // whole point of the per-control opt-in: only the curated kinds
        // below ever respond to a voice phrase.  Add a kind here (and it
        // gets both its Chinese name and its English dName aliased) to make
        // that shortcut voice-launchable.
        static string VoiceAliasForKind(int k){
            if (k == Kind.Browser)      return "浏览器";
            if (k == Kind.Calculator)   return "计算器";
            if (k == Kind.Notepad)      return "记事本";
            if (k == Kind.ControlPanel) return "设置";
            if (k == Kind.Terminal)     return "终端";
            if (k == Kind.FileExplorer) return "文件";
            if (k == Kind.About)        return "关于";
            if (k == Kind.AiAgent)      return "人工智能";
            if (k == Kind.Demo)         return "演示";
            return "";
        }

        static void Brand(int w)
        {
            int ls = 10, lg = 3;
            int wordW = Gfx.Measure("NexOS");
            int groupW = (ls * 2 + lg) + 12 + wordW;
            int gx = (w - groupW) / 2;
            int gy = pSearchY - 36;
            uint a = Theme.Accent;
            Gfx.FillRect(gx,           gy,           ls, ls, a);
            Gfx.FillRect(gx + ls + lg, gy,           ls, ls, a);
            Gfx.FillRect(gx,           gy + ls + lg, ls, ls, a);
            Gfx.FillRect(gx + ls + lg, gy + ls + lg, ls, ls, a);
            Gfx.Text(gx + (ls * 2 + lg) + 12, gy + 1, "NexOS", a);
        }

        static void SearchBar(int w)
        {
            Gfx.FillRound(pSearchX, pSearchY, pSearchW, pSearchH, pSearchH / 2, 0xF2F5FAu);
            Gfx.DrawRound(pSearchX, pSearchY, pSearchW, pSearchH, pSearchH / 2, 0xCCD4E0u);
            int six = pSearchX + 16, siy = pSearchY + pSearchH / 2;
            Gfx.DrawCircle(six, siy, 6, 0x8A93A0u);
            Gfx.DrawLine(six + 5, siy + 5, six + 11, siy + 11, 0x8A93A0u);
            Gfx.Text(pSearchX + 32, pSearchY + (pSearchH - 16) / 2, "Search NexOS...", 0x8A93A0u);
        }

        static void PortalTiles(int w, int h)
        {
            for (int i = 0; i < dN; i++)
            {
                int col = i % pCols;
                int row = i / pCols;
                int tx = pGridX + col * (pTileW + pTileGap);
                int ty = pGridY + row * (pTileH + pTileGap);
                if (tx + pTileW > w) break;

                // Opt-in voice launch: only curated kinds (VoiceAliasForKind
                // returns non-empty) get a command.  Register the Chinese
                // alias AND the English display name as separate commands so
                // both "打开浏览器" and "open browser" resolve to this tile.
                string va1 = VoiceAliasForKind(dKind[i]);
                if (va1 != "")
                {
                    W.Voice(va1,      tx, ty, pTileW, pTileH);
                    W.Voice(dName[i], tx, ty, pTileW, pTileH);
                }

                bool hot = W.Hot(tx, ty, pTileW, pTileH);
                uint bg = hot ? 0xEAF2FBu : 0xFFFFFFFFu;
                Gfx.FillRound(tx, ty, pTileW, pTileH, 12, bg);
                Gfx.DrawRound(tx, ty, pTileW, pTileH, 12, 0xD5DDE8u);
                Gfx.Icon(tx + (pTileW - 32) / 2, ty + 8, 32, (uint)dCol[i], dLet[i], 0xFFFFFF);

                if (i == renameIdx)
                {
                    int bw = pTileW + 8, bh = 22;
                    int bx = tx - 4, by = ty + pTileH - 26;
                    Gfx.FillRound(bx, by, bw, bh, 4, 0xFFFFFFFF);
                    Gfx.DrawRound(bx, by, bw, bh, 4, Accent);
                    string shown = renameBuf;
                    if ((Host.Ticks() / 30) % 2 == 0) shown = U.Cat(shown, "|");
                    Gfx.Text(bx + 6, by + 4, shown, Ink);
                }
                else
                {
                    Gfx.TextCenter(tx, ty + pTileH - 20, pTileW, dName[i], Ink);
                }
            }
        }

        static void NavTabs(int w)
        {
            string[] tabs = new string[4];
            tabs[0] = "Home"; tabs[1] = "Apps"; tabs[2] = "System"; tabs[3] = "Tools";
            int tabW = 88, tabGap = 6, tabH = 30;
            int tw = 4 * tabW + 3 * tabGap;
            int tx0 = (w - tw) / 2;
            for (int i = 0; i < 4; i++)
            {
                int tx = tx0 + i * (tabW + tabGap);
                bool sel = (i == portalTab);
                uint tbg = sel ? Theme.Accent : (W.Hot(tx, pTabY, tabW, tabH) ? 0xEAF2FBu : 0xF2F5FAu);
                Gfx.FillRound(tx, pTabY, tabW, tabH, 8, tbg);
                Gfx.TextCenter(tx, pTabY + (tabH - 16) / 2, tabW, tabs[i],
                               sel ? 0xFFFFFFFFu : Ink);
            }
        }

        static void PortalCards(int w, int h)
        {
            int cardW = (w - 48) / 3;
            int cardH = 104;
            uint cardBg = 0xFFFFFFFFu, cardEdge = 0xE1E1E1u;

            // ---- Card 1: System Status ----
            {
                int cx = 16;
                Gfx.FillRound(cx, pCardY, cardW, cardH, 10, cardBg);
                Gfx.DrawRound(cx, pCardY, cardW, cardH, 10, cardEdge);
                Gfx.Text(cx + 14, pCardY + 10, "System Status", Theme.Accent);
                Gfx.DrawLine(cx + 14, pCardY + 30, cx + cardW - 14, pCardY + 30, cardEdge);
                Gfx.Text(cx + 14, pCardY + 38,
                         Host.Is64Bit() != 0 ? "Mode: 64-bit" : "Mode: 32-bit", Ink);
                Gfx.Text(cx + 14, pCardY + 58, U.Cat("RAM: ", U.Mb(Host.MemTotalKb())), Ink);
                int used = Host.PagesUsed(), tot = Host.PagesTotal();
                int pct = tot > 0 ? used * 100 / tot : 0;
                uint mc = pct < 50 ? C.Good : (pct < 80 ? C.Warn : C.Danger);
                Gfx.Progress(cx + 14, pCardY + 80, cardW - 28, 8, pct, mc);
            }
            // ---- Card 2: Clock ----
            {
                int cx = 16 + cardW + 16;
                Gfx.FillRound(cx, pCardY, cardW, cardH, 10, cardBg);
                Gfx.DrawRound(cx, pCardY, cardW, cardH, 10, cardEdge);
                Gfx.Text(cx + 14, pCardY + 10, "Clock", Theme.Accent);
                Gfx.DrawLine(cx + 14, pCardY + 30, cx + cardW - 14, pCardY + 30, cardEdge);
                string t = U.Cat(Two(Host.Hour()), ":", Two(Host.Minute()), ":", Two(Host.Second()));
                Gfx.TextCenter(cx, pCardY + 46, cardW, t, Theme.Accent);
                Gfx.TextCenter(cx, pCardY + 72, cardW, "NexOS Desktop", 0x606060u);
            }
            // ---- Card 3: Quick Actions ----
            {
                int cx = 16 + 2 * (cardW + 16);
                Gfx.FillRound(cx, pCardY, cardW, cardH, 10, cardBg);
                Gfx.DrawRound(cx, pCardY, cardW, cardH, 10, cardEdge);
                Gfx.Text(cx + 14, pCardY + 10, "Quick Actions", Theme.Accent);
                Gfx.DrawLine(cx + 14, pCardY + 30, cx + cardW - 14, pCardY + 30, cardEdge);
                Gfx.Text(cx + 14, pCardY + 40, "Click shortcuts above", 0x606060u);
                Gfx.Text(cx + 14, pCardY + 60, "or use Start menu", 0x606060u);
                Gfx.Text(cx + 14, pCardY + 80, "to launch apps", 0x606060u);
            }
        }

        // =============================================================
        //  Simple-mode desktop - the earlier clean Win11 surface:
        //  a left-aligned icon grid (top-to-bottom, then wrap to the
        //  next column) with no search bar / wordmark / nav tabs / cards.
        //  The icon table (dKind/dName/dCol/dLet) is shared with the
        //  portal tiles, so launch / rename / right-click all still work.
        // =============================================================
        // Classic (Simple-mode) icon geometry. Returns the cell origin
        // packed as 0xXXXXYYYY so we avoid `ref` parameters (unsupported
        // by the MiniCLR interpreter's call convention).
        static int ClassicRect(int i, int w, int h)
        {
            int perCol = PerCol(h);
            int col = i / perCol;
            int row = i % perCol;
            int x = Margin + col * Cell;
            int y = Margin + row * Cell;
            return (x << 16) | (y & 0xFFFF);
        }

        static void ClassicIcons(int w, int h)
        {
            for (int i = 0; i < dN; i++)
            {
                int r = ClassicRect(i, w, h);
                int x = (r >> 16) & 0xFFFF;
                int y = r & 0xFFFF;

                // Opt-in voice launch (same aliases as the portal tiles).
                string va2 = VoiceAliasForKind(dKind[i]);
                if (va2 != "")
                {
                    W.Voice(va2,      x, y, Cell, Cell);
                    W.Voice(dName[i], x, y, Cell, Cell);
                }

                bool hot = W.Hot(x, y, Cell, Cell);
                if (hot)
                    Gfx.FillRound(x + 2, y + 2, Cell - 4, Cell - 4, 8, IconHot);

                int tx = x + (Cell - IcoSz) / 2;
                int ty = y + 12;
                // Desktop icons use the same AI textures as the taskbar /
                // start-menu tiles; fall back to the old flat tile + letter.
                if (Gfx.HasImage(Tex.Icon + dKind[i]) != 0)
                    Gfx.Image(Tex.Icon + dKind[i], tx, ty, IcoSz, IcoSz);
                else
                {
                    Gfx.FillRound(tx, ty, IcoSz, IcoSz, 10, (uint)dCol[i]);
                    Gfx.TextCenter(tx, ty + (IcoSz - 16) / 2, IcoSz, Host.CharStr(dLet[i]), 0xFFFFFFFFu);
                }

                if (i == renameIdx)
                {
                    int bw = Cell + 8, bh = 20;
                    int bx = x - 4, by = ty + IcoSz + 4;
                    Gfx.FillRound(bx, by, bw, bh, 4, 0xFFFFFFFF);
                    Gfx.DrawRound(bx, by, bw, bh, 4, Accent);
                    string shown = renameBuf;
                    if ((Host.Ticks() / 30) % 2 == 0) shown = U.Cat(shown, "|");
                    Gfx.Text(bx + 4, by + 3, shown, Ink);
                }
                else
                {
                    string nm = dName[i];
                    int ly = ty + IcoSz + 8;
                    int tw = Gfx.Measure(nm);
                    int cx = x + (Cell - tw) / 2;
                    if (cx < 2) cx = 2;
                    // halo for legibility over the wallpaper (theme-aware)
                    Gfx.Text(cx - 1, ly - 1, nm, DeskHalo);
                    Gfx.Text(cx,     ly,     nm, DeskInk);
                }
            }
        }

        // Public toggle used by the context menu, the control panel and
        // WinHost.  0 = Simple, 1 = Busy.
        public static void SetMode(int m) { Theme.DesktopMode = m != 0 ? 1 : 0; }
        public static int  GetMode()      { return Theme.DesktopMode; }

        // ---- Virtual desktop (independent AI workspace) ----
        public static void SwitchDesktop(int d) {
            if (d == CurrentDesktop) return;
            CurrentDesktop = d;
            menuOpen = false;
            Host.Log(U.Cat("[DESK] switched to ", U.I(d), "\n"));
            // Keep (or stop) requesting animation repaints: the typewriter
            // pauses while the user is on the other desktop and resumes on
            // return.  Without this the loop would repaint the wrong desktop
            // forever while aiTypeActive is latched.
            if (CurrentDesktop == 1) Host.SetAnim((aiThinking != 0 || aiTypeActive != 0) ? 1 : 0);
            else Host.SetAnim(0);
            // caller (gui.cpp handle_ctrl -> render_all) repaints the frame
        }
        static int AiDesktopClick(int mx, int my, int w, int h) {
            AiEnsure();
            // The AI desktop uses the same shell chrome as the classic one:
            // a right-click context menu (e.g. "Switch to Default desktop")
            // and the Start menu must both stay clickable here.
            if (Popup.IsOpen()) return ContextClick(mx, my);
            if (menuOpen) return MenuClick(mx, my, w, h);
            // Taskbar (bottom strip) keeps working on the AI desktop.
            if (my >= h - TaskH) {
                int t = TrayHit(mx, my, w, h);
                if (t >= 0) { OpenTrayPopup(t, mx, my); return -1; }
                int bx = GroupX(w);
                int by = h - TaskH + (TaskH - BtnSz) / 2;
                if (U.In(mx, my, bx, by, BtnSz, BtnSz)) { menuOpen = true; return -1; }
                for (int i = 0; i < tN; i++) {
                    if (tKind[i] < 0) continue;
                    int x = bx + (i + 1) * (BtnSz + BtnGap);
                    if (U.In(mx, my, x, by, BtnSz, BtnSz)) return tKind[i];
                }
                return -1;
            }
            if (U.In(mx, my, AiInX(w), AiInY(h), AiInW(w), AiInH())) { aiFocus = 1; return -1; }
            if (U.In(mx, my, AiBtnX(w), AiBtnY(h), AiBtnW(), AiBtnH())) { AiSend(); return -1; }
            aiFocus = 0;
            return -1;
        }
                static void AiDesktopPaint(int w, int h) {
            AiEnsure();
            AiTypeTick();                  // advance the typewriter each frame
            Wallpaper(w, h);               // restore the Win11 wallpaper behind the AI panel
            int pw = AiPw(w), px = AiPx(w), py = AiPy(h), ph = AiPh(h);
            // Dark pill behind the branding so the label stays readable on the wallpaper.
            // NOTE: every colour is packed 0xAARRGGBB; the native Gfx treats the
            // high byte as a literal alpha, so we force 0xFF (opaque) here or the
            // panel/text would vanish against the light wallpaper.
            string aiBrand = "AI \u684C\u9762  \u00B7  \u865A\u62DF\u684C\u9762 2  \u00B7  \u804A\u5929";
            int brandW = Gfx.Measure(aiBrand);
            Gfx.FillRound(8, 6, brandW + 20, 28, 8, 0xFF14122Eu);
            Gfx.Text(18, 14, aiBrand, 0xFFC3B6FFu);
            Gfx.FillRound(px, py, pw, ph, 16, 0xFF14122Eu);
            Gfx.DrawRound(px, py, pw, ph, 16, 0xFF4A3FA0u);
            Gfx.FillRound(px, py, pw, 40, 16, 0xFF211D44u);
            Gfx.DrawRound(px, py, pw, 40, 16, 0xFF4A3FA0u);
            Gfx.Text(px + 16, py + 12, "NexOS AI \u52A9\u624B", 0xFFC3B6FFu);
            Gfx.Text(px + pw - 16 - Gfx.Measure("online"), py + 12, "online", 0xFF6FE0A0u);
            // History: newest at the bottom, just above the input row.
            // Compute each message's height first, then stack upward so long
            // messages never spill into the input box.
            int yBottom = AiInY(h) - 10;
            int shown = 0;
            for (int k = aiHistN - 1; k >= 0 && shown < 16; k--) {
                string draw = aiHist[k];
                // While the newest AI reply is still typing out, show only the
                // revealed byte prefix instead of the full message.
                if (aiTypeActive != 0 && k == aiHistN - 1) draw = U.Sub(aiTypeFull, 0, aiTypePos);
                // While thinking, animate the trailing dots on the placeholder
                // line ("AI: 思考中" + 0/1/2/3 dots) so it reads as "in progress".
                else if (aiThinking != 0 && k == aiHistN - 1) {
                    int d = (Host.Ticks() / 25) % 4;
                    string dotsStr = "";
                    if (d == 1) dotsStr = "."; else if (d == 2) dotsStr = ".."; else if (d == 3) dotsStr = "...";
                    draw = U.Cat("AI: 思考中", dotsStr);
                }
                int msgH = AiMsgLines(pw - 32, draw) * 18;
                if (yBottom - msgH < py + 46) break;
                yBottom -= msgH;
                AiDrawMsgAt(px + 16, yBottom, pw - 32, draw, 0xFFD8D2FFu);
                yBottom -= 6;
                shown++;
            }
            if (aiHistN == 0)
                Gfx.Text(px + 16, py + 52, "\u70B9\u8F93\u5165\u6846\u6253\u5B57\uFF0C\u56DE\u8F66\u53D1\u9001\uFF1B\u6211\u4F1A\u8C03\u7528 agent run \u540E\u7AEF\u3002", 0xFF8A7FD0u);
            // Input box + Send button
            int iy = AiInY(h);
            Gfx.FillRound(AiInX(w), iy, AiInW(w), AiInH(), 10, aiFocus == 1 ? 0xFF0E0C22u : 0xFF1A1736u);
            Gfx.DrawRound(AiInX(w), iy, AiInW(w), AiInH(), 10, 0xFF4A3FA0u);
            string shown2 = AiCodeToString(0, aiCodeN);
            if (shown2.Length == 0 && aiFocus == 0) shown2 = "\u8F93\u5165\u6307\u4EE4\uFF0C\u56DE\u8F66\u53D1\u9001\u2026";
            Gfx.Text(AiInX(w) + 10, iy + 11, shown2, aiFocus == 1 ? 0xFFE6E2FFu : 0xFF7C73B0u);
            if (aiFocus == 1 && (Host.Ticks() / 30) % 2 == 0) {
                int cw = Gfx.Measure(shown2);
                Gfx.FillRect(AiInX(w) + 10 + cw, iy + 9, 2, 18, 0xFFE6E2FFu);
            }
            int bx = AiBtnX(w), byx = AiBtnY(h);
            Gfx.FillRound(bx, byx, AiBtnW(), AiBtnH(), 10, 0xFF4A3FA0u);
            Gfx.DrawRound(bx, byx, AiBtnW(), AiBtnH(), 10, 0xFF6A5FD0u);
            Gfx.TextCenter(bx, byx + (AiBtnH() - 16) / 2, AiBtnW(), "\u53D1\u9001", 0xFFFFFFFFu);
            Gfx.TextCenter(0, h - TaskH - 14, w, "Ctrl+\u2192 \u8FDB\u5165 AI \u684C\u9762   \u00B7   Ctrl+\u2190 \u8FD4\u56DE   \u00B7   \u70B9\u8F93\u5165\u6846\u6253\u5B57\uFF0C\u56DE\u8F66\u53D1\u9001", 0xFF3A2C82u);
        }

        // Called by the native host AFTER the paint heap has been reset, so
        // any strings allocated here become part of the persistent baseline
        // instead of being discarded with the temporary paint allocations.
        // This is what lets us show a "思考中…" frame first and then run the
        // (blocking) agent call without the reply string becoming a dangling
        // reference on the next frame.
        public static void DeferredRun() {
            Host.Log(U.Cat("[AIDESK] DeferredRun thinking=", U.I(aiThinking), " pending=", aiPendingGoal != null ? "yes" : "no"));
            if (aiThinking != 0) { AiRunPending(); return; }
            // Safety net: if no animation is pending and the typewriter is
            // idle, clear any stale repaint request (e.g. AiRunPending was
            // skipped because the goal was empty or the run faulted early).
            if (aiTypeActive == 0 && aiPendingGoal == null) Host.SetAnim(0);
        }


        static int PerCol(int h)
        {
            int usable = h - TaskH - 12 - Margin;
            int p = usable / Cell;
            if (p < 1) p = 1;
            return p;
        }

        // =============================================================
        //  Layer 2 - taskbar + Start menu (painted above every window)
        // =============================================================
        public static void PaintOverlay(int w, int h)
        {
            ShellTheme();
            Taskbar(w, h);
            if (menuOpen || Anim.Get(START_KEY) != 0) StartMenu(w, h);
            if (Popup.IsOpen()) Popup.Paint(w, h);
            Toast.Paint(w, h);
        }

        static void Taskbar(int w, int h)
        {
            int y = h - TaskH;
            // Always paint a distinct, opaque dark bar. The bundled tex_task
            // texture is a light image that vanishes against the light
            // wallpaper, so we draw our own visible bar instead.
            Gfx.FillRect(0, y, w, TaskH, TaskBarBg);
            Gfx.FillRect(0, y, w, 1, TaskBarLine);

            int bx = GroupX(w);
            int by = y + (TaskH - BtnSz) / 2;

            // Start button lifts slightly on hover (smooth via W.Hover).
            int bl = (2 * W.Hover(bx, by, BtnSz, BtnSz)) / 1000;
            int sby = by - bl;
            if (menuOpen || W.Hot(bx, by, BtnSz, BtnSz))
                Gfx.FillRound(bx, sby, BtnSz, BtnSz, 8, BarHot);
            Logo(bx, sby);

            // Voice: "开始" opens (or toggles) the Start menu -- same rect
            // the mouse would hit, so the original Click handler runs.
            W.Voice("开始 start", bx, by, BtnSz, BtnSz);

            int mask = Host.RunningMask();
            for (int i = 0; i < tN; i++)
            {
                if (tKind[i] < 0) continue;   // uninstalled: skip the slot
                int x = bx + (i + 1) * (BtnSz + BtnGap);
                int lift = (2 * W.Hover(x, by, BtnSz, BtnSz)) / 1000;
                int dy = by - lift;
                if (W.Hot(x, by, BtnSz, BtnSz))
                    Gfx.FillRound(x, dy, BtnSz, BtnSz, 8, BarHot);
                if (Gfx.HasImage(Tex.Icon + tKind[i]) != 0)
                    Gfx.Image(Tex.Icon + tKind[i], x + 8, dy + 8, 24, 24);
                else
                    Gfx.Icon(x + 8, dy + 8, 24, (uint)tCol[i], tLet[i], 0xFFFFFF);
                if (((mask >> tKind[i]) & 1) != 0)
                    Gfx.FillRound(x + 13, dy + BtnSz - 3, 14, 3, 1, Theme.Accent);
            }

            TrayLayout(w, h);
            int tx = trayRect[0], ty = trayRect[1], tb = trayRect[2], tg = trayRect[3];
            int cw3 = 3 * tb + 2 * tg;
            Gfx.FillRound(tx - 10, ty - 5, cw3 + 20, tb + 10, 10, BarHot);
            for (int i = 0; i < 3; i++)
            {
                int cx = tx + i * (tb + tg);
                uint bg = W.Hot(cx, ty, tb, tb) ? BarHot : Bar;
                Gfx.FillRound(cx, ty, tb, tb, 9, bg);
                TrayGlyph(i, cx, ty, tb);
                // i == 1 is the microphone: "麦克风"/"voice" opens the voice
                // input popup (its original tray handler).
                if (i == 1) W.Voice("麦克风 voice", cx, ty, tb, tb);
            }

            string t = Clock();
            int tw = Gfx.Measure(t);
            Gfx.Text(w - 16 - tw, y + (TaskH - 16) / 2, t, TaskBarInk);
        }

        // Lay out the three tray buttons and stash the rect in trayRect.
        static void TrayLayout(int w, int h)
        {
            int y = h - TaskH + (TaskH - TrayBtn) / 2;
            int tw = Gfx.Measure(Clock());
            int clockX = w - 16 - tw;
            int clusterW = 3 * TrayBtn + 2 * TrayGap;
            int x = clockX - 14 - clusterW;
            if (x < 8) x = 8;
            trayRect[0] = x; trayRect[1] = y; trayRect[2] = TrayBtn; trayRect[3] = TrayGap;
        }

        // Which tray button (0..2) is at (mx,my), or -1.
        public static int TrayHit(int mx, int my, int w, int h)
        {
            TrayLayout(w, h);
            int x = trayRect[0], y = trayRect[1], tb = trayRect[2], gap = trayRect[3];
            if (my < y || my >= y + tb) return -1;
            for (int i = 0; i < 3; i++)
            {
                int bx = x + i * (tb + gap);
                if (mx >= bx && mx < bx + tb) return i;
            }
            return -1;
        }

        // Which taskbar *pin* is at (mx,my), or -1.  (Start is not a pin.)
        public static int TaskbarButtonAt(int mx, int my, int w)
        {
            int h = Gfx.Height();
            if (my < h - TaskH) return -1;
            int bx = GroupX(w);
            int by = h - TaskH + (TaskH - BtnSz) / 2;
            int x0 = bx + (BtnSz + BtnGap);   // first pin sits right of Start
            for (int i = 0; i < tN; i++)
            {
                int x = x0 + i * (BtnSz + BtnGap);
                if (U.In(mx, my, x, by, BtnSz, BtnSz)) return tKind[i];
            }
            return -1;
        }

        static void TrayGlyph(int which, int x, int y, int s)
        {
            int cx = x + s / 2;
            if (which == 0)
            {
                Gfx.FillRound(x + 7, y + 8, 13, 11, 2, Theme.Accent);
                Gfx.FillRound(x + 13, y + 13, 13, 11, 2, 0x8B5CF6);
                Gfx.FillRound(x + 19, y + 18, 7, 6, 2, 0xC4B5FD);
            }
            else if (which == 1)
            {
                uint c = Theme.VoiceOn != 0 ? 0x107C10u : 0x4B5563u;
                Gfx.FillRound(cx - 4, y + 8, 8, 14, 4, c);
                Gfx.DrawRound(cx - 7, y + 22, 14, 7, 3, c);
                Gfx.DrawLine(cx, y + 24, cx, y + 27, c);
            }
            else
            {
                uint c = 0x0EA5E9u;
                Gfx.DrawCircle(cx, y + s / 2, 9, c);
                Gfx.DrawLine(cx - 9, y + s / 2, cx + 9, y + s / 2, c);
                Gfx.DrawLine(cx, y + s / 2 - 9, cx, y + s / 2 + 9, c);
                Gfx.FillRect(cx - 2, y + s / 2 + 8, 4, 6, c);
            }
        }

        static void Logo(int x, int y)
        {
            int s = 7, g = 2;
            int o = (BtnSz - (s * 2 + g)) / 2;
            Gfx.FillRect(x + o,             y + o,             s, s, Accent);
            Gfx.FillRect(x + o + s + g,     y + o,             s, s, Accent);
            Gfx.FillRect(x + o,             y + o + s + g,     s, s, Accent);
            Gfx.FillRect(x + o + s + g,     y + o + s + g,     s, s, Accent);
        }

        // Default colour / letter for an app kind when it is not currently
        // pinned (used by the All apps list and by Pin-to-Start).
        public static uint AppColor(int kind)
        {
            switch (kind)
            {
                case 0: return 0x0078D4; case 1: return 0xF2B400; case 2: return 0x0F7B0F;
                case 3: return 0x2F3A45; case 4: return 0x00A3A3; case 5: return 0xD8541B;
                case 6: return 0x6B3FA0; case 7: return 0x1B6BC9; case 8: return 0x1A73E8;
                case 9: return 0x888888; case 10: return 0x0078D4; case 11: return 0x888888;
            }
            return 0x888888;
        }
        public static int AppLetter(int kind) { if (kind >= 0 && kind < 12) return kName[kind][0]; return '?'; }
        static bool IsStartPinned(int kind) { for (int i = 0; i < dN; i++) if (dKind[i] == kind) return true; return false; }

        static void StartMenu(int w, int h)
        {
            // open/close transition: slide up + fade in, Back overshoot.
            Anim.Set(START_KEY, menuOpen ? 1000 : 0, 220, 1);
            int enter = (int)Anim.Get(START_KEY);
            if (!menuOpen && enter == 0) return;

            int mw = MenuW(w), mh = MenuH(h);
            int x  = MenuX(w),  y = MenuY(h);
            startMenuX = x; startMenuY = y; startMenuW = mw; startMenuH = mh;
            y -= (16 * (1000 - enter)) / 1000;          // slide up from below

            uint bg = U.Fade(MenuBg, enter);
            uint ln = U.Fade(0xFF000000 | BarLine, enter);
            Gfx.FillRound(x, y, mw, mh, 10, bg);
            if (Gfx.HasImage(Tex.Menu) != 0)
                Gfx.Image(Tex.Menu, x + 3, y + 3, mw - 6, mh - 6);
            Gfx.DrawRound(x, y, mw, mh, 10, ln);

            if (enter < 380) return;                    // delay content until faded in

            Gfx.FillRound(x + 24, y + 18, mw - 48, 32, 8, FieldBg);
            Gfx.DrawRound(x + 24, y + 18, mw - 48, 32, 8, FieldEdge);
            Gfx.Text(x + 38, y + 26, "Search apps and files", Ghost);

            // Header: Pinned <-> All apps switch.
            if (startView == 0)
            {
                Gfx.Text(x + 28, y + 66, "Pinned", Ink);
                string all = "> All apps";
                int aw = Gfx.Measure(all);
                int ax = x + mw - 36 - aw;
                if (W.Hot(ax - 4, y + 62, aw + 12, 20))
                    Gfx.FillRound(ax - 4, y + 62, aw + 12, 20, 4, BarHot);
                Gfx.Text(ax, y + 66, all, Theme.Accent);
            }
            else
            {
                string back = "< Pinned";
                int bw = Gfx.Measure(back);
                if (W.Hot(x + 24, y + 62, bw + 12, 20))
                    Gfx.FillRound(x + 24, y + 62, bw + 12, 20, 4, BarHot);
                Gfx.Text(x + 32, y + 66, back, Theme.Accent);
                Gfx.Text(x + 40 + bw, y + 66, "All apps", Ink);
            }

            if (startView == 0)
            {
                // Pinned tile grid (same apps as the desktop icon grid).
                int tw = TileW(mw), th = 84;
                int gx = x + 28, gy = y + 94;
                for (int i = 0; i < dN; i++)
                {
                    int row = i / 4;
                    int col = i - row * 4;
                    int tx = gx + col * tw, ty = gy + row * th;
                    if (W.Hot(tx, ty, tw - 6, th - 6))
                        Gfx.FillRound(tx, ty, tw - 6, th - 6, 6, 0xE7EEF8);
                    if (Gfx.HasImage(Tex.Icon + dKind[i]) != 0)
                        Gfx.Image(Tex.Icon + dKind[i], tx + (tw - 6 - 32) / 2, ty + 10, 32, 32);
                    else
                        Gfx.Icon(tx + (tw - 6 - 32) / 2, ty + 10, 32, (uint)dCol[i], dLet[i], 0xFFFFFF);
                    Gfx.TextCenter(tx, ty + 50, tw - 6, dName[i], Ink);
                    string vaM = VoiceAliasForKind(dKind[i]);
                    if (vaM != "")
                    {
                        W.Voice(vaM,      tx, ty, tw - 6, th - 6);
                        W.Voice(dName[i], tx, ty, tw - 6, th - 6);
                    }
                }
            }
            else
            {
                // All apps list: every installed kind, alphabetical-ish order.
                int rowH = 22, lx = x + 28, ly = y + 94, listW = mw - 56;
                int fyEnd = FootY(y, mh);
                for (int i = 0; i < 12; i++)
                {
                    int ry = ly + i * rowH;
                    if (ry + rowH > fyEnd - 4) break;
                    if (IsInstalled(i) == 0) continue;   // hidden when uninstalled
                    int kind = i;   // Kind order; good enough for the shell
                    if (W.Hot(lx, ry, listW, rowH - 1))
                        Gfx.FillRound(lx, ry, listW, rowH - 1, 4, BarHot);
                    if (Gfx.HasImage(Tex.Icon + kind) != 0)
                        Gfx.Image(Tex.Icon + kind, lx, ry + 2, 18, 18);
                    else
                        Gfx.Icon(lx, ry + 2, 18, AppColor(kind), AppLetter(kind), 0xFFFFFF);
                    Gfx.Text(lx + 26, ry + 3, kName[kind], Ink);
                }
            }

            int fy = FootY(y, mh);
            Gfx.FillRound(x + 8, fy, mw - 16, 48, 8, MenuFoot);
            Gfx.Icon(x + 20, fy + 9, 30, Accent, 'R', 0xFFFFFF);
            Gfx.Text(x + 60, fy + 17, "root", Ink);

            int px = PowerX(x, mw), rx = px - 48, py = fy + 4;
            Btn(rx, py, 40, 40, "Rst");
            Btn(px, py, 40, 40, "Off");
            // Voice: power + reboot from the Start menu footer.
            W.Voice("重启 reboot", rx, py, 40, 40);
            W.Voice("关机 shutdown", px, py, 40, 40);

            // Desktop switch toggle — reachable from the Start menu in BOTH the
            // classic and the AI desktop, so the user can always flip between them.
            int swx = x + 130, swy = fy + 8, swbw = 130, swbh = 32;
            uint swf = W.Hot(swx, swy, swbw, swbh) ? BarHot : Bar;
            Gfx.FillRound(swx, swy, swbw, swbh, 6, swf);
            Gfx.DrawRound(swx, swy, swbw, swbh, 6, FieldEdge);
            Gfx.TextCenter(swx, swy + (swbh - 16) / 2, swbw,
                            CurrentDesktop == 0 ? "AI 桌面" : "旧版桌面", Ink);
            // Voice: flip between the AI desktop and the classic desktop.
            W.Voice("切换桌面 switch desktop", swx, swy, swbw, swbh);
        }

        static void Btn(int x, int y, int w, int h, string s)
        {
            uint f = W.Hot(x, y, w, h) ? BarHot : Bar;
            Gfx.FillRound(x, y, w, h, 6, f);
            Gfx.DrawRound(x, y, w, h, 6, FieldEdge);
            Gfx.TextCenter(x, y + (h - 16) / 2, w, s, Ink);
        }

        // WinHost screenshot harness helper: open the Start menu and (optionally)
        // jump straight to the All apps list so the menu can be captured.
        internal static void OpenStartMenu(int showAll, int w, int h)
        {
            menuOpen = true;
            startView = (showAll != 0) ? 1 : 0;
            startMenuW = MenuW(w); startMenuH = MenuH(h);
            startMenuX = MenuX(w); startMenuY = MenuY(h);
        }

        // Resolve which Start-menu entry (pinned tile or All-apps row) sits
        // under (mx,my), returning its Kind or -1.  Mirrors the hit-test in
        // MenuClick / StartMenu exactly.
        static int StartEntryAt(int mx, int my)
        {
            int x = startMenuX, y = startMenuY, mw = startMenuW, mh = startMenuH;
            if (startView == 0)
            {
                int tw = TileW(mw), th = 84;
                int gx = x + 28, gy = y + 94;
                for (int i = 0; i < dN; i++)
                {
                    int row = i / 4;
                    int col = i - row * 4;
                    int tx = gx + col * tw, ty = gy + row * th;
                    if (U.In(mx, my, tx, ty, tw - 6, th - 6)) return dKind[i];
                }
            }
            else
            {
                int rowH = 22, lx = x + 28, ly = y + 94, listW = mw - 56;
                int fyEnd = FootY(y, mh);
                for (int i = 0; i < 12; i++)
                {
                    int ry = ly + i * rowH;
                    if (ry + rowH > fyEnd - 4) break;
                    if (U.In(mx, my, lx, ry, listW, rowH - 1)) return i;
                }
            }
            return -1;
        }

        // Right-click on a Start-menu app / tile: Pin to Start (when not
        // pinned) or Unpin from Start (when pinned), plus "App settings".
        static void OpenStartPopup(int mx, int my)
        {
            int kind = StartEntryAt(mx, my);
            if (kind < 0) return;            // clicked empty area -> ignore
            startRClickIdx = kind;
            bool pinned = IsStartPinned(kind);
            int cap = 5;
            string[] labs = new string[cap]; int[] acts = new int[cap]; int k = 0;
            if (pinned)
            {
                labs[k] = "Unpin from Start"; acts[k] = A_START_UNPIN; k++;
            }
            else
            {
                labs[k] = Lang.T("menu.pintostart"); acts[k] = A_START_PIN; k++;
            }
            labs[k] = Lang.T("menu.runasadmin"); acts[k] = -1; k++;        // disabled placeholder
            labs[k] = "";                 acts[k] = -1;          k++;    // separator
            labs[k] = Lang.T("menu.appsettings");     acts[k] = A_START_SETTINGS; k++;
            Popup.Open(OWNER_START, mx, my, labs, acts, k);
        }

        // Pin / unpin a kind on the Start menu tile grid.  dKind[]/dName[]/
        // dCol[]/dLet[] are the shared desktop-icon arrays; the Start pinned
        // grid reuses them, so pinning here also adds a desktop shortcut.
        static void StartPin(int kind, int pin)
        {
            if (pin != 0)
            {
                if (IsStartPinned(kind)) return;
                if (dN >= dKind.Length) return;
                dKind[dN] = kind; dName[dN] = kName[kind];
                dCol[dN]  = (int)AppColor(kind); dLet[dN] = AppLetter(kind);
                dN++;
                Host.Log("[START] pin " + kName[kind]);
            }
            else
            {
                for (int i = 0; i < dN; i++)
                {
                    if (dKind[i] == kind)
                    {
                        for (int j = i; j < dN - 1; j++)
                        {
                            dKind[j] = dKind[j + 1]; dName[j] = dName[j + 1];
                            dCol[j]  = dCol[j + 1];  dLet[j]  = dLet[j + 1];
                        }
                        dN--;
                        Host.Log("[START] unpin " + kName[kind]);
                        return;
                  }
                }
            }
        }

        // Right-click on the Start menu footer account chip -> the small
        // account / "Start settings" popup (Lock / Sign out / Settings).
        static void OpenStartSettingsPopup(int mx, int my)
        {
            int cap = 5;
            string[] labs = new string[cap]; int[] acts = new int[cap]; int k = 0;
            labs[k] = Lang.T("start.lock");          acts[k] = -1; k++;
            labs[k] = Lang.T("start.signout");        acts[k] = A_START_SETTINGS; k++;
            labs[k] = "";                   acts[k] = -1; k++;          // separator
            labs[k] = Lang.T("menu.startsettings");   acts[k] = A_START_SETTINGS; k++;
            labs[k] = Lang.T("menu.personalize");     acts[k] = -1; k++;
            Popup.Open(OWNER_START, mx, my, labs, acts, k);
        }

        // =============================================================
        //  Input
        // =============================================================
        public static int Click(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();
            if (CurrentDesktop == 1) return AiDesktopClick(mx, my, w, h);

            if (Popup.IsOpen()) return ContextClick(mx, my);

            if (menuOpen) return MenuClick(mx, my, w, h);

            if (my >= h - TaskH)
            {
                int t = TrayHit(mx, my, w, h);
                if (t >= 0) { OpenTrayPopup(t, mx, my); return -1; }
                int bx = GroupX(w);
                int by = h - TaskH + (TaskH - BtnSz) / 2;
                if (U.In(mx, my, bx, by, BtnSz, BtnSz)) { menuOpen = true; return -1; }
                for (int i = 0; i < tN; i++)
                {
                    int x = bx + (i + 1) * (BtnSz + BtnGap);
                    if (U.In(mx, my, x, by, BtnSz, BtnSz)) return tKind[i];
                }
                return -1;
            }

            PortalGeom(w, h);
            // In Simple mode there is no search bar / nav tabs: only the
            // left-aligned icon grid is hit-tested, then the desktop.
            if (Theme.DesktopMode == 0)
            {
                for (int i = 0; i < dN; i++)
                {
                    int r = ClassicRect(i, w, h);
                    int x = (r >> 16) & 0xFFFF;
                    int y = r & 0xFFFF;
                    if (U.In(mx, my, x, y, Cell, Cell)) return dKind[i];
                }
                return -2;
            }
            // Search bar -> open the Start menu (which has its own box).
            if (U.In(mx, my, pSearchX, pSearchY, pSearchW, pSearchH))
            { menuOpen = true; return -1; }
            // Nav tabs (Home/Apps/System/Tools): select and consume.
            {
                int tabW = 88, tabGap = 6, tabH = 30;
                int tw = 4 * tabW + 3 * tabGap;
                int tx0 = (w - tw) / 2;
                for (int i = 0; i < 4; i++)
                {
                    int tx = tx0 + i * (tabW + tabGap);
                    if (U.In(mx, my, tx, pTabY, tabW, tabH)) { portalTab = i; return -1; }
                }
            }
            // Shortcut tiles -> launch their managed Kind.
            for (int i = 0; i < dN; i++)
            {
                int col = i % pCols, row = i / pCols;
                int x = pGridX + col * (pTileW + pTileGap);
                int y = pGridY + row * (pTileH + pTileGap);
                if (U.In(mx, my, x, y, pTileW, pTileH)) return dKind[i];
            }
            return -2;
        }

        static int MenuClick(int mx, int my, int w, int h)
        {
            int mw = MenuW(w), mh = MenuH(h);
            int x  = MenuX(w),  y = MenuY(h);

            if (!U.In(mx, my, x, y, mw, mh)) { menuOpen = false; return -1; }

            int fy = FootY(y, mh);
            int px = PowerX(x, mw), rx = px - 48, py = fy + 4;
            if (U.In(mx, my, px, py, 40, 40)) { menuOpen = false; Host.Shutdown(); return -1; }
            if (U.In(mx, my, rx, py, 40, 40)) { menuOpen = false; Host.Reboot();   return -1; }

            int swx = x + 130, swy = fy + 8, swbw = 130, swbh = 32;
            if (U.In(mx, my, swx, swy, swbw, swbh))
                { menuOpen = false; SwitchDesktop(CurrentDesktop == 0 ? 1 : 0); return -1; }

            // Header toggle: "All apps >" (startView 0) or "< Pinned" (startView 1).
            if (startView == 0)
            {
                string all = "> All apps";
                int aw = Gfx.Measure(all);
                int ax = x + mw - 36 - aw;
                if (U.In(mx, my, ax - 4, y + 62, aw + 12, 20)) { startView = 1; return -1; }
            }
            else
            {
                string back = "< Pinned";
                int bw = Gfx.Measure(back);
                if (U.In(mx, my, x + 24, y + 62, bw + 12, 20)) { startView = 0; return -1; }
            }

            if (startView == 0)
            {
                int tw = TileW(mw), th = 84;
                int gx = x + 28, gy = y + 94;
                for (int i = 0; i < dN; i++)
                {
                    int row = i / 4;
                    int col = i - row * 4;
                    int tx = gx + col * tw, ty = gy + row * th;
                    if (U.In(mx, my, tx, ty, tw - 6, th - 6)) { menuOpen = false; return dKind[i]; }
                }
            }
            else
            {
                // All apps list rows.
                int rowH = 22, lx = x + 28, ly = y + 94, listW = mw - 56;
                int fy2 = FootY(y, mh);
                for (int i = 0; i < 12; i++)
                {
                    int ry = ly + i * rowH;
                    if (ry + rowH > fy2 - 4) break;
                    if (U.In(mx, my, lx, ry, listW, rowH - 1)) { menuOpen = false; return i; }
                }
            }
            return -1;
        }

        // =============================================================
        //  Right-click context menus  (kernel-native, no WinForms)
        // =============================================================
        public static int ContextOpen() { return Popup.IsOpen() ? 1 : 0; }

        public static void OnRightClick(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();
            // Start menu is modal: a right-click inside its rect opens the
            // per-app Pin / Unpin context menu instead of the desktop menu.
            // Recompute the menu rect here (cached fields are only populated
            // after the menu has been rendered at least once).
            int smx = MenuX(w), smy = MenuY(h), smw = MenuW(w), smh = MenuH(h);
            if (menuOpen && U.In(mx, my, smx, smy, smw, smh))
            {
                // Footer account chip (bottom-left) -> Start / account settings.
                int fy = FootY(smy, smh);
                if (U.In(mx, my, smx + 8, fy, smw - 16, 48))
                {
                    OpenStartSettingsPopup(mx, my);
                    return;
                }
                OpenStartPopup(mx, my);
                return;
            }
            int t = TrayHit(mx, my, w, h);
            if (t >= 0) { OpenTrayPopup(t, mx, my); return; }
            // Taskbar strip.  Right-clicking the START button opens the
            // Win+X power-user menu (Canvas reference); a pin opens its
            // jump list; an empty part of the bar keeps the Task Manager /
            // Taskbar settings menu.
            if (my >= h - TaskH)
            {
                int bx = GroupX(w), by = h - TaskH + (TaskH - BtnSz) / 2;
                if (mx >= bx && mx < bx + BtnSz && my >= by && my < by + BtnSz)
                    { OpenWinX(mx, my); return; }
                int k = TaskbarButtonAt(mx, my, w);
                if (k >= 0) { OpenJumpList(k, mx, my); return; }
                OpenTaskbarPopup(mx, my);
                return;
            }
            // Desktop icon: right-click opens the same file menu the File
            // Explorer uses, operating on the .lnk shortcut file.
            int icon = IconAt(mx, my);
            if (icon >= 0)
            {
                OpenFileMenu(OWNER_DESKTOP_FILE, 3, icon, mx, my);
                return;
            }
            OpenDesktopPopup(mx, my);
        }

        // A click while a context menu is up.
        static int ContextClick(int mx, int my)
        {
            int code = Popup.Hit(mx, my, Gfx.ScreenW(), Gfx.ScreenH());
            if (code == -1) { Popup.Close(); return -1; }
            if (code == -2) return -1;
            if (code == -3) { Popup.Close(); return -1; }   // dialog "Close" button
            if (code == A_F_OPENWITH)
            {
                if (fileOwner == OWNER_DESKTOP_FILE) OpenWithMenuDesktop();
                else OpenWithMenu();
                return -1;
            }
            // "Show more options" from the Win11 file menu -> the classic
            // (Windows-10-style) file menu, opened at the same point.
            if (code == A_F_CLASSIC)
            {
                OpenFileMenuClassic(fileOwner, fileFs, fileSel, fileMenuX, fileMenuY);
                return -1;
            }
            // Cascading flyouts on the desktop / Win+X menus: a ">" sentinel
            // closes this menu and opens its child at the same cursor point.
            if (code == A_VIEW)   { OpenViewSub(mx, my);  return -1; }
            if (code == A_NEW)    { OpenNewSub(mx, my);   return -1; }
            if (code == 24)       { OpenSortSub(mx, my);  return -1; }   // Sort by >
            if (code == A_MORE)   { OpenMoreSub(mx, my);  return -1; }
            if (code >= A_WINX_BASE && code < A_WINX_BASE + 100)
                { WinXDispatch(code - A_WINX_BASE); return -1; }
            if (code >= A_JUMP_RECENT && code < A_JUMP_RECENT + 16)
                { /* jump-list recent: focus / open target */ return -1; }
            if (Popup.Owner() == OWNER_WIN)
            {
                Shell.WinAction(winOwner, code & ~Popup.DangerBit);
                Popup.Close();
                return -1;
            }
            if (Popup.Owner() == OWNER_START)
            {
                int kind = startRClickIdx;
                if (code == A_START_PIN)   StartPin(kind, 1);
                else if (code == A_START_UNPIN) StartPin(kind, 0);
                else if (code == A_START_SETTINGS) Host.Log("[START] settings " + kName[kind]);
                Popup.Close();
                return -1;
            }
            // Dismiss the context menu FIRST, then dispatch.  Some actions
            // (Properties, PE-run errors) open a NEW popup of their own;
            // closing afterwards would instantly close that dialog too.
            Popup.Close();
            HandleAction(code);
            return -1;
        }

        static void HandleAction(int code)
        {
            int c = code & ~Popup.DangerBit;
            // Taskbar window menu.
            if (c == A_WIN_CLOSE || c == A_WIN_END) { Shell.CloseApp(taskWinKind); return; }
            // Desktop "Open with..." submenu targets.  The kernel must
            // create a real window, so hand these to the host (same as
            // Shell.OpenNotepad) instead of Shell.Open() which would only
            // make an unpainted C# instance.
            if (c == A_DESK_OPEN_NOTEPAD) { Host.OpenApp(Kind.Notepad); return; }
            if (c == A_DESK_OPEN_TERM)    { Host.OpenApp(Kind.Terminal); return; }
            // File actions: desktop shortcuts are handled here, everything
            // else forwards to the owning window (the File Explorer).
            if (c >= A_F_OPEN)
            {
                if (fileOwner == OWNER_DESKTOP_FILE) { FileAction(c); return; }
                Shell.FileAction(fileOwner, c);
                return;
            }
            if (code >= A_TASKS_BASE) { Host.OpenApp(code - A_TASKS_BASE); return; }
            if (code >= A_JUMP_RECENT && code < A_JUMP_RECENT + 16) { JumpRecent(code - A_JUMP_RECENT); return; }
            if (c == A_DISPLAY)      Shell.OpenSettings(3);    // Display settings
            else if (c == A_VIEW_LRG){ deskViewMode = 0; Host.Log("[VIEW] large"); Refresh(); }
            else if (c == A_VIEW_MED){ deskViewMode = 1; Host.Log("[VIEW] medium"); Refresh(); }
            else if (c == A_VIEW_SM) { deskViewMode = 2; Host.Log("[VIEW] small"); Refresh(); }
            else if (c == A_VIEW_ICONS){ deskShowIcons ^= 1; Host.Log("[VIEW] show-icons"); Refresh(); }
            else if (c == A_VIEW_AUTO) { deskAuto ^= 1; Host.Log("[VIEW] auto-arrange"); Refresh(); }
            else if (c == A_VIEW_GRID) { deskGrid ^= 1; Host.Log("[VIEW] align-grid"); Refresh(); }
            else if (c == A_NEW_SHC)  { Host.Log("[NEW] shortcut (placeholder)"); }
            else if (c == A_SORT_NAME)  SortBy(0);
            else if (c == A_SORT_SIZE)  SortBy(1);
            else if (c == A_SORT_TYPE)  SortBy(2);
            else if (c == A_SORT_DATE)  SortBy(3);
            else if (c == A_REFRESH)    Refresh();
            else if (c == A_PERSONALIZE) Shell.OpenSettings(6);
            else if (c == A_TERMINAL)    Host.OpenApp(Kind.Terminal);
            else if (c == A_NEWFOLDER || c == A_NEW_FOLDER) MkdirOnDesktop();
            else if (c == A_TASKMGR)      Host.OpenApp(Kind.TaskManager);
            else if (c == A_TASKBAR)      Shell.OpenSettings(7);
            else if (c == A_VOICE)        { SetVoice(Theme.VoiceOn == 0 ? 1 : 0); Theme.Save(); }
            else if (c == A_NET_ETH)      { Theme.ActiveNet = 0; Theme.Save(); }
            else if (c == A_NET_WIFI)     { Theme.ActiveNet = 1; Theme.Save(); }
            else if (c == A_NET_SETTINGS) Shell.OpenSettings(3);
            else if (c == A_MODE_SIMPLE) { SetMode(0); Theme.Save(); Host.Log("[LAYOUT] simple"); }
            else if (c == A_MODE_BUSY)   { SetMode(1); Theme.Save(); Host.Log("[LAYOUT] busy"); }
            else if (c == A_DESK_AI)  { SwitchDesktop(1); }
            else if (c == A_DESK_DEF) { SwitchDesktop(0); }
            // Taskbar jump-list actions.
            else if (c == A_JUMP_UNPIN)    { UnpinFromTaskbar(jumpKind); }
            else if (c == A_JUMP_NEWWIN)   { Host.OpenApp(jumpKind >= 0 ? jumpKind : Kind.FileExplorer); }
            else if (c == A_JUMP_CLOSEALL) { Shell.CloseApp(jumpKind >= 0 ? jumpKind : Kind.FileExplorer); }
        }

        // Remove a kind from the pinned taskbar set (jump-list "Unpin").
        static void UnpinFromTaskbar(int kind)
        {
            if (kind < 0 || tN <= 0) return;
            int dst = -1;
            for (int i = 0; i < tN; i++) if (tKind[i] == kind) { dst = i; break; }
            if (dst < 0) return;
            for (int i = dst; i < tN - 1; i++) { tKind[i] = tKind[i + 1]; tCol[i] = tCol[i + 1]; tLet[i] = tLet[i + 1]; }
            tN--;
        }

        // ---- desktop shortcut file actions (owner == OWNER_DESKTOP_FILE)
        public static void FileAction(int code)
        {
            if (fileFs != 3) { return; }
            int cnt = Host.FileCount(3);
            if (fileSel < 0 || fileSel >= cnt) { return; }
            string nm = Host.FileName(3, fileSel);
            // A .exe sitting on the desktop is a program: run it through
            // the PE loader instead of treating it as a shortcut/document.
            if (code == A_F_OPEN && U.IsExe(nm))
            {
                Host.RunExe(nm);
                return;
            }
            if (code == A_F_NOTEPAD) { Shell.OpenNotepad(nm); return; }
            if (code == A_F_OPEN || code == A_F_EDIT)
            {
                string body = Host.ReadText(3, nm);
                int k = ParseKind(body);
                if (k == Kind.Terminal) Shell.ExitGui();   // Terminal shortcut exits GUI
                else { Host.OpenApp(k); }
            }
            else if (code == A_F_TERM) Host.OpenApp(Kind.Terminal);
            else if (code == A_F_COPY) deskClip = nm;
            else if (code == A_F_DEL)
            {
                Host.FileDelete(3, nm);
                Host.FileRefresh();
                SyncFromFs();
            }
            else if (code == A_F_RENAME) BeginRename(fileSel, nm);
            else if (code == A_F_PROPS) ShowDeskProps(nm);
            else if (code == A_F_MKDIR) MkdirOnDesktop();
        }

        // Create a new desktop shortcut folder immediately (auto-unique
        // name), re-sync the icon list and drop into the inline rename
        // editor so the user can rename it (Enter confirms / Esc keeps it).
        static void MkdirOnDesktop()
        {
            string baseName = "New Folder";
            string name = baseName;
            int n = Host.FileCount(3);
            int k = 1;
            while (true)
            {
                bool clash = false;
                for (int i = 0; i < n; i++)
                    if (Host.FileName(3, i) == name) { clash = true; break; }
                if (!clash) break;
                name = U.Cat(baseName, " (", U.I(k), ")");
                k++;
                n = Host.FileCount(3);
            }
            Host.FileMkDir(3, name);
            Host.FileRefresh();
            SyncFromFs();
            // Find the new icon and enter rename mode on it.
            for (int i = 0; i < dN; i++)
                if (dName[i] == name) { BeginRename(i, U.Cat(name, ".lnk")); break; }
        }

        static void BeginRename(int idx, string lnkName)
        {
            renameIdx = idx;
            renameOld = lnkName;
            renameBuf = StripLnk(lnkName);
        }

        static void CommitRename()
        {
            if (renameIdx < 0) return;
            string newName = renameBuf;
            // Keep the .lnk suffix so it stays a shortcut.
            int nl = newName.Length;
            if (!(nl > 4 && newName[nl - 4] == '.' &&
                  (newName[nl - 3] == 'l' || newName[nl - 3] == 'L') &&
                  (newName[nl - 2] == 'n' || newName[nl - 2] == 'N') &&
                  (newName[nl - 1] == 'k' || newName[nl - 1] == 'K')))
                newName = U.Cat(newName, ".lnk");
            if (newName.Length > 0 && newName != renameOld)
                Host.FileRename(3, renameOld, newName);
            Host.FileRefresh();
            SyncFromFs();
            renameIdx = -1;
        }

        static void ShowDeskProps(string lnkName)
        {
            string ty = "Shortcut";
            string tgt = KindName(ParseKind(Host.ReadText(3, lnkName)));
            string[] labs = new string[4];
            int[]    acts = new int[4];
            labs[0] = U.Cat("Name:    ", StripLnk(lnkName)); acts[0] = -3;
            labs[1] = U.Cat("Type:    ", ty);                acts[1] = -3;
            labs[2] = U.Cat("Target:  ", tgt);               acts[2] = -3;
            labs[3] = "Close";                               acts[3] = -3;   // -3 = dismiss
            Popup.Open(OWNER_DESKTOP_FILE, Gfx.Width() / 2 - 90, Gfx.Height() / 2 - 70, labs, acts, 4);
        }

        // Win11 desktop right-click menu (Canvas reference).  The leading
        // "> " marks a cascading flyout; "* " marks the current selection.
        // Order: View > / Sort by > / Refresh / New > / Display settings /
        // Personalize / Show more options.
        static void OpenDesktopPopup(int mx, int my)
        {
            int cap = 8;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            labs[k] = "> View";              acts[k] = A_VIEW;       k++;
            labs[k] = "> Sort by";           acts[k] = 24;           k++; // sentinel for Sort sub-menu
            labs[k] = "Refresh\tF5";         acts[k] = A_REFRESH;    k++;
            labs[k] = "> New";               acts[k] = A_NEW;        k++;
            labs[k] = "Display settings";    acts[k] = A_DISPLAY;    k++;
            labs[k] = "Personalize";         acts[k] = A_PERSONALIZE;k++;
            labs[k] = "Show more options";   acts[k] = A_MORE;       k++;
            Popup.Open(OWNER_DESKTOP, mx, my, labs, acts, k);
        }

        // "View" flyout: icon size + the auto-arrange / grid / show-icons
        // toggles.  "Medium icons" carries "*" as the current selection.
        static void OpenViewSub(int sx, int sy)
        {
            int cap = 7;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            labs[k] = "Large icons";     acts[k] = A_VIEW_LRG;   k++;
            labs[k] = "* Medium icons";  acts[k] = A_VIEW_MED;   k++;
            labs[k] = "Small icons";     acts[k] = A_VIEW_SM;    k++;
            labs[k] = "";                acts[k] = -1;           k++;
            labs[k] = "Show desktop icons"; acts[k] = A_VIEW_ICONS; k++;
            labs[k] = "Auto arrange icons"; acts[k] = A_VIEW_AUTO; k++;
            labs[k] = "Align icons to grid"; acts[k] = A_VIEW_GRID; k++;
            Popup.Open(OWNER_DESKTOP, sx, sy, labs, acts, k);
        }

        // "New" flyout: create a folder or a shortcut on the desktop.
        static void OpenNewSub(int sx, int sy)
        {
            int cap = 2;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            labs[k] = "Folder";    acts[k] = A_NEW_FOLDER; k++;
            labs[k] = "Shortcut";   acts[k] = A_NEW_SHC;    k++;
            Popup.Open(OWNER_DESKTOP, sx, sy, labs, acts, k);
        }

        // "Show more options" -> the classic (legacy) desktop menu, carrying
        // the extra utilities Win11 tucks behind the second click.
        static void OpenMoreSub(int sx, int sy)
        {
            int cap = 9;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            labs[k] = "View";              acts[k] = A_VIEW;       k++;
            labs[k] = "Refresh";           acts[k] = A_REFRESH;    k++;
            labs[k] = "New";               acts[k] = A_NEW;        k++;
            labs[k] = "";                  acts[k] = -1;           k++;
            labs[k] = "Display settings";  acts[k] = A_DISPLAY;    k++;
            labs[k] = "Personalize";       acts[k] = A_PERSONALIZE;k++;
            labs[k] = "Open in terminal";  acts[k] = A_TERMINAL;   k++;
            labs[k] = "Task Manager";      acts[k] = A_TASKMGR;    k++;
            labs[k] = "Switch to AI desktop"; acts[k] = A_DESK_AI; k++;
            Popup.Open(OWNER_DESKTOP, sx, sy, labs, acts, k);
        }

        // "Sort by" flyout: choose the desktop icon ordering key.
        static void OpenSortSub(int sx, int sy)
        {
            int cap = 4;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            labs[k] = "Name";              acts[k] = A_SORT_NAME;  k++;
            labs[k] = "Size";              acts[k] = A_SORT_SIZE;  k++;
            labs[k] = "Item type";         acts[k] = A_SORT_TYPE;  k++;
            labs[k] = "Date modified";     acts[k] = A_SORT_DATE; k++;
            Popup.Open(OWNER_DESKTOP, sx, sy, labs, acts, k);
        }

        // Taskbar right-click on a pin: close window / end process.
        static void OpenTaskWinPopup(int kind, int mx, int my)
        {
            taskWinKind = kind;
            string[] labs = new string[2];
            int[]    acts = new int[2];
            labs[0] = "Close window"; acts[0] = A_WIN_CLOSE;
            labs[1] = "End process";  acts[1] = A_WIN_END;
            Popup.Open(OWNER_TASKBAR, mx, my, labs, acts, 2);
        }

        static void OpenTaskbarPopup(int mx, int my)
        {
            string[] labs = new string[2];
            int[]    acts = new int[2];
            labs[0] = "Task Manager";   acts[0] = A_TASKMGR;
            labs[1] = "Taskbar settings"; acts[1] = A_TASKBAR;
            Popup.Open(OWNER_TASKBAR, mx, my, labs, acts, 2);
        }

        static void OpenTrayPopup(int t, int mx, int my)
        {
            if (t == 0)            OpenTasksPopup(mx, my);
            else if (t == 1)       OpenVoicePopup(mx, my);
            else                   OpenNetworkPopup(mx, my);
        }

        // Win11 window (Alt+Space) menu: Restore / Move / Size / Minimize /
        // Maximize / Close.  Move / Size are live gestures the immediate-mode
        // shell cannot capture, so they are surfaced as actions the host
        // accepts (and ignores when unsupported) - the menu still closes.
        public static void OpenWinMenu(int id, int sx, int sy)
        {
            winOwner = id;
            // "Maximize" is greyed when already maximised; "Restore" only
            // makes sense when the window is minimised / maximised.  We keep
            // all six rows present (faithful to Win11) and let the host no-op
            // the geometry actions it cannot perform.
            int cap = 6;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            labs[k] = Lang.T("win.restore"); acts[k] = WAct.Restore;  k++;
            labs[k] = Lang.T("win.move");    acts[k] = WAct.Move;     k++;
            labs[k] = Lang.T("win.size");    acts[k] = WAct.Size;     k++;
            labs[k] = Lang.T("win.min");     acts[k] = WAct.Minimize; k++;
            labs[k] = Lang.T("win.max");     acts[k] = WAct.Maximize; k++;
            labs[k] = Lang.T("win.close") + "\tAlt+F4"; acts[k] = WAct.Close; k++;
            Popup.Open(OWNER_WIN, sx, sy, labs, acts, k);
        }

        // Taskbar jump list for a pinned app (Canvas reference: File
        // Explorer).  Header carries the app name; below it the task actions
        // (unpin / close all windows) and a "Recent" section with the user's
        // quick-access targets, then "New window".
        internal static void OpenJumpList(int kind, int mx, int my)
        {
            jumpKind = kind;
            string[] rec = new string[] { "Quick access", "Documents", "Pictures", "Downloads" };
            int headerCap = 1, taskCap = 3, recentCap = 4, tailCap = 1;
            int cap = headerCap + 1 + taskCap + 1 + recentCap + 1 + tailCap;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            labs[k] = kName[kind];   acts[k] = -3; k++;        // -3 = header row
            labs[k] = "";            acts[k] = -1; k++;        // separator
            labs[k] = "Unpin from taskbar"; acts[k] = A_JUMP_UNPIN; k++;
            labs[k] = "Close all windows";   acts[k] = A_JUMP_CLOSEALL; k++;
            labs[k] = "";            acts[k] = -1; k++;        // separator
            for (int i = 0; i < recentCap; i++)
                { labs[k] = rec[i]; acts[k] = A_JUMP_RECENT + i; k++; }
            labs[k] = "";            acts[k] = -1; k++;        // separator
            labs[k] = "New window";  acts[k] = A_JUMP_NEWWIN; k++;
            Popup.Open(OWNER_JUMP, mx, my, labs, acts, k);
        }

        // Win+X power-user menu (right-click Start / taskbar).  The full
        // 19-entry table, faithfully ordered as Windows 11 exposes it.
        internal static void OpenWinX(int mx, int my)
        {
            string[] labs = new string[] {
                "Terminal (Admin)", "Windows Terminal", "Apps & Features",
                "Mobility Center", "Power Options", "Event Viewer", "System",
                "Device Manager", "Disk Management", "Computer Management",
                "Windows PowerShell", "Task Manager", "Settings",
                "File Explorer", "Search", "Run", "Desktop",
                "Shut down or sign out >"
            };
            int[] acts = new int[] {
                A_WINX_BASE + 0,  A_WINX_BASE + 1,  A_WINX_BASE + 2,
                A_WINX_BASE + 3,  A_WINX_BASE + 4,  A_WINX_BASE + 5,  A_WINX_BASE + 6,
                A_WINX_BASE + 7,  A_WINX_BASE + 8,  A_WINX_BASE + 9,
                A_WINX_BASE + 10, A_WINX_BASE + 11, A_WINX_BASE + 12,
                A_WINX_BASE + 13, A_WINX_BASE + 14, A_WINX_BASE + 15, A_WINX_BASE + 16,
                A_WINX_BASE + 17
            };
            Popup.Open(OWNER_WINX, mx, my, labs, acts, 18);
        }

        // Dispatch a Win+X entry (index into OpenWinX).  Maps each row to the
        // matching managed launch / settings page on the NexOS shell.
        static void WinXDispatch(int idx)
        {
            switch (idx)
            {
                case 0:  case 1:  Host.OpenApp(Kind.Terminal); break;     // Terminal / WT
                case 2:  Shell.OpenSettings(9); break;                    // Apps & Features
                case 3:  Shell.OpenSettings(1); break;                    // Mobility Center
                case 4:  Shell.OpenSettings(2); break;                    // Power Options
                case 5:  Shell.OpenSettings(4); break;                    // Event Viewer
                case 6:  Shell.OpenSettings(0); break;                    // System
                case 7:  Shell.OpenSettings(5); break;                    // Device Manager
                case 8:  Shell.OpenSettings(8); break;                    // Disk Management
                case 9:  Shell.OpenSettings(9); break;                    // Computer Management
                case 10: Host.OpenApp(Kind.Terminal); break;              // PowerShell
                case 11: Host.OpenApp(Kind.TaskManager); break;           // Task Manager
                case 12: Shell.OpenSettings(0); break;                    // Settings
                case 13: Host.OpenApp(Kind.FileExplorer); break;          // File Explorer
                case 14: Host.OpenApp(Kind.Terminal); break;              // Search (terminal stub)
                case 15: Shell.OpenSettings(0); break;                    // Run (settings stub)
                case 16: Host.OpenApp(Kind.ControlPanel); break;          // Desktop
                case 17: Host.OpenApp(Kind.About); break;                 // Shut down/sign out
                default: break;
            }
        }

        // Open a jump-list "Recent" target.  On NexOS these resolve to the
        // matching managed view; the index maps to the Quick-access list.
        static void JumpRecent(int idx)
        {
            Host.Log(U.Cat("[JUMP] recent ", U.I(idx)));
            Host.OpenApp(jumpKind >= 0 ? jumpKind : Kind.FileExplorer);
        }

        static void OpenTasksPopup(int mx, int my)
        {
            int mask = Host.RunningMask();
            int cap = 9;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            for (int i = 0; i < 8 && k < cap - 1; i++)
            {
                if (((mask >> i) & 1) != 0)
                {
                    labs[k] = kName[i];
                    acts[k] = A_TASKS_BASE + i;
                    k++;
                }
            }
            labs[k] = "";        acts[k] = -1; k++;
            labs[k] = "Open Task Manager"; acts[k] = A_TASKMGR; k++;
            Popup.Open(OWNER_TRAY, mx, my, labs, acts, k);
        }

        static void OpenVoicePopup(int mx, int my)
        {
            string[] labs = new string[1];
            int[]    acts = new int[1];
            labs[0] = Theme.VoiceOn != 0 ? "Turn off voice input" : "Turn on voice input";
            acts[0] = A_VOICE;
            Popup.Open(OWNER_TRAY, mx, my, labs, acts, 1);
        }

        static void OpenNetworkPopup(int mx, int my)
        {
            string[] labs = new string[3];
            int[]    acts = new int[3];
            labs[0] = Theme.ActiveNet == 0 ? "> Ethernet" : "Ethernet";  acts[0] = A_NET_ETH;
            labs[1] = Theme.ActiveNet == 1 ? "> Wi-Fi"    : "Wi-Fi";     acts[1] = A_NET_WIFI;
            labs[2] = "Network settings";                                acts[2] = A_NET_SETTINGS;
            Popup.Open(OWNER_TRAY, mx, my, labs, acts, 3);
        }

        // Win11-style file context menu (新版).  Top: Open / Open with▸
        // grouped together (matching the official "common commands next to
        // where the menu opens" layout).  Then a Cut / Copy / Paste / Rename
        // / Share / Delete cluster (Delete is destructive/red).  Bottom:
        // "Show more options" escapes to the classic Windows-10-style menu.
        // Opened by FileExplorerApp.OnRightClick and by the desktop; sx,sy
        // are screen coordinates so the menu lines up under the cursor.
        public static void OpenFileMenu(int id, int fs, int sel, int sx, int sy)
        {
            fileOwner = id; fileFs = fs; fileSel = sel;
            fileMenuX = sx; fileMenuY = sy;
            // An .exe is a program: its default action is "Run" (the PE
            // loader executes it), not "Open in Notepad".
            bool isExe = false;
            if (sel >= 0 && sel < Host.FileCount(fs)) isExe = U.IsExe(Host.FileName(fs, sel));
            bool readOnly = (fs == 1);
            int cap = 12;
            string[] labs = new string[cap];
            int[]    acts = new int[cap];
            int k = 0;
            if (isExe) labs[k] = "Run";  else labs[k] = "Open";
            acts[k] = A_F_OPEN; k++;
            labs[k] = "> Open with"; acts[k] = A_F_OPENWITH; k++;   // ▸ cascade
            labs[k] = "";            acts[k] = -1;           k++;    // separator
            labs[k] = "Cut";   acts[k] = A_F_CUT;   k++;
            labs[k] = "Copy";  acts[k] = A_F_COPY;  k++;
            labs[k] = "Paste"; acts[k] = A_F_PASTE; k++;
            labs[k] = "Rename"; acts[k] = A_F_RENAME; k++;
            labs[k] = "Share"; acts[k] = A_F_SHARE; k++;
            if (!readOnly) { labs[k] = "Delete"; acts[k] = A_F_DEL | Popup.DangerBit; k++; }
            labs[k] = "";            acts[k] = -1;           k++;    // separator
            labs[k] = "Show more options"; acts[k] = A_F_CLASSIC; k++;
            Popup.Open(OWNER_FILE, sx, sy, labs, acts, k);
        }

        // Classic (Windows-10-style) file menu, reached via "Show more
        // options".  Kept verbatim from the pre-Win11 layout so legacy
        // verbs (Edit, Open with Terminal, Properties, New file / folder)
        // stay reachable.  The SFS system volume (fs==1) is read-only:
        // mutating actions are hidden; "New file" only on the writable
        // MKFS volume (fs==0).
        static void OpenFileMenuClassic(int id, int fs, int sel, int sx, int sy)
        {
            fileOwner = id; fileFs = fs; fileSel = sel;
            fileMenuX = sx; fileMenuY = sy;
            bool readOnly = (fs == 1);
            bool canNewFile = (fs == 0);
            string[] labs = new string[13];
            int[]    acts = new int[13];
            int k = 0;
            bool isExe = false;
            if (sel >= 0 && sel < Host.FileCount(fs)) isExe = U.IsExe(Host.FileName(fs, sel));
            if (isExe) { labs[k] = "Run";  acts[k] = A_F_OPEN; k++; }
            else       { labs[k] = "Open"; acts[k] = A_F_OPEN; k++;
                         labs[k] = "Edit"; acts[k] = A_F_EDIT; k++; }
            labs[k] = "Open with Terminal"; acts[k] = A_F_TERM;  k++;
            labs[k] = "Open with...";     acts[k] = A_F_OPENWITH; k++;
            labs[k] = "";                 acts[k] = -1;          k++;
            labs[k] = "Copy";             acts[k] = A_F_COPY;    k++;
            if (!readOnly)
            {
                labs[k] = "Delete";           acts[k] = A_F_DEL | Popup.DangerBit; k++;
                labs[k] = "Rename";           acts[k] = A_F_RENAME;  k++;
            }
            labs[k] = "Properties";       acts[k] = A_F_PROPS;   k++;
            if (!readOnly)
            {
                labs[k] = "";                 acts[k] = -1;          k++;
                if (canNewFile)
                {
                    labs[k] = "New file";     acts[k] = A_F_NEWFILE; k++;
                }
                labs[k] = "New folder";       acts[k] = A_F_MKDIR;   k++;
            }
            Popup.Open(OWNER_FILE, sx, sy, labs, acts, k);
        }

        // Second-level menu behind "Open with..." for the File Explorer.
        static void OpenWithMenu()
        {
            string[] labs = new string[2];
            int[]    acts = new int[2];
            labs[0] = "Notepad";          acts[0] = A_F_NOTEPAD;
            labs[1] = "Terminal";         acts[1] = A_F_TERM;
            Popup.Open(OWNER_FILE, fileMenuX + 8, fileMenuY + 8, labs, acts, 2);
        }

        // Second-level menu behind "Open with..." for a desktop shortcut:
        // opens the chosen app directly (a shortcut has no file body to view).
        static void OpenWithMenuDesktop()
        {
            string[] labs = new string[2];
            int[]    acts = new int[2];
            labs[0] = "Notepad";  acts[0] = A_DESK_OPEN_NOTEPAD;
            labs[1] = "Terminal"; acts[1] = A_DESK_OPEN_TERM;
            Popup.Open(OWNER_DESKTOP_FILE, fileMenuX + 8, fileMenuY + 8, labs, acts, 2);
        }

        // Keystroke delivery for the desktop inline-rename editor.
        // ch is an ASCII code (or a control code from the host).  Ignored
        // unless the desktop is actively renaming an icon.
        public static void Key(int ch)
        {
            // mforms.cpp routes every unfocused keystroke straight here,
            // which makes this the only keyboard path the lock screen can
            // use.  Claim it first while the session is locked.
            if (Login.IsActive() != 0) { Login.Key(ch); return; }
            // Virtual-desktop switch: Ctrl+Left=-7 (default), Right=-8 (AI),
            // Up=-9 (toggle).  The kernel shortcut was changed from Ctrl+Win+arrows
            // to plain Ctrl+arrows; delivered via gui_handle_ctrl -> mforms_desktop_key.
            if (ch == -7) { SwitchDesktop(0); return; }
            if (ch == -8) { SwitchDesktop(1); return; }
            if (ch == -9) { if (CurrentDesktop == 0) SwitchDesktop(1); else SwitchDesktop(0); return; }
            if (CurrentDesktop == 1) { AiDesktopKey(ch); return; }
            if (renameIdx < 0) return;
            if (ch == '\n' || ch == '\r' || ch == -2) { CommitRename(); return; }
            if (ch == -3)                              // Ctrl+C: copy whole name
            {
                Host.SetClipboard(renameBuf == null ? "" : renameBuf);
                return;
            }
            if (ch == -4)                              // Ctrl+V: paste clipboard
            {
                renameUndo = renameBuf;                // snapshot before mutate
                renameBuf = Host.GetClipboard();
                return;
            }
            if (ch == -5)                              // Ctrl+Z: undo last edit
            {
                if (renameUndo != null && renameUndo.Length > 0) renameBuf = renameUndo;
                return;
            }
            if (ch == -6)                              // Ctrl+A: select-all (copy whole name)
            {
                Host.SetClipboard(renameBuf == null ? "" : renameBuf);
                return;
            }
            if (ch == 8 || ch == -1)                   // Backspace
            {
                renameUndo = renameBuf;                // snapshot before mutate
                int m = renameBuf.Length;
                if (m > 0)
                {
                    string r = "";
                    for (int i = 0; i < m - 1; i++) r = U.Cat(r, Host.CharStr((int)renameBuf[i]));
                    renameBuf = r;
                }
                return;
            }
            if ((ch >= 32 && ch < 127) || (ch >= 0x80 && ch <= 0xFFFF))
            {
                renameUndo = renameBuf;                // snapshot before mutate
                renameBuf = U.Cat(renameBuf, Host.CharStr(ch));
            }
        }

        // =============================================================
        //  Geometry helpers - paint and hit-test share them.
        // =============================================================
        static int GroupW()      { return (tN + 1) * BtnSz + tN * BtnGap; }
        static int GroupX(int w)
        {
            if (Theme.TaskbarLeft != 0) return 8;
            int x = (w - GroupW()) / 2;
            return x < 8 ? 8 : x;
        }

        static int MenuW(int w) { int r = 520; if (r > w - 40) r = w - 40; return r; }
        static int MenuH(int h) { int r = 430; int lim = h - TaskH - 40; if (r > lim) r = lim; return r; }
        static int MenuX(int w) { return (w - MenuW(w)) / 2; }
        static int MenuY(int h) { return h - TaskH - MenuH(h) - 10; }

        static int TileW(int mw)         { return (mw - 56) / 4; }
        static int FootY(int y, int mh)  { return y + mh - 56; }
        static int PowerX(int x, int mw) { return x + mw - 16 - 40; }

        // =============================================================
        //  Small utilities
        // =============================================================
        static string Two(int v)
        {
            if (v < 10) return U.Cat("0", U.I(v));
            return U.I(v);
        }

        static string Clock() { return U.Cat(Two(Host.Hour()), ":", Two(Host.Minute())); }

        static uint Mix(uint a, uint b, int t)
        {
            if (t < 0) t = 0;
            if (t > 255) t = 255;
            int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
            int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
            int r = ar + (br - ar) * t / 255;
            int g = ag + (bg - ag) * t / 255;
            int l = ab + (bb - ab) * t / 255;
            return ((uint)r << 16) | ((uint)g << 8) | (uint)l;
        }
    }
}
