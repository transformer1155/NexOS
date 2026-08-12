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
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Windows.Forms;
using NexOS.Forms;

namespace NexOS.WinHost
{
    // One live window: the host owns geometry, the shell owns content.
    internal sealed class WinRec
    {
        public int Id;
        public int Kind;
        public int X, Y, W, H;
        public bool Minimized;
    }

    public sealed partial class ShellForm : Form
    {
        // Chrome metrics, matching the kernel's window frame.
        const int TitleH = 34;
        const int Edge = 1;
        const int TaskH = 48;          // must equal Desktop.TaskH
        const int BtnW = 44;

        // Palette for the host-drawn chrome (Win11 light).
        static readonly Color CFrame = Color.FromArgb(0xCF, 0xCF, 0xCF);
        static readonly Color CBarAct = Color.FromArgb(0xFF, 0xFF, 0xFF);
        static readonly Color CBarIn = Color.FromArgb(0xF3, 0xF3, 0xF3);
        static readonly Color CInk = Color.FromArgb(0x1B, 0x1B, 0x1B);
        static readonly Color CInkDim = Color.FromArgb(0x77, 0x77, 0x77);
        static readonly Color CClose = Color.FromArgb(0xC4, 0x2B, 0x1C);
        static readonly Color CHover = Color.FromArgb(0xE8, 0xE8, 0xE8);

        readonly List<WinRec> wins = new List<WinRec>();   // back -> front
        int nextId = 0;
        int cascade = 0;

        WinRec drag; int dragDX, dragDY;
        int hoverBtnWin = -1, hoverBtnIdx = -1;
        int mouseX = -1, mouseY = -1;
        string clipboardPath = null;      // for the file "Copy" action

        // Native WebBrowser overlay for the Browser app (real Windows
        // Forms WebBrowser control, backed by the system's engine).  The
        // managed BrowserApp draws only inside the MiniCLR VM; on Windows
        // we host the genuine control instead so web pages render fully.
        System.Windows.Forms.WebBrowser browserCtl;
        TextBox browserUrl;
        Button browserGo;
        int browserWinId = -1;

        readonly Font chromeFont = new Font("Segoe UI", 9f);

        // --shot support (parsed from the command line in the ctor).
        internal bool ShotRequested;
        internal string ShotPath = "winhost.png";
        internal int ShotDelay = 700;
        internal string ShotKinds = "";

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

            Shell.Init();
            LoadTextures();

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
                    if (ShotKinds.Length > 0)
                        foreach (string k in ShotKinds.Split(','))
                            OpenKind(int.Parse(k.Trim()));

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
                st.Start();
            }
        }

        // Best-effort: load the shared UI textures (same ids as the VM) from
        // assets/ near the repo root.  Missing files are ignored; the shell
        // falls back to flat theme colours via Gfx.HasImage.
        void LoadTextures()
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
                if (args[i] != "--shot") continue;
                ShotRequested = true;
                ShotPath = (i + 1 < args.Length) ? args[i + 1] : "winhost.png";
                ShotDelay = (i + 2 < args.Length) ? int.Parse(args[i + 2]) : 700;
                ShotKinds = (i + 3 < args.Length) ? args[i + 3] : "";
                break;
            }
        }

        // The shell's file browser reads two volumes; give it real folders
        // on disk so File Explorer / Notepad have something to show.
        static void SeedSandbox()
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
            if (!File.Exists(path)) File.WriteAllText(path, body);
        }

        // Bit i set when a window of Kind i is open -> taskbar indicators.
        int RunningMask()
        {
            int m = 0;
            foreach (var w in wins) m |= 1 << w.Kind;
            return m;
        }

        Rectangle ClientRectOf(WinRec w)
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
        internal void RenderFrame(Graphics g, int W, int H)
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

            // Layer 2b: native WebBrowser overlay for the Browser app.
            // The shared BrowserApp still paints its text fallback behind
            // this; the real control is a child HWND that owns the client
            // area on Windows.
            SyncBrowser();

            // Layer 3: taskbar + Start menu, above everything.
            g.ResetTransform();
            g.ResetClip();
            Gfx.SetContext(g, 0, 0, W, H);
            Shell.PaintOverlay(W, H);

            g.ResetTransform();
            g.ResetClip();
        }

        void DrawChrome(Graphics g, WinRec w)
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

            // Window body + acrylic-ish title bar.
            using (var bp = RoundRectPath(w.X, w.Y, w.W, w.H, rad))
                g.FillPath(new SolidBrush(Color.FromArgb(0xF3, 0xF3, 0xF3)), bp);
            using (var tp = RoundRectPath(w.X, w.Y, w.W, TitleH, rad))
                g.FillPath(new SolidBrush(active ? Color.White : Color.FromArgb(0xF3, 0xF3, 0xF3)), tp);
            g.FillRectangle(new SolidBrush(Color.FromArgb(0xF3, 0xF3, 0xF3)),
                            w.X, w.Y + TitleH - rad, w.W, rad);
            using (var pn = new Pen(Color.FromArgb(0xCF, 0xCF, 0xCF)))
                g.DrawPath(pn, RoundRectPath(w.X, w.Y, w.W, w.H, rad));
            g.DrawLine(new Pen(Color.FromArgb(0xD5, 0xD5, 0xD5)),
                       w.X, w.Y + TitleH, w.X + w.W - 1, w.Y + TitleH);

            // Accent dot + title.
            using (var b = new SolidBrush(Color.FromArgb(0x00, 0x78, 0xD4)))
                g.FillEllipse(b, w.X + 12, w.Y + TitleH / 2 - 4, 8, 8);
            g.SmoothingMode = SmoothingMode.None;
            using (var b = new SolidBrush(active ? Color.FromArgb(0x1B, 0x1B, 0x1B)
                                                 : Color.FromArgb(0x77, 0x77, 0x77)))
                g.DrawString(Shell.Title(w.Id), chromeFont, b, w.X + 28, w.Y + 9);

            // Minimise / maximise / close, drawn as vector glyphs so they
            // read crisply at any DPI (no font dependency).
            for (int i = 0; i < 3; i++)
            {
                int bx = w.X + w.W - BtnW * (3 - i);
                bool hot = hoverBtnWin == w.Id && hoverBtnIdx == i;
                if (hot)
                    g.FillRectangle(
                        new SolidBrush(i == 2 ? Color.FromArgb(0xC4, 0x2B, 0x1C)
                                              : Color.FromArgb(0xE8, 0xE8, 0xE8)),
                        bx, w.Y + 1, BtnW, TitleH - 2);
                int cx = bx + BtnW / 2, cy = w.Y + TitleH / 2;
                using (var pen = new Pen(i == 2 && hot ? Color.White
                                                      : Color.FromArgb(0x1B, 0x1B, 0x1B)))
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

        // ---- native WebBrowser overlay (Browser app only) --------------
        // The shared BrowserApp draws a text fallback inside the MiniCLR VM;
        // on Windows we host the genuine WebBrowser control (plus a real
        // address box and Go button) so pages render fully.  The managed
        // Shell.Paint still runs for the window, but these child controls
        // sit on top of the client area and own all input there.
        void SyncBrowser()
        {
            WinRec b = null;
            foreach (var w in wins)
                if (!w.Minimized && w.Kind == Kind.Browser) { b = w; break; }

            if (b == null) { CleanupBrowser(); return; }

            Rectangle cr = ClientRectOf(b);
            int barH = 34, goW = 56;
            if (browserCtl == null)
            {
                browserUrl = new TextBox
                {
                    Left = cr.X + 8, Top = cr.Y + 8,
                    Width = cr.Width - goW - 16, Height = 23,
                    Text = "http://example.com/",
                    Font = chromeFont,
                };
                browserGo = new Button
                {
                    Left = cr.X + cr.Width - goW - 4, Top = cr.Y + 8,
                    Width = goW, Height = 23, Text = "Go",
                };
                browserGo.Click += (s, e2) => NavigateBrowser();
                browserCtl = new System.Windows.Forms.WebBrowser
                {
                    ScriptErrorsSuppressed = true,
                    Location = new Point(cr.X, cr.Y + barH + 4),
                    Size = new Size(cr.Width, cr.Height - barH - 4),
                };
                // Start on a blank page so the client area is white instead
                // of the uninitialized black background that shows through
                // while no document is loaded.
                browserCtl.Navigate("about:blank");
                Controls.Add(browserCtl);
                Controls.Add(browserUrl);
                Controls.Add(browserGo);
            }
            browserWinId = b.Id;
            browserUrl.SetBounds(cr.X + 8, cr.Y + 8, cr.Width - goW - 16, 23);
            browserGo.SetBounds(cr.X + cr.Width - goW - 4, cr.Y + 8, goW, 23);
            browserCtl.SetBounds(cr.X, cr.Y + barH + 4, cr.Width, cr.Height - barH - 4);
            browserUrl.Visible = browserGo.Visible = browserCtl.Visible = true;
        }

        void NavigateBrowser()
        {
            if (browserCtl == null || browserUrl == null) return;
            string u = browserUrl.Text;
            if (u == null || u.Length == 0) return;
            if (!u.StartsWith("http://") && !u.StartsWith("https://")) u = "http://" + u;
            try { browserCtl.Navigate(u); }
            catch { /* offline / bad URL - ignore */ }
        }

        void CleanupBrowser()
        {
            if (browserCtl != null) { Controls.Remove(browserCtl); browserCtl.Dispose(); browserCtl = null; }
            if (browserUrl != null) { Controls.Remove(browserUrl); browserUrl.Dispose(); browserUrl = null; }
            if (browserGo != null) { Controls.Remove(browserGo); browserGo.Dispose(); browserGo = null; }
            browserWinId = -1;
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
        internal void OpenKind(int kind) { LaunchOrFocus(kind); }

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

        protected override void OnMouseDown(MouseEventArgs e)
        {
            int W = ClientSize.Width, H = ClientSize.Height;
            Gfx.SetScreen(W, H);
            Gfx.SetMouse(e.X, e.Y);

            // Right button -> contextual menus (desktop / taskbar / file).
            if (e.Button == MouseButtons.Right)
            {
                ShowContextMenu(e.X, e.Y, W, H);
                return;
            }

            // Left-click on a tray button is owned by the host (popups).
            if (e.Y >= H - TaskH)
            {
                int tray = Desktop.TrayHit(e.X, e.Y, W, H);
                if (tray >= 0) { ShowTrayPopup(tray, e.X, e.Y); return; }
            }

            // 1. The Start menu is modal, and 2. the taskbar floats above
            //    every window - both go straight to the shell.
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
                // The Browser app hosts a real WebBrowser + address box as
                // child controls; the form's click handler must not also
                // forward to the (hidden) managed control for the client area.
                if (hit.Kind == Kind.Browser && e.Y >= hit.Y + TitleH)
                {
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
                Gfx.SetContext(CreateGraphicsSafe(), cr.X, cr.Y, cr.Width, cr.Height);
                Shell.Click(hit.Id, e.X - cr.X, e.Y - cr.Y);
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
        }

        protected override void OnMouseUp(MouseEventArgs e) { drag = null; }

        protected override void OnKeyPress(KeyPressEventArgs e)
        {
            if (wins.Count == 0) return;
            var top = wins[wins.Count - 1];
            Shell.Key(top.Id, e.KeyChar);
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
            if (e.KeyCode == Keys.Back) { Shell.Key(top.Id, 8); Invalidate(); }
            else if (e.KeyCode == Keys.Enter) { Shell.Key(top.Id, 13); Invalidate(); }
            else if (e.KeyCode == Keys.Escape) { Shell.Key(top.Id, 27); Invalidate(); }
            else if (e.KeyCode == Keys.W && e.Control)
            {
                Shell.Close(top.Id); wins.Remove(top); Invalidate();
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                if (components != null) components.Dispose();
                if (scratchDc != null) scratchDc.Dispose();
                CleanupBrowser();
            }
            base.Dispose(disposing);
        }

        // =============================================================
        //  Context menus (right-click) and tray popups (left-click)
        // =============================================================
        void ShowAt(ToolStripDropDown m, int x, int y) { m.Show(this, new Point(x, y)); }

        void ShowContextMenu(int x, int y, int W, int H)
        {
            // 1. System tray buttons -> tray popup (tasks / voice / network).
            int tray = Desktop.TrayHit(x, y, W, H);
            if (tray >= 0) { ShowTrayPopup(tray, x, y); return; }

            // 2. Taskbar strip -> a right-click on a running pin offers
            //    Close window / End process; empty bar keeps the generic
            //    Task Manager / Taskbar settings menu.
            if (y >= H - TaskH)
            {
                int pin = Desktop.TaskbarButtonAt(x, y, W);
                WinRec pw = null!;
                if (pin >= 0)
                    foreach (var w in wins)
                        if (w.Kind == pin) { pw = w; break; }
                if (pw != null && !pw.Minimized)
                {
                    int pid = pw.Id;
                    var m = new ContextMenuStrip();
                    m.Items.Add("Close window", null, (s, e2) =>
                        { Shell.Close(pid); wins.Remove(pw); Invalidate(); });
                    m.Items.Add("End process", null, (s, e2) =>
                        { Shell.Close(pid); wins.Remove(pw); Invalidate(); });
                    ShowAt(m, x, y);
                    return;
                }
                var tb = new ContextMenuStrip();
                tb.Items.Add("Task Manager", null, (s, e2) => LaunchOrFocus(Kind.TaskManager));
                tb.Items.Add("Taskbar settings", null, (s, e2) => Shell.OpenSettings(7));
                ShowAt(tb, x, y);
                return;
            }

            // 3. A window under the cursor: File Explorer with a selected
            //    file gets the file menu; every other window gets the
            //    generic Refresh / Close window menu.
            var hit = HitWindow(x, y);
            if (hit != null)
            {
                Rectangle cr = ClientRectOf(hit);
                bool inClient = x >= cr.X && x < cr.X + cr.Width &&
                                y >= cr.Y && y < cr.Y + cr.Height;
                if (hit.Kind == Kind.FileExplorer && inClient)
                {
                    string fn = Shell.FileContext(hit.Id);
                    if (fn != null && fn != "")
                    {
                        ShowFileMenu(fn, Shell.FileContextFs(hit.Id),
                                    Shell.FileContextDir(hit.Id) != 0, x, y);
                        return;
                    }
                }
                int hid = hit.Id;
                var wm = new ContextMenuStrip();
                wm.Items.Add("Refresh", null, (s, e2) => Shell.WinAction(hid, WAct.Refresh));
                wm.Items.Add("Close window", null, (s, e2) =>
                    { Shell.Close(hid); wins.Remove(hit); Invalidate(); });
                ShowAt(wm, x, y);
                return;
            }

            // 4. Desktop icon -> launch menu.  The icons are virtual
            //    (SyncFromFs falls back to defaults in WinHost), so the
            //    menu offers Open + shell actions, not file surgery.
            int icon = Desktop.IconAt(x, y);
            if (icon >= 0)
            {
                string iname = Desktop.DesktopIconName(icon);
                if (iname != "")
                {
                    ShowDesktopIconMenu(icon, iname, x, y);
                    return;
                }
            }

            // 5. Bare desktop.
            ShowDesktopMenu(x, y);
        }

        void ShowDesktopIconMenu(int idx, string name, int x, int y)
        {
            int kind = Desktop.DesktopKind(idx);
            var m = new ContextMenuStrip();
            if (kind >= 0)
                m.Items.Add("Open", null, (s, e2) => LaunchOrFocus(kind));
            m.Items.Add("New folder", null, (s, e2) => NewFolder());
            m.Items.Add("Refresh", null, (s, e2) => { Desktop.Refresh(); Invalidate(); });
            ShowAt(m, x, y);
        }

        void ShowDesktopMenu(int x, int y)
        {
            var m = new ContextMenuStrip();
            var view = new ToolStripMenuItem("View");
            var sort = new ToolStripMenuItem("Sort by");
            string[] sorts = { "Name", "Size", "Type", "Date modified" };
            for (int i = 0; i < 4; i++)
            {
                int mode = i;
                var it = new ToolStripMenuItem(sorts[i]);
                if (Desktop.GetSortMode() == i) it.Checked = true;
                it.Click += (s, e2) => { Desktop.SortBy(mode); Invalidate(); };
                sort.DropDownItems.Add(it);
            }
            view.DropDownItems.Add(sort);
            view.DropDownItems.Add(new ToolStripMenuItem("Refresh", null,
                (s, e2) => { Desktop.Refresh(); Invalidate(); }));
            m.Items.Add(view);
            m.Items.Add(new ToolStripMenuItem("Refresh", null,
                (s, e2) => { Desktop.Refresh(); Invalidate(); }));
            m.Items.Add(new ToolStripMenuItem("Personalize", null,
                (s, e2) => Shell.OpenSettings(6)));
            m.Items.Add(new ToolStripMenuItem("Open in terminal", null,
                (s, e2) => LaunchOrFocus(Kind.Terminal)));
            m.Items.Add(new ToolStripMenuItem("New folder", null,
                (s, e2) => NewFolder()));
            ShowAt(m, x, y);
        }

        void ShowFileMenu(string fn, int fs, bool isDir, int x, int y)
        {
            var m = new ContextMenuStrip();
            m.Items.Add("Open", null, (s, e2) => OpenFile(fn, fs, isDir));
            var ow = new ToolStripMenuItem("Open with");
            ow.DropDownItems.Add("Notepad", null, (s, e2) => Shell.OpenNotepad(fn));
            ow.DropDownItems.Add("Terminal (folder)", null, (s, e2) => LaunchOrFocus(Kind.Terminal));
            m.Items.Add(ow);
            m.Items.Add("Copy", null, (s, e2) => { clipboardPath = PathFor(fs, fn); });
            m.Items.Add("Delete", null, (s, e2) => DeleteFile(fn, fs));
            m.Items.Add("Rename", null, (s, e2) => RenameFile(fn, fs));
            m.Items.Add("Properties", null, (s, e2) => ShowFileProps(fn, fs, isDir));
            ShowAt(m, x, y);
        }

        void ShowTrayPopup(int which, int x, int y)
        {
            if (which == 0) ShowTasksPopup(x, y);
            else if (which == 1) ShowVoicePopup(x, y);
            else ShowNetworkPopup(x, y);
        }

        void ShowTasksPopup(int x, int y)
        {
            var m = new ContextMenuStrip();
            if (wins.Count == 0) m.Items.Add("(no running apps)");
            for (int i = wins.Count - 1; i >= 0; i--)
            {
                var w = wins[i];
                int id = w.Id;
                m.Items.Add(Shell.Title(id), null, (s, e2) =>
                {
                    w.Minimized = false; Raise(w); Invalidate();
                });
            }
            ShowAt(m, x, y);
        }

        void ShowVoicePopup(int x, int y)
        {
            var m = new ContextMenuStrip();
            var t = new ToolStripMenuItem("Voice input: " + (Theme.VoiceOn != 0 ? "On" : "Off"));
            t.Click += (s, e2) =>
            {
                Theme.VoiceOn = Theme.VoiceOn != 0 ? 0 : 1;
                Desktop.SetVoice(Theme.VoiceOn); Invalidate();
            };
            m.Items.Add(t);
            m.Items.Add("Microphone settings", null, (s, e2) => Shell.OpenSettings(2));
            ShowAt(m, x, y);
        }

        void ShowNetworkPopup(int x, int y)
        {
            var m = new ContextMenuStrip();
            var eth = new ToolStripMenuItem("Ethernet" + (Theme.ActiveNet == 0 ? "  (active)" : ""));
            eth.Click += (s, e2) => { Theme.ActiveNet = 0; Invalidate(); };
            var wifi = new ToolStripMenuItem("Wi-Fi" + (Theme.ActiveNet == 1 ? "  (active)" : ""));
            wifi.Click += (s, e2) => { Theme.ActiveNet = 1; Invalidate(); };
            m.Items.Add(eth);
            m.Items.Add(wifi);
            m.Items.Add(new ToolStripSeparator());
            m.Items.Add("Network settings", null, (s, e2) => Shell.OpenSettings(3));
            ShowAt(m, x, y);
        }

        // ---- file operations against the sandbox -----------------------
        string PathFor(int fs, string name)
        {
            string sub = fs == 1 ? "sfs" : "mkfs";
            return Path.Combine(Host.FsRoot, sub, name);
        }

        void OpenFile(string fn, int fs, bool isDir)
        {
            if (isDir) LaunchOrFocus(Kind.FileExplorer);
            else Shell.OpenNotepad(fn);
        }

        void DeleteFile(string fn, int fs)
        {
            try
            {
                string p = PathFor(fs, fn);
                if (File.Exists(p)) File.Delete(p);
                else if (Directory.Exists(p)) Directory.Delete(p, true);
                Host.FileRefresh(); Invalidate();
            }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Delete failed"); }
        }

        void RenameFile(string fn, int fs)
        {
            using (var dlg = new InputDialog("Rename", "New name:", fn))
                if (dlg.ShowDialog(this) == DialogResult.OK)
                {
                    string np = dlg.TextValue;
                    if (np != "" && np != fn)
                        try
                        {
                            string p = PathFor(fs, fn), d = PathFor(fs, np);
                            if (File.Exists(p)) File.Move(p, d);
                            else if (Directory.Exists(p)) Directory.Move(p, d);
                            Host.FileRefresh(); Invalidate();
                        }
                        catch (Exception ex) { MessageBox.Show(this, ex.Message, "Rename failed"); }
                }
        }

        void ShowFileProps(string fn, int fs, bool isDir)
        {
            string p = PathFor(fs, fn);
            long size = 0; bool exists = false;
            if (File.Exists(p)) { size = new FileInfo(p).Length; exists = true; }
            else if (Directory.Exists(p)) exists = true;
            string info = "Name:    " + fn + "\n" +
                          "Type:    " + (isDir ? "Folder" : "File") + "\n" +
                          "Volume:  " + (fs == 1 ? "System (SFS)" : "Local (MKFS)") + "\n" +
                          "Size:    " + (size / 1024) + " KB\n" +
                          "Exists:  " + (exists ? "yes" : "no");
            MessageBox.Show(this, info, "Properties - " + fn);
        }

        void NewFolder()
        {
            string baseDir = Path.Combine(Host.FsRoot, "mkfs");
            string name = "New folder";
            string p = Path.Combine(baseDir, name);
            int n = 1;
            while (Directory.Exists(p)) { p = Path.Combine(baseDir, name + " (" + n + ")"); n++; }
            try { Directory.CreateDirectory(p); Host.FileRefresh(); Invalidate(); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "New folder failed"); }
        }

        // A tiny modal text-entry dialog (used by Rename).
        sealed class InputDialog : Form
        {
            readonly TextBox box = new TextBox();
            public string TextValue { get { return box.Text; } }
            public InputDialog(string title, string label, string initial)
            {
                Text = title;
                ClientSize = new Size(320, 110);
                StartPosition = FormStartPosition.CenterParent;
                var lbl = new Label { Left = 12, Top = 12, Text = label, AutoSize = true };
                box.Left = 12; box.Top = 36; box.Width = 296; box.Text = initial;
                var ok = new Button { Left = 148, Top = 70, Width = 80, Text = "OK", DialogResult = DialogResult.OK };
                var cancel = new Button { Left = 236, Top = 70, Width = 80, Text = "Cancel", DialogResult = DialogResult.Cancel };
                Controls.AddRange(new Control[] { lbl, box, ok, cancel });
                AcceptButton = ok; CancelButton = cancel;
            }
        }
    }
}
