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

        // ---- palette -------------------------------------------------
        const uint WallTop  = 0x05162C;   // wallpaper gradient
        const uint WallBot  = 0x0B4A83;
        const uint Glow0    = 0x0E3F6E;   // bloom rim
        const uint Glow1    = 0x2C86D6;   // bloom mid
        const uint GlowHi   = 0xBFE4FF;   // bloom core
        const uint IconHot  = 0x1D5A96;   // desktop icon hover
        const uint Bar      = 0xF2F5FA;   // taskbar fill (light theme)
        const uint BarLine  = 0xD5DDE8;
        const uint BarHot   = 0xE1EAF6;
        const uint Ink      = 0x1B1B1B;
        const uint MenuBg   = 0xF7F9FC;
        const uint MenuFoot = 0xEBF0F7;
        const uint FieldBg  = 0xFFFFFF;
        const uint FieldEdge= 0xCCD4E0;
        const uint Ghost    = 0x8A93A0;
        const uint Accent   = 0x0078D4;

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

        static bool menuOpen;

        // ---- system tray cluster (left of the clock) ----------------
        // Buttons: 0 = background tasks, 1 = voice input, 2 = network.
        const int TrayBtn = 34;
        const int TrayGap = 4;
        static int[] trayRect;          // [x, y, btn, gap], laid out per frame
        static int sortMode;            // desktop icon sort (0..3)

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
        // Taskbar window menu (close window / end process).
        public const int A_WIN_CLOSE  = 20, A_WIN_END = 21;
        // Desktop "Open with..." submenu targets (open the app directly).
        public const int A_DESK_OPEN_NOTEPAD = 22, A_DESK_OPEN_TERM = 23;
        public const int OWNER_DESKTOP = 0, OWNER_TASKBAR = 1, OWNER_TRAY = 2, OWNER_FILE = 3, OWNER_WIN = 4;
        public const int OWNER_DESKTOP_FILE = 5;   // a right-clicked desktop shortcut

        static string[] kName;         // canonical name per kind (unsorted)
        static int fileOwner = -1, fileFs = 0, fileSel = -1;
        static int fileMenuX = 0, fileMenuY = 0;   // popup origin for "Open with..." sub-menu
        static int winOwner = -1;      // app that opened the generic window menu
        static int taskWinKind = -1;   // kind of the window the taskbar menu targets
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

            kName = new string[9];
            kName[0] = "Settings"; kName[1] = "This PC"; kName[2] = "Terminal";
            kName[3] = "Calculator"; kName[4] = "Task Mgr"; kName[5] = "Optimizer";
            kName[6] = "Notepad"; kName[7] = "About"; kName[8] = "Browser";

            Popup.Init();
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
            dN = 10;
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

        static string KindName(int k)
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
            return "Unknown";
        }

        // Hit-test a desktop icon; returns its index or -1.
        public static int IconAt(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();
            int per = PerCol(h), cw = Cell - 8;
            for (int i = 0; i < dN; i++)
            {
                int col = i / per, row = i - col * per;
                int x = Margin + col * Cell, y = Margin + row * Cell;
                if (U.In(mx, my, x, y, cw, cw)) return i;
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
            Wallpaper(w, h);
            Icons(w, h);
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

            Gfx.Gradient(0, 0, w, h, Theme.WallTop, Theme.WallBot);

            int cx = w / 2;
            int cy = (h * 44) / 100;

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

        static void Icons(int w, int h)
        {
            int per = PerCol(h);
            int cw  = Cell - 8;
            for (int i = 0; i < dN; i++)
            {
                int col = i / per;
                int row = i - col * per;
                int x = Margin + col * Cell;
                int y = Margin + row * Cell;
                if (x + cw > w) break;

                if (W.Hot(x, y, cw, cw)) Gfx.FillRound(x, y, cw, cw, 6, IconHot);
                if (Gfx.HasImage(Tex.Icon + dKind[i]) != 0)
                    Gfx.Image(Tex.Icon + dKind[i], x + (cw - IcoSz) / 2, y + 6, IcoSz, IcoSz);
                else
                    Gfx.Icon(x + (cw - IcoSz) / 2, y + 6, IcoSz, (uint)dCol[i], dLet[i], 0xFFFFFF);
                Gfx.TextCenter(x, y + 6 + IcoSz + 6, cw, dName[i], 0xFFFFFF);

                // Inline rename editor for the icon being renamed.
                if (i == renameIdx)
                {
                    int bw = cw + 8, bh = 22;
                    int bx = x - 4, by = y + IcoSz + 12;
                    Gfx.FillRound(bx, by, bw, bh, 4, 0xFFFFFFFF);
                    Gfx.DrawRound(bx, by, bw, bh, 4, Accent);
                    string shown = renameBuf;
                    if ((Host.Ticks() / 30) % 2 == 0) shown = U.Cat(shown, "|");
                    Gfx.Text(bx + 6, by + 4, shown, Ink);
                }
            }
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
            Taskbar(w, h);
            if (menuOpen) StartMenu(w, h);
            if (Popup.IsOpen()) Popup.Paint(w, h);
        }

        static void Taskbar(int w, int h)
        {
            int y = h - TaskH;
            if (Gfx.HasImage(Tex.Task) != 0)
                Gfx.Image(Tex.Task, 0, y, w, TaskH);
            else
                Gfx.FillRect(0, y, w, TaskH, Bar);
            Gfx.FillRect(0, y, w, 1, BarLine);

            int bx = GroupX(w);
            int by = y + (TaskH - BtnSz) / 2;

            if (menuOpen || W.Hot(bx, by, BtnSz, BtnSz))
                Gfx.FillRound(bx, by, BtnSz, BtnSz, 8, BarHot);
            Logo(bx, by);

            int mask = Host.RunningMask();
            for (int i = 0; i < tN; i++)
            {
                int x = bx + (i + 1) * (BtnSz + BtnGap);
                if (W.Hot(x, by, BtnSz, BtnSz))
                    Gfx.FillRound(x, by, BtnSz, BtnSz, 8, BarHot);
                if (Gfx.HasImage(Tex.Icon + tKind[i]) != 0)
                    Gfx.Image(Tex.Icon + tKind[i], x + 8, by + 8, 24, 24);
                else
                    Gfx.Icon(x + 8, by + 8, 24, (uint)tCol[i], tLet[i], 0xFFFFFF);
                if (((mask >> tKind[i]) & 1) != 0)
                    Gfx.FillRound(x + 13, by + BtnSz - 3, 14, 3, 1, Theme.Accent);
            }

            TrayLayout(w, h);
            int tx = trayRect[0], ty = trayRect[1], tb = trayRect[2], tg = trayRect[3];
            int cw3 = 3 * tb + 2 * tg;
            Gfx.FillRound(tx - 10, ty - 5, cw3 + 20, tb + 10, 10, BarHot);
            for (int i = 0; i < 3; i++)
            {
                int cx = tx + i * (tb + tg);
                uint bg = W.Hot(cx, ty, tb, tb) ? BarLine : 0xF7F9FC;
                Gfx.FillRound(cx, ty, tb, tb, 9, bg);
                TrayGlyph(i, cx, ty, tb);
            }

            string t = Clock();
            int tw = Gfx.Measure(t);
            Gfx.Text(w - 16 - tw, y + (TaskH - 16) / 2, t, Ink);
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

        static void StartMenu(int w, int h)
        {
            int mw = MenuW(w), mh = MenuH(h);
            int x  = MenuX(w),  y = MenuY(h);

            Gfx.FillRound(x, y, mw, mh, 10, MenuBg);
            if (Gfx.HasImage(Tex.Menu) != 0)
                Gfx.Image(Tex.Menu, x + 3, y + 3, mw - 6, mh - 6);
            Gfx.DrawRound(x, y, mw, mh, 10, BarLine);

            Gfx.FillRound(x + 24, y + 18, mw - 48, 32, 8, FieldBg);
            Gfx.DrawRound(x + 24, y + 18, mw - 48, 32, 8, FieldEdge);
            Gfx.Text(x + 38, y + 26, "Search apps and files", Ghost);

            Gfx.Text(x + 28, y + 66, "Pinned", Ink);

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
            }

            int fy = FootY(y, mh);
            Gfx.FillRound(x + 8, fy, mw - 16, 48, 8, MenuFoot);
            Gfx.Icon(x + 20, fy + 9, 30, Accent, 'R', 0xFFFFFF);
            Gfx.Text(x + 60, fy + 17, "root", Ink);

            int px = PowerX(x, mw), rx = px - 48, py = fy + 4;
            Btn(rx, py, 40, 40, "Rst");
            Btn(px, py, 40, 40, "Off");
        }

        static void Btn(int x, int y, int w, int h, string s)
        {
            uint f = W.Hot(x, y, w, h) ? BarHot : Bar;
            Gfx.FillRound(x, y, w, h, 6, f);
            Gfx.DrawRound(x, y, w, h, 6, FieldEdge);
            Gfx.TextCenter(x, y + (h - 16) / 2, w, s, Ink);
        }

        // =============================================================
        //  Input
        // =============================================================
        public static int Click(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();

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

            int per = PerCol(h), cw = Cell - 8;
            for (int i = 0; i < dN; i++)
            {
                int col = i / per;
                int row = i - col * per;
                int x = Margin + col * Cell, y = Margin + row * Cell;
                if (U.In(mx, my, x, y, cw, cw)) return dKind[i];
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

            int tw = TileW(mw), th = 84;
            int gx = x + 28, gy = y + 94;
            for (int i = 0; i < dN; i++)
            {
                int row = i / 4;
                int col = i - row * 4;
                int tx = gx + col * tw, ty = gy + row * th;
                if (U.In(mx, my, tx, ty, tw - 6, th - 6)) { menuOpen = false; return dKind[i]; }
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
            int t = TrayHit(mx, my, w, h);
            if (t >= 0) { OpenTrayPopup(t, mx, my); return; }
            // Taskbar strip: a right-click on a pin opens that window's
            // "Close window" / "End process" menu; empty bar keeps the
            // Task Manager / Taskbar settings menu.
            if (my >= h - TaskH)
            {
                int k = TaskbarButtonAt(mx, my, w);
                if (k >= 0) { OpenTaskWinPopup(k, mx, my); return; }
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
            if (code == A_F_OPENWITH)
            {
                if (fileOwner == OWNER_DESKTOP_FILE) OpenWithMenuDesktop();
                else OpenWithMenu();
                return -1;
            }
            if (Popup.Owner() == OWNER_WIN)
            {
                Shell.WinAction(winOwner, code & ~Popup.DangerBit);
                Popup.Close();
                return -1;
            }
            Host.Log(U.Cat("[CTX] code=", U.I(code), " owner=", U.I(Popup.Owner()), " fileOwner=", U.I(fileOwner), ""));
            HandleAction(code);
            Popup.Close();
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
            if (c == A_SORT_NAME)  SortBy(0);
            else if (c == A_SORT_SIZE)  SortBy(1);
            else if (c == A_SORT_TYPE)  SortBy(2);
            else if (c == A_SORT_DATE)  SortBy(3);
            else if (c == A_REFRESH)    Refresh();
            else if (c == A_PERSONALIZE) Shell.OpenSettings(6);
            else if (c == A_TERMINAL)    Host.OpenApp(Kind.Terminal);
            else if (c == A_NEWFOLDER)   { Host.FileMkDir(3, "New Folder"); Host.FileRefresh(); SyncFromFs(); }
            else if (c == A_TASKMGR)      Host.OpenApp(Kind.TaskManager);
            else if (c == A_TASKBAR)      Shell.OpenSettings(7);
            else if (c == A_VOICE)        SetVoice(Theme.VoiceOn == 0 ? 1 : 0);
            else if (c == A_NET_ETH)      Theme.ActiveNet = 0;
            else if (c == A_NET_WIFI)     Theme.ActiveNet = 1;
            else if (c == A_NET_SETTINGS) Shell.OpenSettings(3);
        }

        // ---- desktop shortcut file actions (owner == OWNER_DESKTOP_FILE)
        public static void FileAction(int code)
        {
            if (fileFs != 3) { Host.Log("[FA] early: fileFs!=3"); return; }
            int cnt = Host.FileCount(3);
            Host.Log(U.Cat(U.Cat("[FA] code=", U.I(code), " fs=", U.I(fileFs)),
                           U.Cat(" sel=", U.I(fileSel), " cnt=", U.I(cnt))));
            if (fileSel < 0 || fileSel >= cnt) { Host.Log("[FA] early: fileSel OOB"); return; }
            string nm = Host.FileName(3, fileSel);
            Host.Log(U.Cat("[FA] nm=[", nm, "]"));
            // A .exe sitting on the desktop is a program: run it through
            // the PE loader instead of treating it as a shortcut/document.
            if (code == A_F_OPEN && U.IsExe(nm))
            {
                Host.Log(U.Cat("[FA] running PE image ", nm));
                Host.RunExe(nm);
                return;
            }
            if (code == A_F_NOTEPAD) { Shell.OpenNotepad(nm); return; }
            if (code == A_F_OPEN || code == A_F_EDIT)
            {
                string body = Host.ReadText(3, nm);
                int k = ParseKind(body);
                Host.Log(U.Cat("[FA] body=[", body, "] k=", U.I(k)));
                if (k == Kind.Terminal) Shell.ExitGui();   // Terminal shortcut exits GUI
                else { Host.Log(U.Cat("[FA] OpenApp ", U.I(k))); Host.OpenApp(k); }
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
            else if (code == A_F_MKDIR) { Host.FileMkDir(3, "New Folder"); Host.FileRefresh(); SyncFromFs(); }
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
            labs[0] = U.Cat("Name:    ", StripLnk(lnkName)); acts[0] = A_F_PROPS;
            labs[1] = U.Cat("Type:    ", ty);                acts[1] = A_F_PROPS;
            labs[2] = U.Cat("Target:  ", tgt);               acts[2] = A_F_PROPS;
            labs[3] = "Close";                               acts[3] = A_F_PROPS;
            Popup.Open(OWNER_DESKTOP_FILE, Gfx.Width() / 2 - 90, Gfx.Height() / 2 - 70, labs, acts, 4);
        }

        static void OpenDesktopPopup(int mx, int my)
        {
            string[] labs = new string[9];
            int[]    acts = new int[9];
            int k = 0;
            labs[k] = "Sort by name";      acts[k] = A_SORT_NAME;  k++;
            labs[k] = "Sort by size";      acts[k] = A_SORT_SIZE;  k++;
            labs[k] = "Sort by type";      acts[k] = A_SORT_TYPE;  k++;
            labs[k] = "Sort by date modified"; acts[k] = A_SORT_DATE; k++;
            labs[k] = "";                  acts[k] = -1;           k++;
            labs[k] = "Refresh";           acts[k] = A_REFRESH;    k++;
            labs[k] = "Personalize";       acts[k] = A_PERSONALIZE; k++;
            labs[k] = "Open in terminal";  acts[k] = A_TERMINAL;   k++;
            labs[k] = "New folder";        acts[k] = A_NEWFOLDER;  k++;
            Popup.Open(OWNER_DESKTOP, mx, my, labs, acts, k);
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

        // Generic per-window context menu (Refresh / Close window).
        public static void OpenWinMenu(int id, int sx, int sy)
        {
            winOwner = id;
            string[] labs = new string[2];
            int[]    acts = new int[2];
            labs[0] = "Refresh";        acts[0] = WAct.Refresh;
            labs[1] = "Close window";   acts[1] = WAct.Close;
            Popup.Open(OWNER_WIN, sx, sy, labs, acts, 2);
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

        // Opened by FileExplorerApp.OnRightClick and by the desktop.
        // `sx,sy` are screen coordinates so the menu lines up under cursor.
        public static void OpenFileMenu(int id, int fs, int sel, int sx, int sy)
        {
            fileOwner = id; fileFs = fs; fileSel = sel;
            fileMenuX = sx; fileMenuY = sy;
            string[] labs = new string[12];
            int[]    acts = new int[12];
            int k = 0;
            // An .exe is a program: its default action is "Run" (the PE
            // loader executes it), not "Open in Notepad".
            bool isExe = false;
            if (sel >= 0 && sel < Host.FileCount(fs)) isExe = U.IsExe(Host.FileName(fs, sel));
            if (isExe) { labs[k] = "Run";  acts[k] = A_F_OPEN; k++; }
            else       { labs[k] = "Open"; acts[k] = A_F_OPEN; k++;
                         labs[k] = "Edit"; acts[k] = A_F_EDIT; k++; }
            labs[k] = "Open with Terminal"; acts[k] = A_F_TERM;  k++;
            labs[k] = "Open with...";     acts[k] = A_F_OPENWITH; k++;
            labs[k] = "";                 acts[k] = -1;          k++;
            labs[k] = "Copy";             acts[k] = A_F_COPY;    k++;
            labs[k] = "Delete";           acts[k] = A_F_DEL | Popup.DangerBit; k++;
            labs[k] = "Rename";           acts[k] = A_F_RENAME;  k++;
            labs[k] = "Properties";       acts[k] = A_F_PROPS;   k++;
            labs[k] = "";                 acts[k] = -1;          k++;
            labs[k] = "New folder";       acts[k] = A_F_MKDIR;   k++;
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
