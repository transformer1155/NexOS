// =====================================================================
//  ShellForm.cs  -  main partial of the WinForms harness form
// ---------------------------------------------------------------------
//  This is the Windows-side twin of gui.cpp's Win11Desktop: it owns the
//  window chrome, dragging, z-order, the taskbar->window mapping and
//  input routing, and drives the managed shell through exactly the same
//  call sequence mforms.cpp uses each frame:
//
//      Shell.PaintDesktop(w,h)          wallpaper + icons
//      <chrome>  Shell.Paint(id,cw,ch)  per window, clipped to client
//      Shell.PaintOverlay(w,h)          taskbar + Start menu, on top
//
//  Click routing mirrors mforms_desktop_click()'s contract:
//      >= 0  a Kind to launch (or focus, when already open)
//      -1    consumed by the shell
//      -2    not the shell's - hit-test the windows
//
//  The designer half (form properties) lives in ShellForm.Designer.cs;
//  the resources in ShellForm.resx.  Built from the Visual Studio
//  WinForms template, so it opens cleanly in the designer.
// =====================================================================
using System;
using System.Collections.Generic;
using System.Text;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Windows.Forms;
using NexOS.Forms;

namespace NexOS.WinHost
{
    // One live window: the host owns geometry, the shell owns content.
    // Public so the standalone AppHost (single-app .exe) can find the live
    // window record it opened via Shell.Open().
    public sealed class WinRec
    {
        public int Id;
        public int Kind;
        public int X, Y, W, H;
        public bool Minimized;
    }

    public partial class ShellForm : Form
    {
        // Chrome metrics, matching the kernel's window frame.
        // protected so the standalone AppHost (ShellForm subclass) can read them.
        protected const int TitleH = 34;
        const int Edge = 1;
        const int TaskH = 48;          // must equal Desktop.TaskH
        protected const int BtnW = 44;

        // Palette for the host-drawn chrome - DARK to match the shell's
        // dark acrylic panels (TaskBarBg rgba(26,26,28,.82), Start/Menu
        // rgba(28,28,31,.95) / rgba(39,39,43,.97)).  Mirrors the Canvas
        // Win11-design reference rather than the old light Win11 look.
        static readonly Color CFrame   = Color.FromArgb(0x1C, 0x1C, 0x1F);   // window body
        static readonly Color CBarAct  = Color.FromArgb(0x2A, 0x2A, 0x2E);   // active title bar
        static readonly Color CBarIn   = Color.FromArgb(0x21, 0x21, 0x25);   // inactive title bar
        static readonly Color CInk     = Color.FromArgb(0xF3, 0xF3, 0xF3);   // title text
        static readonly Color CInkDim  = Color.FromArgb(0x9A, 0xA0, 0xA6);   // dim text
        static readonly Color CClose   = Color.FromArgb(0xC4, 0x2B, 0x1C);   // close (design red)
        static readonly Color CHover   = Color.FromArgb(0x3A, 0x3A, 0x40);   // button hover

        protected readonly List<WinRec> wins = new List<WinRec>();   // back -> front
        int nextId = 0;
        int cascade = 0;

        WinRec drag; int dragDX, dragDY;
        int hoverBtnWin = -1, hoverBtnIdx = -1;
        int mouseX = -1, mouseY = -1;

        // All UI is drawn by the shared NexOS.Forms shell (Gfx / Popup /
        // BrowserApp) — no WinForms child controls live on this form.  The
        // only "native" widgets left are the host Form itself, its Timer
        // and the GDI+ surface it hands to Gfx.SetContext().
        readonly Font chromeFont = new Font("Segoe UI", 9f);

        // --shot support (parsed from the command line in the ctor).
        internal bool ShotRequested;
        internal string ShotPath = "winhost.png";
        internal int ShotDelay = 700;
        internal string ShotKinds = "";

        // --termtest support: scripted behavioural drive of the terminal
        // emulator (open, type, select, scroll, tab) with a PASS/FAIL
        // report and an optional screenshot -- proves the GNOME-Terminal
        // alignment without a human at the keyboard.
        internal bool TermTestRequested;
        internal string TermTestReport = "termtest.txt";
        internal string TermTestShot = "";

        public ShellForm(string[] args)
        {
            ParseArgs(args);
            InitializeComponent();
            SetStyle(ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw, true);

            SeedSandbox();
            Host.ShutdownHook = () => Close();
            Host.RebootHook = () => { wins.Clear(); nextId = 0; cascade = 0; Shell.Init(); Invalidate(); };
            Host.CloseAppHook = (kind) =>
            {
                for (int i = wins.Count - 1; i >= 0; i--)
                    if (wins[i].Kind == kind) { Shell.Close(wins[i].Id); wins.RemoveAt(i); }
                Invalidate();
            };
            Host.ExitGuiHook = () => Close();
            // Shared context-menu actions (OpenApp) must land as real
            // windows, exactly like gui.cpp's launch_app does in the VM.
            Host.OpenAppHook = LaunchOrFocus;
            Host.WinActionHook = WinAction;

            Shell.Init();
            LoadTextures();
            Host.StartTimeSync();   // network-accurate clock (fire-and-forget)
            if (ShotRequested) Login.BypassForHost();   // headless preview skips the lock screen

            var t = new System.Windows.Forms.Timer { Interval = 40 };   // ~25 fps
            t.Tick += (s, e) => Invalidate();
            t.Start();

            // Off-screen render path: open the listed apps, draw one frame
            // to a PNG and quit - lets the build prove the harness really
            // draws the shell without a human at the keyboard.
            if (ShotRequested)
            {
                var st = new System.Windows.Forms.Timer { Interval = ShotDelay };
                st.Tick += (s, e) =>
                {
                    st.Stop();

                    // Parse kinds.  A trailing ":c" means "open it and then
                    // script a click into its centred button" (used to drive
                    // the Demo animation to its settled "啊" state for the
                    // headless screenshot); ":r" means "open it and then
                    // right-click the client centre" to pop the OS's own
                    // Popup context menu for the screenshot.
                    var clickKinds = new List<int>();
                    var rclickKinds = new List<int>();
                    bool deskRClick = false;
                    bool deskView = false, deskSort = false, deskNew = false, deskMore = false;
                    bool startX = false;
                    bool startAll = false, startTileR = false, startSet = false;
                    bool fileRClick = false;
                    int pinJump = -1;
                    int winMenuKind = -1;
                    if (ShotKinds.Length > 0)
                        foreach (string tok in ShotKinds.Split(','))
                        {
                            string t = tok.Trim();
                            if (t.Length == 0) continue;
                            if (t == "desk:r") { deskRClick = true; continue; }
                            if (t == "desk:view") { deskRClick = true; deskView = true; continue; }
                            if (t == "desk:sort") { deskRClick = true; deskSort = true; continue; }
                            if (t == "desk:new")  { deskRClick = true; deskNew  = true; continue; }
                            if (t == "desk:more") { deskRClick = true; deskMore = true; continue; }
                            if (t == "start:x")  { startX = true; continue; }
                            if (t == "start:all") { startAll = true; continue; }
                            if (t == "start:r")   { startTileR = true; continue; }
                            if (t == "start:set") { startSet = true; continue; }
                            if (t == "file:r")  { fileRClick = true; continue; }
                            if (t.StartsWith("pin:")) { pinJump = int.Parse(t.Substring(4)); continue; }
                            if (t.StartsWith("win:") && t.EndsWith(":m"))
                                { winMenuKind = int.Parse(t.Substring(4, t.Length - 6)); continue; }
                            // "N@P" opens app kind N pre-navigated to
                            // settings page P (e.g. 0@2 = Control Panel /
                            // Display, where the Layout + Dark/Light toggles
                            // live).  WinHost-only convenience for screenshots.
                            if (t.Contains("@"))
                            {
                                int at = t.IndexOf('@');
                                int kind = int.Parse(t.Substring(0, at));
                                int page = int.Parse(t.Substring(at + 1));
                                if (kind == 0) Shell.OpenSettings(page);
                                else OpenKind(kind);
                                continue;
                            }
                            if (t.EndsWith(":c"))
                                clickKinds.Add(int.Parse(t.Substring(0, t.Length - 2)));
                            else if (t.EndsWith(":r"))
                                rclickKinds.Add(int.Parse(t.Substring(0, t.Length - 2)));
                            else
                                OpenKind(int.Parse(t));
                        }
                    foreach (int k in clickKinds) OpenKind(k);
                    foreach (int k in rclickKinds) OpenKind(k);

                    Action save = () =>
                    {
                        int w = ClientSize.Width, h = ClientSize.Height;
                        using (var bmp = new Bitmap(w, h))
                        {
                            using (var g = Graphics.FromImage(bmp))
                                RenderFrame(g, w, h);
                            bmp.Save(Path.GetFullPath(ShotPath),
                                     System.Drawing.Imaging.ImageFormat.Png);
                        }
                        Console.WriteLine("saved " + Path.GetFullPath(ShotPath));
                        Close();
                    };

                    if (clickKinds.Count == 0 && rclickKinds.Count == 0 && !deskRClick
                        && !startX && !startAll && !startTileR && !startSet
                        && !fileRClick && pinJump < 0 && winMenuKind < 0) { save(); return; }

                    // Let the freshly opened window paint at least once
                    // (so its button hit-box is computed), then dispatch a
                    // click on the centred button, then wait for the
                    // shrink animation to finish before saving.
                    var ct = new System.Windows.Forms.Timer { Interval = 160 };
                    ct.Tick += (s2, e2) =>
                    {
                        ct.Stop();
                        if (deskRClick)
                        {
                            Gfx.SetContext(CreateGraphicsSafe(), 0, 0,
                                           ClientSize.Width, ClientSize.Height);
                            Shell.DesktopRClick(ClientSize.Width / 2,
                                                ClientSize.Height / 2);
                            int cx = ClientSize.Width / 2, cy = ClientSize.Height / 2;
                            if (deskView || deskSort || deskNew || deskMore)
                            {
                                // Row = cy + PadY(6) + idx*ItemH(34) + ItemH/2(17)
                                int rowY = cy + 23, rowX = cx + 24;
                                int idx = deskView ? 0 : deskSort ? 1 : deskNew ? 3 : 6;
                                rowY = cy + 6 + idx * 34 + 17;
                                Shell.DesktopClick(rowX, rowY);
                            }
                        }
                        if (startX)
                        {
                            Gfx.SetContext(CreateGraphicsSafe(), 0, 0,
                                           ClientSize.Width, ClientSize.Height);
                            int bx = (ClientSize.Width - ((7 + 1) * 40 + 7 * 6)) / 2;
                            int sx = bx + 20, sy = ClientSize.Height - 24;
                            NexOS.Forms.Desktop.OpenWinX(sx, sy);
                        }
                        if (startAll || startTileR || startSet)
                        {
                            Gfx.SetContext(CreateGraphicsSafe(), 0, 0,
                                           ClientSize.Width, ClientSize.Height);
                            NexOS.Forms.Desktop.OpenStartMenu(startAll ? 1 : 0,
                                                                ClientSize.Width, ClientSize.Height);
                            if (startTileR)
                            {
                                // Right-click the first pinned tile (top-left of grid).
                                int W = ClientSize.Width, H = ClientSize.Height;
                                int smw = 520; if (smw > W - 40) smw = W - 40;
                                int smh = 430; int lim = H - 40 - 40; if (smh > lim) smh = lim;
                                int x = (W - smw) / 2, y = H - 40 - smh - 10;
                                int gx = x + 28, gy = y + 94;
                                int tw = (smw - 56) / 4;
                                Shell.DesktopRClick(gx + tw / 2, gy + 42);
                            }
                            else if (startSet)
                            {
                                // Right-click the footer account chip (bottom-left).
                                int W = ClientSize.Width, H = ClientSize.Height;
                                int smw = 520; if (smw > W - 40) smw = W - 40;
                                int smh = 430; int lim = H - 40 - 40; if (smh > lim) smh = lim;
                                int x = (W - smw) / 2, y = H - 40 - smh - 10;
                                int fy = y + smh - 56;
                                Shell.DesktopRClick(x + 40, fy + 24);
                            }
                        }
                        if (pinJump >= 0)
                        {
                            Gfx.SetContext(CreateGraphicsSafe(), 0, 0,
                                           ClientSize.Width, ClientSize.Height);
                            int bx = (ClientSize.Width - ((7 + 1) * 40 + 7 * 6)) / 2;
                            int px = bx + 46 + 20, py = ClientSize.Height - 24;
                            NexOS.Forms.Desktop.OpenJumpList(pinJump, px, py);
                        }
                        if (fileRClick)
                        {
                            WinRec fe = null;
                            foreach (var w in wins) if (w.Kind == 1) { fe = w; break; }
                            if (fe == null) OpenKind(1);
                            foreach (var w in wins) if (w.Kind == 1) { fe = w; break; }
                            if (fe != null)
                            {
                                Rectangle cr = ClientRectOf(fe);
                                Gfx.SetContext(CreateGraphicsSafe(),
                                               cr.X, cr.Y, cr.Width, cr.Height);
                                // File list row ~y=100 lands on the 2nd item.
                                Shell.RightClick(fe.Id, cr.Width / 2, 100, cr.X, cr.Y);
                            }
                        }
                        if (winMenuKind >= 0)
                        {
                            WinRec wr = null;
                            foreach (var w in wins) if (w.Kind == winMenuKind) { wr = w; break; }
                            if (wr == null) OpenKind(winMenuKind);
                            foreach (var w in wins) if (w.Kind == winMenuKind) { wr = w; break; }
                            if (wr != null)
                            {
                                Gfx.SetContext(CreateGraphicsSafe(), 0, 0,
                                               ClientSize.Width, ClientSize.Height);
                                NexOS.Forms.Desktop.OpenWinMenu(wr.Id, wr.X + 60, wr.Y + 17);
                            }
                        }
                        foreach (int k in clickKinds)
                        {
                            WinRec wnd = null;
                            foreach (var w in wins) if (w.Kind == k) wnd = w;
                            if (wnd == null) continue;
                            Rectangle cr = ClientRectOf(wnd);
                            Gfx.SetContext(CreateGraphicsSafe(),
                                           cr.X, cr.Y, cr.Width, cr.Height);
                            // Demo's "点我" button is centred horizontally
                            // and sits at client y ~128 (pad 18 + 80 + 30).
                            Shell.Click(wnd.Id, cr.Width / 2, 128);
                        }
                        foreach (int k in rclickKinds)
                        {
                            WinRec wnd = null;
                            foreach (var w in wins) if (w.Kind == k) wnd = w;
                            if (wnd == null) continue;
                            Rectangle cr = ClientRectOf(wnd);
                            Gfx.SetContext(CreateGraphicsSafe(),
                                           cr.X, cr.Y, cr.Width, cr.Height);
                            Shell.RightClick(wnd.Id, cr.Width / 2, 120,
                                             cr.X, cr.Y);
                        }
                        var rt = new System.Windows.Forms.Timer { Interval = 1200 };
                        rt.Tick += (s3, e3) => { rt.Stop(); save(); };
                        rt.Start();
                    };
                    ct.Start();
                };
                st.Start();
            }

            // Scripted terminal behavioural drive (--termtest).  Exercises
            // the GNOME-Terminal-aligned emulator through the public Shell
            // dispatch and writes a PASS/FAIL report (+ optional screenshot).
            if (TermTestRequested)
            {
                var tt = new System.Windows.Forms.Timer { Interval = 300 };
                tt.Tick += (s, e) =>
                {
                    tt.Stop();
                    RunTermTest();
                    Close();
                };
                tt.Start();
            }
        }

        // Best-effort: load the shared UI textures (same ids as the VM) from
        // assets/ near the repo root.  Missing files are ignored; the shell
        // falls back to flat theme colours via Gfx.HasImage.
        protected void LoadTextures()
        {
            string cwd = Directory.GetCurrentDirectory();
            string[] roots = new string[] {
                cwd,
                Path.Combine(cwd, ".."),
                Path.Combine(cwd, "..", ".."),
                Path.Combine(cwd, "..", "..", ".."),
                Path.Combine(cwd, "..", "..", "..", ".."),
                Path.Combine(cwd, "..", "..", "..", "..", ".."),
            };
            string assets = null;
            foreach (string r in roots)
            {
                string p = Path.Combine(r, "assets");
                if (Directory.Exists(p)) { assets = p; break; }
            }
            if (assets == null) return;
            Gfx.LoadImage(NexOS.Forms.Tex.Wall, Path.Combine(assets, "wallpaper.png"));
            Gfx.LoadImage(NexOS.Forms.Tex.Task, Path.Combine(assets, "taskbar.png"));
            Gfx.LoadImage(NexOS.Forms.Tex.Menu, Path.Combine(assets, "menu.png"));
            Gfx.LoadImage(NexOS.Forms.Tex.Chrome, Path.Combine(assets, "chrome.png"));
            Gfx.LoadImage(NexOS.Forms.Tex.WinBg, Path.Combine(assets, "winbg.png"));
            string[] kinds = { "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8" };
            for (int i = 0; i < kinds.Length; i++)
                Gfx.LoadImage(NexOS.Forms.Tex.Icon + i,
                              Path.Combine(assets, "icon_" + kinds[i] + ".png"));
        }

        void ParseArgs(string[] args)
        {
            if (args == null) return;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--shot")
                {
                    ShotRequested = true;
                    ShotPath = (i + 1 < args.Length) ? args[i + 1] : "winhost.png";
                    ShotDelay = (i + 2 < args.Length) ? int.Parse(args[i + 2]) : 700;
                    ShotKinds = (i + 3 < args.Length) ? args[i + 3] : "";
                }
                else if (args[i] == "--termtest")
                {
                    TermTestRequested = true;
                    TermTestReport = (i + 1 < args.Length) ? args[i + 1] : "termtest.txt";
                    TermTestShot = (i + 2 < args.Length) ? args[i + 2] : "";
                }
            }
        }

        // -----------------------------------------------------------------
        //  --termtest  :  scripted behavioural verification of TerminalApp
        // -----------------------------------------------------------------
        //  Drives the emulator through the same public Shell dispatch the
        //  kernel / host use, then asserts on observable state.  A PASS/FAIL
        //  report is written to TermTestReport; if TermTestShot is set, a
        //  final frame (with coloured demo lines) is saved for visual proof.
        void RunTermTest()
        {
            var sb = new StringBuilder();
            int pass = 0, fail = 0;
            void Check(string name, bool ok, string detail)
            {
                if (ok) { pass++; sb.Append("PASS  "); }
                else    { fail++; sb.Append("FAIL  "); }
                sb.Append(name);
                if (detail != null && detail.Length > 0)
                    sb.Append("  [" + detail + "]");
                sb.Append("\n");
            }

            // Open the terminal window (creates both the window and the App).
            OpenKind(Kind.Terminal);
            WinRec tw = null;
            foreach (var w in wins) if (w.Kind == Kind.Terminal) { tw = w; break; }
            if (tw == null) { Check("launch", false, "no terminal window"); WriteTermReport(sb, pass, fail); return; }
            int id = tw.Id;
            var ta = (TerminalApp)Shell.Get(id);
            if (ta == null) { Check("launch", false, "no TerminalApp instance"); WriteTermReport(sb, pass, fail); return; }
            Check("launch", true, "window id=" + id);

            // Title follows user@host:cwd$ (GNOME-style dynamic title).
            Check("title", ta.GetTitle().IndexOf("root@nexos") >= 0, ta.GetTitle());

            // Banner present on open.
            int baseCount = ta.ActiveTerm().count;
            Check("banner", baseCount >= 1, "lines=" + baseCount);

            // Prime a real off-screen paint so the terminal's layout cache
            // (contentX / rows / lastTotalRows ...) is populated.  Headless
            // Invalidate() paints may not fire before the scripted drive,
            // and the cache is otherwise only built inside OnPaint -- which
            // would leave CellAt() invalid (selection empty) and MaxView 0
            // (scrollback unable to move).
            void Prime()
            {
                using (var pb = new Bitmap(ClientSize.Width, ClientSize.Height))
                using (var pg = Graphics.FromImage(pb))
                    RenderFrame(pg, ClientSize.Width, ClientSize.Height);
            }
            Prime();

            // ---- triple-click line selection -> primary clipboard --------
            // contentX=PAD(8), contentY=TAB_H(26)+PAD(8)=34.  Three clicks
            // on row 0 select the whole logical line and copy it.
            int cx = 8 + 3, cy = 34 + 3;
            Shell.MouseDown(id, 0, cx, cy);
            Shell.MouseUp(id, 0, cx, cy);
            Shell.MouseDown(id, 0, cx, cy);
            Shell.MouseUp(id, 0, cx, cy);
            Shell.MouseDown(id, 0, cx, cy);
            Shell.MouseUp(id, 0, cx, cy);
            string sel = Host.GetClipboard();
            string line0 = ta.ActiveTerm().lines[0];   // oldest = banner line 0
            Check("select-line", sel == line0 && sel.Length > 0, "\"" + sel + "\"");

            // ---- type a command, run it, expect echoed output -----------
            void Type(string s) { for (int i = 0; i < s.Length; i++) Shell.Key(id, (int)s[i]); }
            int c0 = ta.ActiveTerm().count;
            Type("echo hello world"); Shell.Key(id, VK.Enter);
            int c1 = ta.ActiveTerm().count;
            Check("echo-output", c1 == c0 + 2, "delta=" + (c1 - c0));
            Check("echo-text", ta.LastLine().IndexOf("hello world") >= 0, ta.LastLine());

            // ---- command history (Up recalls last command) -------------
            Shell.Key(id, VK.Up);
            Check("history-up", ta.ActiveTerm().input == "echo hello world", "\"" + ta.ActiveTerm().input + "\"");
            Shell.Key(id, VK.CtrlU);   // clear the input line

            // ---- reverse search (Ctrl+R) --------------------------------
            Shell.Key(id, VK.CtrlR);
            Type("cho");               // matches "echo hello world"
            Check("reverse-search", ta.ActiveTerm().input.IndexOf("cho") >= 0, "\"" + ta.ActiveTerm().input + "\"");
            Shell.Key(id, VK.Enter);   // accept the match (runs it)
            Shell.Key(id, VK.CtrlU);

            // ---- scrollback: fill > a screen, then PageUp ---------------
            for (int k = 0; k < 40; k++) { Type("echo line " + k); Shell.Key(id, VK.Enter); }
            Prime();   // refresh lastTotalRows so MaxView is non-zero
            Shell.Key(id, VK.PageUp);
            Check("scrollback", ta.ActiveTerm().view > 0, "view=" + ta.ActiveTerm().view);
            Shell.Key(id, VK.PageDown);

            // ---- new tab (Ctrl+Shift+T) ---------------------------------
            int tabs0 = ta.TabCount();
            Shell.Key(id, VK.CsT);
            Check("new-tab", ta.TabCount() == tabs0 + 1, "tabs=" + ta.TabCount());

            // ---- clear screen (Ctrl+L) ----------------------------------
            Shell.Key(id, VK.CtrlL);
            Check("clear", ta.ActiveTerm().count == 0, "lines=" + ta.ActiveTerm().count);

            // ---- insert a few ANSI-coloured lines for the screenshot ----
            ta.TestAppend("\u001b[1;31mNexOS\u001b[0m Terminal - 16/256/true-colour demo:");
            ta.TestAppend("\u001b[31mred\u001b[0m \u001b[32mgreen\u001b[0m \u001b[34mblue\u001b[0m  (16-colour SGR)");
            ta.TestAppend("\u001b[38;5;46mbright green (256)\u001b[0m \u001b[38;5;201mmagenta (256)\u001b[0m");
            ta.TestAppend("\u001b[48;5;21mblue background (256)\u001b[0m \u001b[38;2;255;128;0morange (true)\u001b[0m");
            ta.TestAppend("user@nexos:~$ _");   // shows the block cursor

            if (TermTestShot != null && TermTestShot.Length > 0)
            {
                try
                {
                    int w = ClientSize.Width, h = ClientSize.Height;
                    using (var bmp = new Bitmap(w, h))
                    {
                        using (var g = Graphics.FromImage(bmp))
                            RenderFrame(g, w, h);
                        bmp.Save(Path.GetFullPath(TermTestShot),
                                 System.Drawing.Imaging.ImageFormat.Png);
                    }
                    sb.Append("shot  " + Path.GetFullPath(TermTestShot) + "\n");
                }
                catch (Exception ex) { sb.Append("shot  FAIL " + ex.Message + "\n"); }
            }

            WriteTermReport(sb, pass, fail);
        }

        void WriteTermReport(StringBuilder sb, int pass, int fail)
        {
            sb.Insert(0, "== NexOS TerminalApp --termtest ==\n");
            sb.Append("== " + pass + " passed, " + fail + " failed ==\n");
            try { File.WriteAllText(Path.GetFullPath(TermTestReport), sb.ToString()); }
            catch (Exception ex) { Console.WriteLine("termtest report write failed: " + ex.Message); }
            Console.WriteLine(sb.ToString());
        }

        // The shell's file browser reads two volumes; give it real folders
        // on disk so File Explorer / Notepad have something to show.
        public static void SeedSandbox()
        {
            string root = Environment.GetEnvironmentVariable("NexOS_WINHOST_FS");
            if (string.IsNullOrEmpty(root))
                root = Path.Combine(AppContext.BaseDirectory, "fs");
            Host.FsRoot = root;

            string mkfs = Path.Combine(root, "mkfs");
            string sfs = Path.Combine(root, "sfs");
            Directory.CreateDirectory(mkfs);
            Directory.CreateDirectory(sfs);
            Directory.CreateDirectory(Path.Combine(mkfs, "Documents"));
            Directory.CreateDirectory(Path.Combine(mkfs, "Downloads"));

            // Desktop shortcuts live as real .lnk files on MKFS (fs==3),
            // mirroring the kernel's seed_desktop_shortcuts() so the icons
            // are ordinary files (rename / delete / properties all work).
            string desk = Path.Combine(mkfs, "Desktop");
            Directory.CreateDirectory(desk);
            string[] lnkNames = { "This PC.lnk", "Terminal.lnk", "Calculator.lnk",
                                  "Task Mgr.lnk", "Settings.lnk", "Optimizer.lnk",
                                  "Notepad.lnk", "About.lnk", "Browser.lnk",
                                  "AI Setup.lnk", "AI Agent.lnk", "Demo.lnk" };
            string[] lnkKinds = { "1", "3", "4", "2", "0", "6", "7", "5", "8",
                                  "9", "10", "11" };
            for (int i = 0; i < lnkNames.Length; i++)
                Seed(Path.Combine(desk, lnkNames[i]), lnkKinds[i]);

            Seed(Path.Combine(sfs, "readme.txt"),
                 "NexOS.Forms running on the WinForms host.\n" +
                 "Every pixel here is drawn by the same C# sources the VM runs.");
            Seed(Path.Combine(sfs, "passwd"), "root:admin:0\nuser:user:1000");
            Seed(Path.Combine(sfs, "boot.cfg"), "gui=managed\nshell=shell.mex\nvbe=1280x720");
            Seed(Path.Combine(mkfs, "notes.txt"), "Edit me in Notepad.");
            Seed(Path.Combine(mkfs, "hello.txt"), "hello from the host sandbox");
        }

        static void Seed(string path, string body)
        {
            // A concurrent host may be seeding the same sandbox; a transient
            // file lock must not take the whole preview down.
            try { if (!File.Exists(path)) File.WriteAllText(path, body); }
            catch { }
        }

        // ---- helpers reused by the standalone AppHost --------------------
        public WinRec FindWin(int id)
        {
            foreach (var w in wins) if (w.Id == id) return w;
            return null;
        }

        // Bit i set when a window of Kind i is open -> taskbar indicators.
        int RunningMask()
        {
            int m = 0;
            foreach (var w in wins) m |= 1 << w.Kind;
            return m;
        }

        protected Rectangle ClientRectOf(WinRec w)
        {
            return new Rectangle(w.X + Edge, w.Y + TitleH,
                                 w.W - Edge * 2, w.H - TitleH - Edge);
        }

        // ---- rendering -------------------------------------------------
        protected override void OnPaint(PaintEventArgs e)
        {
            RenderFrame(e.Graphics, ClientSize.Width, ClientSize.Height);
        }

        // The whole frame, driven exactly like mforms.cpp drives it.
        // Protected so the standalone AppHost (single-app .exe) can render
        // just one window instead of the full desktop stack.
        protected void RenderFrame(Graphics g, int W, int H)
        {
            var p = PointToClient(Cursor.Position);
            bool inside = p.X >= 0 && p.Y >= 0 && p.X < W && p.Y < H;
            mouseX = inside ? p.X : -1;
            mouseY = inside ? p.Y : -1;

            Gfx.SetScreen(W, H);
            Gfx.SetMouse(mouseX, mouseY);
            Host.Running = RunningMask();
            Host.FileRefresh();                 // listings are per-frame

            // Layer 1: wallpaper + desktop icons.
            Gfx.SetContext(g, 0, 0, W, H);
            Shell.PaintDesktop(W, H);

            // Layer 2: windows, back to front.
            foreach (var w in wins)
            {
                if (w.Minimized) continue;
                DrawChrome(g, w);
                Rectangle cr = ClientRectOf(w);
                Gfx.SetContext(g, cr.X, cr.Y, cr.Width, cr.Height);
                Shell.Paint(w.Id, cr.Width, cr.Height);
            }

            // Layer 3: taskbar + Start menu + context menus, above everything.
            g.ResetTransform();
            g.ResetClip();
            Gfx.SetContext(g, 0, 0, W, H);
            Shell.PaintOverlay(W, H);

            g.ResetTransform();
            g.ResetClip();
        }

        protected void DrawChrome(Graphics g, WinRec w)
        {
            g.ResetTransform();
            g.ResetClip();
            g.SmoothingMode = SmoothingMode.AntiAlias;

            bool active = wins.Count > 0 && wins[wins.Count - 1] == w;
            int rad = 10;

            // Rounded contact shadow (layered alpha for a soft, modern fall).
            using (var shp = RoundRectPath(w.X, w.Y, w.W, w.H, rad))
            {
                for (int i = 6; i >= 1; i--)
                {
                    int a = (active ? 26 : 14) - i * 3;
                    if (a < 4) a = 4;
                    using (var sp = new SolidBrush(Color.FromArgb(a, 0, 0, 0)))
                    using (var pp = RoundRectPath(w.X + i, w.Y + i, w.W, w.H, rad))
                        g.FillPath(sp, pp);
                }
            }

            // Frosted translucent rim (mirrors gui.cpp draw_window): a thin
            // neutral band just outside the frame so the surface behind
            // shows through — the Windows 11 acrylic edge.  No blue glow.
            using (var gp = RoundRectPath(w.X - 3, w.Y - 3, w.W + 6, w.H + 6, rad + 2))
            using (var gb = new SolidBrush(Color.FromArgb(55, 0xD2, 0xD2, 0xD2)))
                g.FillPath(gb, gp);

            // Window body + dark acrylic-ish title bar (Canvas reference:
            // dark surfaces, near-white ink).
            using (var bp = RoundRectPath(w.X, w.Y, w.W, w.H, rad))
                g.FillPath(new SolidBrush(CFrame), bp);
            using (var tp = RoundRectPath(w.X, w.Y, w.W, TitleH, rad))
                g.FillPath(new SolidBrush(active ? CBarAct : CBarIn), tp);
            g.FillRectangle(new SolidBrush(CFrame),
                            w.X, w.Y + TitleH - rad, w.W, rad);
            // 1px hairline border: subtle light edge so the dark surface
            // reads against the blue wallpaper.
            using (var pn = new Pen(active ? Color.FromArgb(0x4E, 0x57, 0x66)
                                           : Color.FromArgb(0x33, 0x33, 0x38)))
                g.DrawPath(pn, RoundRectPath(w.X, w.Y, w.W, w.H,    rad));
            g.DrawLine(new Pen(Color.FromArgb(0x33, 0x33, 0x38)),
                       w.X, w.Y + TitleH, w.X + w.W - 1, w.Y + TitleH);

            // Accent dot + title.
            using (var b = new SolidBrush(Color.FromArgb(0x00, 0x78, 0xD4)))
                g.FillEllipse(b, w.X + 12, w.Y + TitleH / 2 - 4, 8, 8);
            g.SmoothingMode = SmoothingMode.None;
            using (var b = new SolidBrush(active ? CInk : CInkDim))
                g.DrawString(Shell.Title(w.Id), chromeFont, b, w.X + 28, w.Y + 9);

            // Minimise / maximise / close, drawn as vector glyphs so they
            // read crisply at any DPI (no font dependency).
            for (int i = 0; i < 3; i++)
            {
                int bx = w.X + w.W - BtnW * (3 - i);
                bool hot = hoverBtnWin == w.Id && hoverBtnIdx == i;
                if (hot)
                    g.FillRectangle(
                        new SolidBrush(i == 2 ? CClose : CHover),
                        bx, w.Y + 1, BtnW, TitleH - 2);
                int cx = bx + BtnW / 2, cy = w.Y + TitleH / 2;
                using (var pen = new Pen(i == 2 && hot ? Color.White
                                                      : CInk))
                {
                    if (i == 0) g.DrawLine(pen, cx - 5, cy, cx + 5, cy);
                    else if (i == 1) g.DrawRectangle(pen, cx - 5, cy - 5, 10, 10);
                    else { g.DrawLine(pen, cx - 5, cy - 5, cx + 5, cy + 5);
                           g.DrawLine(pen, cx + 5, cy - 5, cx - 5, cy + 5); }
                }
            }
        }

        // A rounded-rectangle GraphicsPath (WinHost-only, full GDI+).
        static GraphicsPath RoundRectPath(int x, int y, int w, int h, int r)
        {
            var p = new GraphicsPath();
            if (r * 2 > w) r = w / 2;
            if (r * 2 > h) r = h / 2;
            if (r <= 0) { p.AddRectangle(new Rectangle(x, y, w, h)); return p; }
            int d = r * 2;
            p.AddArc(x, y, d, d, 180, 90);
            p.AddArc(x + w - d - 1, y, d, d, 270, 90);
            p.AddArc(x + w - d - 1, y + h - d - 1, d, d, 0, 90);
            p.AddArc(x, y + h - d - 1, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        // ---- input -----------------------------------------------------
        WinRec HitWindow(int x, int y)
        {
            for (int i = wins.Count - 1; i >= 0; i--)
            {
                var w = wins[i];
                if (w.Minimized) continue;
                if (x >= w.X && x < w.X + w.W && y >= w.Y && y < w.Y + w.H) return w;
            }
            return null;
        }

        int HitTitleButton(WinRec w, int x, int y)
        {
            if (y < w.Y || y >= w.Y + TitleH) return -1;
            for (int i = 0; i < 3; i++)
            {
                int bx = w.X + w.W - BtnW * (3 - i);
                if (x >= bx && x < bx + BtnW) return i;
            }
            return -1;
        }

        void Raise(WinRec w)
        {
            wins.Remove(w);
            wins.Add(w);
        }

        // Scripted launch, used by --shot.
        protected void OpenKind(int kind) { LaunchOrFocus(kind); }

        // Launch a Kind, or focus/restore it when it is already open -
        // the same behaviour gui.cpp gives a taskbar button.
        void LaunchOrFocus(int kind)
        {
            foreach (var w in wins)
            {
                if (w.Kind != kind) continue;
                w.Minimized = false;
                Raise(w);
                return;
            }
            int id = Shell.Open(kind);
            if (id < 0) return;

            int cw = Math.Min(760, ClientSize.Width - 120);
            int chh = Math.Min(500, ClientSize.Height - TaskH - 80);
            var rec = new WinRec
            {
                Id = id,
                Kind = kind,
                W = cw,
                H = chh,
                X = 70 + (cascade % 6) * 28,
                Y = 46 + (cascade % 6) * 26,
            };
            cascade++;
            nextId++;
            wins.Add(rec);
        }

        // Window-geometry action from the Alt+Space / title-bar menu.
        // Mirrors gui.cpp's handling of the Win11 window menu: minimise,
        // maximise (toggle), restore (un-minimise / un-maximise).  Move /
        // Size are live drag gestures the harness does not capture, so they
        // are accepted but ignored here - the menu request closes cleanly.
        void WinAction(int id, int code)
        {
            WinRec w = null;
            foreach (var r in wins) if (r.Id == id) { w = r; break; }
            if (w == null) return;
            switch (code)
            {
                case 13: w.Minimized = true; break;                    // Minimize
                case 14: if (w.W < ClientSize.Width - 4) ToggleMax(w); break;  // Maximize
                case 10:                                                     // Restore
                    if (w.Minimized) w.Minimized = false;
                    else if (w.W >= ClientSize.Width - 4) ToggleMax(w);
                    break;
                default: break;                                       // Move / Size -> no-op
            }
            Invalidate();
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            int W = ClientSize.Width, H = ClientSize.Height;
            Gfx.SetScreen(W, H);
            Gfx.SetMouse(e.X, e.Y);

            // Arm the button press animation for every left click (screen
            // coords) - W.Button/Primary/Key shrink to half for 0.5 s.
            if (e.Button == MouseButtons.Left) Btn.PressScreen(e.X, e.Y);

            // Right button -> the OS's own context menus (Popup), exactly
            // like mforms_desktop_rclick / mforms_rclick in the VM.
            if (e.Button == MouseButtons.Right)
            {
                var rhit = HitWindow(e.X, e.Y);
                if (rhit != null)
                {
                    if (e.Y < rhit.Y + TitleH)
                        Desktop.OpenWinMenu(rhit.Id, e.X, e.Y);
                    else
                    {
                        Rectangle cr = ClientRectOf(rhit);
                        Shell.RightClick(rhit.Id, e.X - cr.X, e.Y - cr.Y,
                                         cr.X, cr.Y);
                    }
                }
                else Shell.DesktopRClick(e.X, e.Y);
                Invalidate();
                return;
            }

            // 1. The Start menu is modal, and 2. the taskbar (incl. tray
            //    popups) floats above every window - both go straight to
            //    the shared shell, which owns tray popups too.
            bool menuOpen = Shell.DesktopMenuOpen() != 0;
            if (menuOpen || e.Y >= H - TaskH)
            {
                Gfx.SetContext(CreateGraphicsSafe(), 0, 0, W, H);
                int r = Shell.DesktopClick(e.X, e.Y);
                if (r >= 0) LaunchOrFocus(r);
                Invalidate();
                return;
            }

            // 3. Windows, front to back.
            var hit = HitWindow(e.X, e.Y);
            if (hit != null)
            {
                Raise(hit);
                int btn = HitTitleButton(hit, e.X, e.Y);
                if (btn == 0) { hit.Minimized = true; Invalidate(); return; }
                if (btn == 1) { ToggleMax(hit); Invalidate(); return; }
                if (btn == 2)
                {
                    Shell.Close(hit.Id);
                    wins.Remove(hit);
                    Invalidate();
                    return;
                }
                if (e.Y < hit.Y + TitleH)
                {
                    drag = hit; dragDX = e.X - hit.X; dragDY = e.Y - hit.Y;
                    Invalidate();
                    return;
                }
                Rectangle cr = ClientRectOf(hit);
                int lx = e.X - cr.X, ly = e.Y - cr.Y;
                int b = (e.Button == MouseButtons.Middle) ? 1
                      : (e.Button == MouseButtons.Right) ? 2 : 0;
                Gfx.SetContext(CreateGraphicsSafe(), cr.X, cr.Y, cr.Width, cr.Height);
                Shell.MouseDown(hit.Id, b, lx, ly);
                if (b == 0) Shell.Click(hit.Id, lx, ly);   // left click still fires
                Invalidate();
                return;
            }

            // 4. Bare desktop / icons.
            Gfx.SetContext(CreateGraphicsSafe(), 0, 0, W, H);
            int k = Shell.DesktopClick(e.X, e.Y);
            if (k >= 0) LaunchOrFocus(k);
            Invalidate();
        }

        // Click handlers may call Gfx.Width()/Height()/MouseX(); they need
        // a live context but must not paint, so hand them a scratch DC.
        Graphics scratchDc;
        Graphics CreateGraphicsSafe()
        {
            if (scratchDc == null) scratchDc = CreateGraphics();
            return scratchDc;
        }

        void ToggleMax(WinRec w)
        {
            if (w.W >= ClientSize.Width - 4)
            {
                w.W = Math.Min(760, ClientSize.Width - 120);
                w.H = Math.Min(500, ClientSize.Height - TaskH - 80);
                w.X = 70; w.Y = 46;
            }
            else
            {
                w.X = 0; w.Y = 0;
                w.W = ClientSize.Width;
                w.H = ClientSize.Height - TaskH;
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (drag != null)
            {
                drag.X = e.X - dragDX;
                drag.Y = e.Y - dragDY;
                if (drag.Y < 0) drag.Y = 0;
                Invalidate();
                return;
            }
            int hw = -1, hb = -1;
            var w2 = HitWindow(e.X, e.Y);
            if (w2 != null)
            {
                int b = HitTitleButton(w2, e.X, e.Y);
                if (b >= 0) { hw = w2.Id; hb = b; }
            }
            if (hw != hoverBtnWin || hb != hoverBtnIdx)
            {
                hoverBtnWin = hw; hoverBtnIdx = hb;
                Invalidate();
            }

            // Route pointer motion to the window under the cursor so the
            // terminal can update hover state and extend a drag-selection.
            var wm = HitWindow(e.X, e.Y);
            if (wm != null && e.Y >= wm.Y + TitleH)
            {
                Rectangle cr = ClientRectOf(wm);
                Gfx.SetContext(CreateGraphicsSafe(), cr.X, cr.Y, cr.Width, cr.Height);
                Shell.MouseMove(wm.Id, e.X - cr.X, e.Y - cr.Y);
            }
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            drag = null;
            var wu = HitWindow(e.X, e.Y);
            if (wu != null && e.Y >= wu.Y + TitleH)
            {
                Rectangle cr = ClientRectOf(wu);
                Gfx.SetContext(CreateGraphicsSafe(), cr.X, cr.Y, cr.Width, cr.Height);
                int b = (e.Button == MouseButtons.Middle) ? 1
                      : (e.Button == MouseButtons.Right) ? 2 : 0;
                Shell.MouseUp(wu.Id, b, e.X - cr.X, e.Y - cr.Y);
            }
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            if (wins.Count == 0) return;
            var w = HitWindow(e.X, e.Y);
            if (w != null && e.Y >= w.Y + TitleH)
            {
                Rectangle cr = ClientRectOf(w);
                Gfx.SetContext(CreateGraphicsSafe(), cr.X, cr.Y, cr.Width, cr.Height);
                Shell.Wheel(w.Id, e.Delta);   // +delta = wheel up = scroll toward older
                Invalidate();
            }
        }

        protected override void OnKeyPress(KeyPressEventArgs e)
        {
            if (wins.Count == 0) return;
            // Literal text only.  OnKeyDown already handled (and suppressed)
            // every control / Ctrl / Alt / arrow / F-key combination, so a
            // control codepoint reaching here is spurious.  Forwarding the
            // real character also delivers IME / non-ASCII input (Chinese,
            // emoji) to the terminal exactly as GNOME Terminal does.
            if ((int)e.KeyChar < 32) { e.Handled = true; return; }
            var top = wins[wins.Count - 1];
            Shell.Key(top.Id, (int)e.KeyChar);
            Invalidate();
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.F5)
            {
                wins.Clear(); cascade = 0; Shell.Init(); Invalidate(); e.Handled = true;
                return;
            }
            if (wins.Count == 0) return;
            var top = wins[wins.Count - 1];

            // Ctrl+W closes any non-terminal window; in the terminal it is
            // "delete previous word" (handled below), so it must not close.
            if (e.Control && !e.Alt && e.KeyCode == Keys.W
                && top.Kind != NexOS.Forms.Kind.Terminal)
            {
                Shell.Close(top.Id); wins.Remove(top); Invalidate(); e.Handled = true;
                return;
            }

            int code = 0;
            bool handled = true;
            if (e.Control && e.Shift && !e.Alt)
            {
                if      (e.KeyCode == Keys.C) code = VK.CsC;
                else if (e.KeyCode == Keys.V) code = VK.CsV;
                else if (e.KeyCode == Keys.T) code = VK.CsT;
                else if (e.KeyCode == Keys.W) code = VK.CsW;
                else handled = false;
            }
            else if (e.Control && !e.Alt)
            {
                if      (e.KeyCode == Keys.C) code = VK.CtrlC;
                else if (e.KeyCode == Keys.V) code = VK.CtrlV;
                else if (e.KeyCode == Keys.Z) code = VK.CtrlZ;
                else if (e.KeyCode == Keys.A) code = VK.CtrlA;
                else if (e.KeyCode == Keys.E) code = VK.CtrlE;
                else if (e.KeyCode == Keys.U) code = VK.CtrlU;
                else if (e.KeyCode == Keys.K) code = VK.CtrlK;
                else if (e.KeyCode == Keys.W) code = VK.CtrlW;
                else if (e.KeyCode == Keys.L) code = VK.CtrlL;
                else if (e.KeyCode == Keys.D) code = VK.CtrlD;
                else if (e.KeyCode == Keys.R) code = VK.CtrlR;
                else if (e.KeyCode == Keys.Oemplus || e.KeyCode == Keys.Add) code = VK.CtrlPlus;
                else if (e.KeyCode == Keys.OemMinus || e.KeyCode == Keys.Subtract) code = VK.CtrlMinus;
                else if (e.KeyCode == Keys.D0 || e.KeyCode == Keys.NumPad0) code = VK.Ctrl0;
                else handled = false;
            }
            else if (e.Alt && !e.Control)
            {
                if      (e.KeyCode == Keys.F) code = VK.AltF;
                else if (e.KeyCode == Keys.B) code = VK.AltB;
                else handled = false;
            }
            else
            {
                if      (e.KeyCode == Keys.Back)     code = VK.Back;
                else if (e.KeyCode == Keys.Return)   code = VK.Enter;
                else if (e.KeyCode == Keys.Escape)   code = VK.Esc;
                else if (e.KeyCode == Keys.Tab)      code = VK.Tab;
                else if (e.KeyCode == Keys.Delete)   code = VK.Delete;
                else if (e.KeyCode == Keys.Up)       code = VK.Up;
                else if (e.KeyCode == Keys.Down)     code = VK.Down;
                else if (e.KeyCode == Keys.Left)     code = VK.Left;
                else if (e.KeyCode == Keys.Right)    code = VK.Right;
                else if (e.KeyCode == Keys.Home)     code = VK.HomeK;
                else if (e.KeyCode == Keys.End)      code = VK.EndK;
                else if (e.KeyCode == Keys.PageUp)   code = VK.PageUp;
                else if (e.KeyCode == Keys.PageDown) code = VK.PageDown;
                else if (e.KeyCode == Keys.F1)  code = VK.F1;
                else if (e.KeyCode == Keys.F2)  code = VK.F2;
                else if (e.KeyCode == Keys.F3)  code = VK.F3;
                else if (e.KeyCode == Keys.F4)  code = VK.F4;
                else if (e.KeyCode == Keys.F5)  code = VK.F5;
                else if (e.KeyCode == Keys.F6)  code = VK.F6;
                else if (e.KeyCode == Keys.F7)  code = VK.F7;
                else if (e.KeyCode == Keys.F8)  code = VK.F8;
                else if (e.KeyCode == Keys.F9)  code = VK.F9;
                else if (e.KeyCode == Keys.F10) code = VK.F10;
                else if (e.KeyCode == Keys.F11) code = VK.F11;
                else if (e.KeyCode == Keys.F12) code = VK.F12;
                else handled = false;
            }

            if (handled)
            {
                e.Handled = true;
                e.SuppressKeyPress = true;   // don't let OnKeyPress re-deliver
                Shell.Key(top.Id, code);
                Invalidate();
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                if (components != null) components.Dispose();
                if (scratchDc != null) scratchDc.Dispose();
            }
            base.Dispose(disposing);
        }

        // =============================================================
        //  Context menus, tray popups, file ops and dialogs
        // =============================================================
        //  All of these are owned by the shared shell now (Desktop.OnRightClick
        //  -> Popup, inline rename editor, ShowDeskProps, BrowserApp).  The
        //  host only routes right-clicks / tray clicks into Shell.DesktopRClick
        //  / Shell.RightClick / Shell.DesktopClick -- see OnMouseDown.  No
        //  WinForms menu, message box or input dialog exists here anymore.
    }
}
