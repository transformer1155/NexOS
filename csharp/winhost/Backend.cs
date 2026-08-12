// =====================================================================
//  Backend.cs  -  Windows implementations of the MiniCLR internal calls
// ---------------------------------------------------------------------
//  Inside NexOS, NexOS.Forms.Gfx / NexOS.Forms.Host / NexOS.Sys are
//  bodyless [InternalCall] declarations that mforms.cpp binds to native
//  kernel routines.  Here we supply real managed implementations with
//  byte-identical signatures, backed by GDI+ and the Windows BCL, so the
//  *unmodified* shell sources (Shell.cs / Desktop.cs / Apps.cs / the
//  NexOS.Forms toolkit) run directly on .NET.
//
//  Forms.cs and Sys.cs guard their internal-call declarations with
//  "#if !WINHOST"; this project defines WINHOST, so these classes take
//  their place at compile time.  Nothing is copied or forked.
//
//  Semantics deliberately mirror mforms.cpp:
//    * all coordinates are client-relative; SetContext() installs the
//      window origin as a transform and the client rect as a clip;
//    * Measure() reproduces mh_measure() exactly (8px per ASCII glyph,
//      16px per CJK glyph) so every layout calculation in the shell
//      lands on the same pixel it does inside the VM;
//    * MouseX/MouseY return -1 when the pointer is off-surface, and are
//      otherwise relative to the current context origin.
// =====================================================================
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Net.Http;
using System.Text;

namespace NexOS.Forms
{
    // -----------------------------------------------------------------
    //  Gfx  --  GDI+ drawing surface
    // -----------------------------------------------------------------
    public static class Gfx
    {
        // ---- context state (mirrors mforms.cpp g_ox/g_oy/g_cw/g_ch) ---
        static Graphics g;
        static int ox, oy, cw, ch;
        static int screenW = 1280, screenH = 720;
        static int msx = -1, msy = -1;          // pointer, screen space

        static Font font;
        static StringFormat sf;
        static readonly SolidBrush brush = new SolidBrush(Color.Black);
        static readonly Pen pen = new Pen(Color.Black, 1f);

        internal static void SetScreen(int w, int h) { screenW = w; screenH = h; }
        internal static void SetMouse(int x, int y) { msx = x; msy = y; }

        // Install the drawing target and the client rectangle.  Clip is
        // set while the transform is identity (device space), then the
        // origin is applied, so client (0,0) maps to (ox,oy) and nothing
        // can bleed outside the window.
        internal static void SetContext(Graphics gr, int x, int y, int w, int h)
        {
            g = gr; ox = x; oy = y; cw = w; ch = h;
            g.ResetTransform();
            g.SetClip(new Rectangle(x, y, w, h));
            g.TranslateTransform(x, y);
            EnsureFont(gr);
        }

        // The kernel font is a fixed 8x16 cell.  Rather than hard-code a
        // point size (which drifts across DPI settings), calibrate once:
        // pick the pixel size at which Consolas advances exactly 8px.
        static void EnsureFont(Graphics gr)
        {
            if (font != null) return;
            sf = (StringFormat)StringFormat.GenericTypographic.Clone();
            sf.FormatFlags |= StringFormatFlags.NoWrap | StringFormatFlags.MeasureTrailingSpaces;

            string probe = new string('M', 20);
            string[] candidates = { "Consolas", "Cascadia Mono", "Lucida Console", "Courier New" };
            foreach (string name in candidates)
            {
                Font f16 = null;
                try { f16 = new Font(name, 16f, FontStyle.Regular, GraphicsUnit.Pixel); }
                catch { continue; }
                if (!f16.Name.StartsWith(name.Split(' ')[0], StringComparison.OrdinalIgnoreCase))
                { f16.Dispose(); continue; }         // silent substitution -> skip

                float adv16 = gr.MeasureString(probe, f16, PointF.Empty, sf).Width / 20f;
                f16.Dispose();
                if (adv16 <= 0.01f) continue;
                float px = 16f * 8f / adv16;         // scale so advance == 8px
                font = new Font(name, px, FontStyle.Regular, GraphicsUnit.Pixel);
                return;
            }
            font = new Font(FontFamily.GenericMonospace, 13f, GraphicsUnit.Pixel);
        }

        // Alpha-aware: pack as 0xAARRGGBB so translucent (acrylic / glass)
        // colours work on the host while opaque 0xRRGGBB stays opaque.
        static Color Col(uint c)
        {
            int a = (int)((c >> 24) & 0xFF);
            if (a == 0) a = 255;
            return Color.FromArgb(a,
                (int)((c >> 16) & 0xFF),
                (int)((c >> 8) & 0xFF),
                (int)(c & 0xFF));
        }
        static Brush B(uint c) { brush.Color = Col(c); return brush; }
        static Pen P(uint c) { pen.Color = Col(c); return pen; }

        // Lighten (d>0) / darken (d<0) a packed 0xRRGGBB colour.
        static uint Shade(uint c, int d)
        {
            int r = (int)((c >> 16) & 0xFF) + d;
            int g = (int)((c >> 8) & 0xFF) + d;
            int b = (int)(c & 0xFF) + d;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            return ((uint)r << 16) | ((uint)g << 8) | (uint)b;
        }

        static GraphicsPath Round(int x, int y, int w, int h, int r)
        {
            if (r * 2 > w) r = w / 2;
            if (r * 2 > h) r = h / 2;
            var p = new GraphicsPath();
            if (r <= 0) { p.AddRectangle(new Rectangle(x, y, w, h)); return p; }
            int d = r * 2;
            p.AddArc(x, y, d, d, 180, 90);
            p.AddArc(x + w - d - 1, y, d, d, 270, 90);
            p.AddArc(x + w - d - 1, y + h - d - 1, d, d, 0, 90);
            p.AddArc(x, y + h - d - 1, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        // ---- primitives ------------------------------------------------
        public static void FillRect(int x, int y, int w, int h, uint c)
        {
            if (w <= 0 || h <= 0) return;
            g.SmoothingMode = SmoothingMode.None;
            g.FillRectangle(B(c), x, y, w, h);
        }

        public static void FillRound(int x, int y, int w, int h, int r, uint c)
        {
            if (w <= 0 || h <= 0) return;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = Round(x, y, w, h, r)) g.FillPath(B(c), p);
        }

        public static void DrawRound(int x, int y, int w, int h, int r, uint c)
        {
            if (w <= 0 || h <= 0) return;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = Round(x, y, w, h, r)) g.DrawPath(P(c), p);
        }

        public static void DrawRect(int x, int y, int w, int h, uint c)
        {
            if (w <= 0 || h <= 0) return;
            g.SmoothingMode = SmoothingMode.None;
            g.DrawRectangle(P(c), x, y, w - 1, h - 1);
        }

        public static void DrawLine(int x0, int y0, int x1, int y1, uint c)
        {
            g.SmoothingMode = (x0 == x1 || y0 == y1) ? SmoothingMode.None : SmoothingMode.AntiAlias;
            g.DrawLine(P(c), x0, y0, x1, y1);
        }

        public static void Gradient(int x, int y, int w, int h, uint top, uint bot)
        {
            if (w <= 0 || h <= 0) return;
            g.SmoothingMode = SmoothingMode.None;
            // +1 height avoids the GDI+ wrap artefact on the last scanline.
            using (var lb = new LinearGradientBrush(new Rectangle(x, y, w, h + 1),
                                                    Col(top), Col(bot), 90f))
                g.FillRectangle(lb, x, y, w, h);
        }

        public static void Text(int x, int y, string s, uint fg)
        {
            if (string.IsNullOrEmpty(s)) return;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            g.DrawString(s, font, B(fg), x - 1, y + 1, sf);
        }

        public static void TextBg(int x, int y, string s, uint fg, uint bg)
        {
            if (string.IsNullOrEmpty(s)) return;
            FillRect(x, y, Measure(s), 16, bg);
            Text(x, y, s, fg);
        }

        public static void TextCenter(int x, int y, int w, string s, uint fg)
        {
            if (string.IsNullOrEmpty(s)) return;
            int tw = Measure(s);
            int sx = x + (w - tw) / 2;
            if (sx < x) sx = x;
            Text(sx, y, s, fg);
        }

        public static void FillCircle(int cx, int cy, int r, uint c)
        {
            if (r <= 0) return;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.FillEllipse(B(c), cx - r, cy - r, r * 2, r * 2);
        }

        public static void DrawCircle(int cx, int cy, int r, uint c)
        {
            if (r <= 0) return;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.DrawEllipse(P(c), cx - r, cy - r, r * 2, r * 2);
        }

        // Rounded tile with a single centred glyph - the shell's app icon.
        // Upgraded on the host to a Fluent tile: soft contact shadow, a
        // top-lit gradient fill and a 1px inner highlight along the top.
        public static void Icon(int x, int y, int sz, uint bg, int letter, uint lc)
        {
            if (sz <= 0) return;
            int r = sz / 4;
            // Contact shadow for depth.
            using (var sp = Round(x + 1, y + 2, sz, sz, r))
                g.FillPath(B(0x2E000000), sp);
            // Base tile with a top-light gradient.
            using (var lb = new LinearGradientBrush(
                       new Rectangle(x, y, sz, sz),
                       Col(Shade(bg, 20)), Col(Shade(bg, -12)), 90f))
                g.FillPath(lb, Round(x, y, sz, sz, r));
            // 1px top highlight.
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var hp = Round(x + 1, y + 1, sz - 2, sz - 2, r - 1))
                g.DrawPath(new Pen(Color.FromArgb(60, 255, 255, 255)), hp);
            if (letter <= 0) return;
            string s = ((char)letter).ToString();
            int tw = Measure(s);
            Text(x + (sz - tw) / 2, y + (sz - 16) / 2, s, lc);
        }

        // ---- bitmap assets (UI resources produced by the drawing plugin) -
        // WinHost-only; the shared Gfx (MiniCLR) does not declare these, so
        // no VM binding is needed.  Keys map to logical resources.
        static readonly System.Collections.Generic.Dictionary<int, Bitmap> images =
            new System.Collections.Generic.Dictionary<int, Bitmap>();
        public static void LoadImage(int id, string path)
        {
            try { images[id] = new Bitmap(path); } catch { }
        }
        // int (0/1), matching the MiniCLR internal call (no bool icalls).
        public static int HasImage(int id) { return images.ContainsKey(id) ? 1 : 0; }
        public static void Image(int id, int x, int y, int w, int h)
        {
            if (!images.TryGetValue(id, out Bitmap bmp) || bmp == null) return;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.DrawImage(bmp, new Rectangle(x, y, w, h));
        }

        public static void Progress(int x, int y, int w, int h, int pct, uint c)
        {
            if (w <= 0 || h <= 0) return;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            FillRound(x, y, w, h, h / 2, 0xE3E3E3);
            int fw = w * pct / 100;
            if (fw > 0) FillRound(x, y, fw < h ? h : fw, h, h / 2, c);
        }

        // Byte-for-byte equivalent of mh_measure() in gui.cpp: ASCII
        // advances 8px, a CJK codepoint (3 UTF-8 bytes) advances 16px,
        // 2-byte sequences carry no glyph in the kernel font.
        public static int Measure(string s)
        {
            if (string.IsNullOrEmpty(s)) return 0;
            int w = 0;
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (c < 0x80) w += 8;
                else if (c < 0x800) { /* no glyph */ }
                else w += 16;
            }
            return w;
        }

        public static int Width() { return cw; }
        public static int Height() { return ch; }
        public static int ScreenW() { return 1280; }
        public static int ScreenH() { return 720; }
        public static int MouseX() { return msx < 0 ? -1 : msx - ox; }
        public static int MouseY() { return msy < 0 ? -1 : msy - oy; }
    }

    // -----------------------------------------------------------------
    //  Host  --  machine state, mapped onto the Windows box we run on
    // -----------------------------------------------------------------
    public static class Host
    {
        internal static int Running;                 // taskbar running bits
        internal static Action ShutdownHook;
        internal static Action RebootHook;
        internal static Action<int> CloseAppHook;    // close windows of a Kind
        internal static Action ExitGuiHook;          // leave GUI mode
        internal static string FsRoot = "";

        static string[][] listing = new string[2][];
        static bool[][] isdir = new bool[2][];
        static readonly int startTick = Environment.TickCount;

        // ---- memory ---------------------------------------------------
        // A synthetic 128 MB machine keeps the meters in the same range
        // the VM shows, so layouts are exercised with realistic numbers.
        const int TotalKb = 128 * 1024;

        public static int MemTotalKb() { return TotalKb; }
        public static int PagesTotal() { return TotalKb / 4; }
        public static int PagesUsed()
        {
            long used = GC.GetTotalMemory(false) / 4096;
            int cap = PagesTotal() - 16;
            return (int)(used > cap ? cap : used) + 2048;
        }
        public static int PagesFree() { return PagesTotal() - PagesUsed(); }
        public static int HeapAlloc() { return (int)(GC.GetTotalAllocatedBytes(false) & 0x3FFFFFFF); }
        public static int HeapFree() { return (int)((GC.GetTotalAllocatedBytes(false) - GC.GetTotalMemory(false)) & 0x3FFFFFFF); }
        public static int HeapAllocCnt() { return GC.CollectionCount(0) * 137 + 42; }
        public static int HeapFreeCnt() { return GC.CollectionCount(0) * 129 + 30; }
        public static void Optimize() { GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); }

        // ---- clock ----------------------------------------------------
        public static int Hour() { return DateTime.Now.Hour; }
        public static int Minute() { return DateTime.Now.Minute; }
        public static int Second() { return DateTime.Now.Second; }
        public static int Ticks() { return Environment.TickCount - startTick; }
        public static int TickMs() { return Environment.TickCount - startTick; }

        // ---- identity -------------------------------------------------
        public static string OsName() { return "NexOS (WinForms host)"; }

        public static string CpuVendor()
        {
            string v = Environment.GetEnvironmentVariable("PROCESSOR_IDENTIFIER");
            if (string.IsNullOrEmpty(v)) return "x86 CPU";
            if (v.Length > 40) v = v.Substring(0, 40);
            return v;
        }

        public static string DiskModel()
        {
            try
            {
                var d = new DriveInfo(Path.GetPathRoot(FsRoot.Length > 0 ? FsRoot : AppContext.BaseDirectory));
                return d.VolumeLabel.Length > 0 ? d.VolumeLabel : ("Volume " + d.Name);
            }
            catch { return "Host Disk"; }
        }

        public static int DiskSizeMb()
        {
            try
            {
                var d = new DriveInfo(Path.GetPathRoot(FsRoot.Length > 0 ? FsRoot : AppContext.BaseDirectory));
                return (int)(d.TotalSize / (1024 * 1024));
            }
            catch { return 0; }
        }

        public static int Is64Bit() { return Environment.Is64BitProcess ? 1 : 0; }
        public static int PciCount() { return 12; }

        public static int NicPresent()
        {
            try
            {
                return System.Net.NetworkInformation.NetworkInterface.GetIsNetworkAvailable() ? 1 : 0;
            }
            catch { return 0; }
        }

        public static int RunningMask() { return Running; }

        // ---- file system (fs: 0 = MKFS user disk, 1 = SFS system) -----
        static string Dir(int fs)
        {
            string sub = fs == 1 ? "sfs" : "mkfs";
            return Path.Combine(FsRoot, sub);
        }

        static void Load(int fs)
        {
            if (fs < 0 || fs > 1) return;
            if (listing[fs] != null) return;
            try
            {
                var dir = new DirectoryInfo(Dir(fs));
                var subs = dir.GetDirectories();
                var files = dir.GetFiles();
                var names = new string[subs.Length + files.Length];
                var dirs = new bool[names.Length];
                int k = 0;
                foreach (var s in subs) { names[k] = s.Name; dirs[k] = true; k++; }
                foreach (var f in files) { names[k] = f.Name; dirs[k] = false; k++; }
                listing[fs] = names; isdir[fs] = dirs;
            }
            catch { listing[fs] = new string[0]; isdir[fs] = new bool[0]; }
        }

        public static int FileCount(int fs)
        {
            if (fs < 0 || fs > 1) return 0;
            Load(fs);
            return listing[fs].Length;
        }

        public static string FileName(int fs, int idx)
        {
            if (fs < 0 || fs > 1) return "";
            Load(fs);
            if (idx < 0 || idx >= listing[fs].Length) return "";
            return listing[fs][idx];
        }

        public static int FileIsDir(int fs, int idx)
        {
            if (fs < 0 || fs > 1) return 0;
            Load(fs);
            if (idx < 0 || idx >= isdir[fs].Length) return 0;
            return isdir[fs][idx] ? 1 : 0;
        }

        public static int FileRefresh() { listing[0] = null; listing[1] = null; return 1; }

        // File-system mutations (context-menu: new folder / delete / rename).
        // Backed by the real Windows directories under FsRoot, so the same
        // managed code path works on both the kernel and WinHost.
        public static int FileMkDir(int fs, string name)
        {
            if (fs < 0 || fs > 1 || string.IsNullOrEmpty(name)) return -1;
            try { Directory.CreateDirectory(Path.Combine(Dir(fs), name)); FileRefresh(); return 1; }
            catch { return -1; }
        }

        public static int FileDelete(int fs, string name)
        {
            if (fs < 0 || fs > 1 || string.IsNullOrEmpty(name)) return -1;
            try
            {
                string p = Path.Combine(Dir(fs), name);
                if (Directory.Exists(p)) Directory.Delete(p, true);
                else if (File.Exists(p)) File.Delete(p);
                else return -1;
                FileRefresh();
                return 1;
            }
            catch { return -1; }
        }

        public static int FileRename(int fs, string oldName, string newName)
        {
            if (fs < 0 || fs > 1 || string.IsNullOrEmpty(oldName) || string.IsNullOrEmpty(newName)) return -1;
            try
            {
                string o = Path.Combine(Dir(fs), oldName);
                string n = Path.Combine(Dir(fs), newName);
                if (Directory.Exists(o)) Directory.Move(o, n);
                else if (File.Exists(o)) File.Move(o, n);
                else return -1;
                FileRefresh();
                return 1;
            }
            catch { return -1; }
        }

        // Real Windows HTTP GET for the Browser control (WINHOST build).
        // Uses the genuine .NET HttpClient stack rather than a text stub.
        private static HttpClient _http;
        private static HttpClient Http() {
            if (_http == null) { _http = new HttpClient(); _http.Timeout = TimeSpan.FromSeconds(8); }
            return _http;
        }
        public static string HttpGet(string url)
        {
            if (string.IsNullOrEmpty(url)) return "";
            try
            {
                string u = url;
                if (!u.StartsWith("http://", StringComparison.OrdinalIgnoreCase) &&
                    !u.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                    u = "http://" + u;
                string r = Http().GetStringAsync(u).GetAwaiter().GetResult();
                return r == null ? "" : r;
            }
            catch { return ""; }
        }

        public static string ReadText(int fs, string name)
        {
            if (fs < 0 || fs > 1 || string.IsNullOrEmpty(name)) return "";
            try
            {
                string p = Path.Combine(Dir(fs), Path.GetFileName(name));
                if (!File.Exists(p)) return "";
                string t = File.ReadAllText(p);
                return t.Length > 4096 ? t.Substring(0, 4096) : t;
            }
            catch { return ""; }
        }

        // ---- terminal -------------------------------------------------
        public static string Exec(string cmd)
        {
            if (string.IsNullOrEmpty(cmd)) return "";
            string c = cmd.Trim();
            string low = c.ToLowerInvariant();

            if (low == "help")
                return "help ver mem date ls cat <f> echo <s> clear";
            if (low == "ver")
                return OsName() + " / .NET " + Environment.Version;
            if (low == "mem")
                return "total " + TotalKb / 1024 + " MB, used " + PagesUsed() * 4 / 1024 + " MB";
            if (low == "date")
                return DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss");
            if (low == "ls")
            {
                FileRefresh();
                var sb = new StringBuilder();
                int n = FileCount(1);
                for (int i = 0; i < n && i < 12; i++)
                {
                    if (i > 0) sb.Append(' ');
                    sb.Append(FileName(1, i));
                }
                return sb.Length == 0 ? "(empty)" : sb.ToString();
            }
            if (low.StartsWith("cat "))
            {
                string t = ReadText(1, c.Substring(4).Trim());
                if (t.Length == 0) return "cat: not found";
                t = t.Replace("\r", " ").Replace("\n", " ");
                return t.Length > 120 ? t.Substring(0, 120) : t;
            }
            if (low.StartsWith("echo "))
                return c.Substring(5);
            if (low == "clear")
                return "";
            return "unknown command: " + c;
        }

        // ---- power ----------------------------------------------------
        public static void Shutdown() { if (ShutdownHook != null) ShutdownHook(); }
        public static void Reboot() { if (RebootHook != null) RebootHook(); }

        // Managed code opens an app (Notepad from the File Explorer).
        public static void OpenApp(int kind) { Shell.Open(kind); }

        // Execute a native Windows PE image.  On the real kernel this runs
        // the .exe through the win32 / win64 PE loader; the Windows-hosted
        // preview shell has no such loader, so we report "no window made"
        // and let the caller fall back to its default handler.
        public static int RunExe(string name)
        {
            Console.WriteLine("[shell] RunExe(" + name + ") - no PE loader in the winhost preview");
            return -1;
        }

        // Close every window of a Kind / leave GUI mode (taskbar menus).
        public static void CloseApp(int kind) { if (CloseAppHook != null) CloseAppHook(kind); }
        public static void ExitGui() { if (ExitGuiHook != null) ExitGuiHook(); }

        public static void Log(string s) { Console.WriteLine("[shell] " + s); }

        public static string CharStr(int ch)
        {
            if (ch <= 0 || ch > 0xFFFF) return "";
            return ((char)ch).ToString();
        }

        // Shared clipboard for the WinForms host (mirrors the kernel one).
        private static string g_clipboard = "";
        public static string GetClipboard() { return g_clipboard ?? ""; }
        public static void SetClipboard(string s) { g_clipboard = s ?? ""; }
    }
}

namespace NexOS
{
    // -----------------------------------------------------------------
    //  Sys  --  the MiniCLR core internal calls, on the real BCL
    // -----------------------------------------------------------------
    public static class Sys
    {
        public static void Print(string s) { Console.Write(s); }
        public static void PrintInt(int v) { Console.Write(v); }
        public static void PrintChar(char c) { Console.Write(c); }

        public static string StrConcat(string a, string b) { return (a ?? "") + (b ?? ""); }
        public static char StrCharAt(string s, int index)
        {
            if (s == null || index < 0 || index >= s.Length) return '\0';
            return s[index];
        }
        public static int StrLen(string s) { return s == null ? 0 : s.Length; }
        public static bool StrEq(string a, string b) { return string.Equals(a, b, StringComparison.Ordinal); }
        public static string IntToStr(int v) { return v.ToString(System.Globalization.CultureInfo.InvariantCulture); }

        public static int TickCount() { return Environment.TickCount; }
    }
}
