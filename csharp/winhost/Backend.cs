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

        // Frosted-glass panel (WinHost parity: a Translucent rounded fill,
        // since GDI+ has no live backdrop blur).  Good enough for previews.
        public static void Glass(int x, int y, int w, int h, int r, uint tint, int alpha, int blur)
        {
            if (w <= 0 || h <= 0) return;
            int a = alpha; if (a < 0) a = 0; if (a > 255) a = 255;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var p = Round(x, y, w, h, r))
                g.FillPath(new SolidBrush(Color.FromArgb(a, (int)(tint & 0xFF),
                                    (int)((tint >> 8) & 0xFF), (int)((tint >> 16) & 0xFF))), p);
        }

        // Text at an explicit glyph height (scales the host font).
        public static void TextPx(int x, int y, string s, uint fg, int px)
        {
            if (string.IsNullOrEmpty(s)) return;
            int sz = px; if (sz < 4) sz = 4; if (sz > 128) sz = 128;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            using (var f = new Font(font.FontFamily, sz, font.Style))
                g.DrawString(s, f, B(fg), x - 1, y + 1, sf);
        }

        // Width of s at glyph height px (matches the kernel's vector advance).
        public static int MeasurePx(string s, int px)
        {
            if (string.IsNullOrEmpty(s)) return 0;
            int sz = px; if (sz < 4) sz = 4; if (sz > 128) sz = 128;
            using (var f = new Font(font.FontFamily, sz, font.Style))
            {
                var szf = g.MeasureString(s, f);
                return (int)(szf.Width + 0.5f);
            }
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
        public static int OriginX() { return ox; }
        public static int OriginY() { return oy; }
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
        internal static Action<int> OpenAppHook;     // create a real window for a Kind
        internal static Action<int, int> WinActionHook;  // window geometry action (id, WAct)
        internal static Action ExitGuiHook;          // leave GUI mode
        internal static Action RepaintHook;          // push the backbuffer now
        internal static Action<int> WindowTransparentHook; // whole-window opacity
        internal static string FsRoot = "";

        static string[][] listing = new string[4][];
        static bool[][] isdir = new bool[4][];
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
        // The VM has no network in QEMU, so it keeps the kernel RTC time
        // (which QEMU seeds from the host).  On the host we fetch the
        // accurate time from a server's HTTP Date header once and remember
        // the offset, calibrating the displayed clock against the network.
        static long timeOffsetMs;
        static bool timeSyncStarted;

        public static void StartTimeSync()
        {
            if (timeSyncStarted) return;
            timeSyncStarted = true;
            System.Threading.ThreadPool.QueueUserWorkItem(_ =>
            {
                try
                {
                    string u = "http://example.com/";
                    var resp = Http().GetAsync(u).GetAwaiter().GetResult();
                    var hdr = resp.Headers.Date;      // DateTimeOffset?
                    resp.Dispose();
                    if (hdr.HasValue)
                    {
                        timeOffsetMs = (hdr.Value.UtcTicks - DateTime.UtcNow.Ticks)
                                       / TimeSpan.TicksPerMillisecond;
                        Console.WriteLine("[time] network sync offset=" + timeOffsetMs + "ms");
                    }
                    else Console.WriteLine("[time] no Date header; keeping local clock");
                }
                catch (Exception ex)
                {
                    Console.WriteLine("[time] sync failed (keeping local): " + ex.Message);
                }
            });
        }

        public static int Hour()   { return DateTime.Now.AddMilliseconds(timeOffsetMs).Hour; }
        public static int Minute() { return DateTime.Now.AddMilliseconds(timeOffsetMs).Minute; }
        public static int Second() { return DateTime.Now.AddMilliseconds(timeOffsetMs).Second; }
        public static int Ticks() { return Environment.TickCount - startTick; }
        public static int TickMs() { return Environment.TickCount - startTick; }
        // WinForms host repaints on its own timer, so this is a no-op stub.
        public static void SetAnim(int on) { }

        // ---- identity -------------------------------------------------
        public static string OsName() { return "NexOS (WinForms host)"; }

        // Account database stubs for the WinForms host (the real kernel
        // keeps a hashed account table; here we seed a single "root"
        // account and accept it for any password so the demo flows).
        public static int UserCount() { return 1; }
        public static string UserName(int i) { return (i == 0) ? "root" : ""; }
        public static int LoginCheck(string user, string pass)
        {
            if (user == null) return -1;
            if (user == "root") return 0;     // accept the seeded account
            return -1;
        }

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

        // ---- file system (fs: 0 = MKFS user disk, 1 = SFS system,
        //      3 = desktop shortcuts inside MKFS/Desktop) -------------
        static string Dir(int fs)
        {
            if (fs == 3) return Path.Combine(FsRoot, "mkfs", "Desktop");
            string sub = fs == 1 ? "sfs" : "mkfs";
            return Path.Combine(FsRoot, sub);
        }

        // fs==2 is reserved (FAT32); anything above 3 is out of range.
        static bool FsOk(int fs) { return fs >= 0 && fs <= 3 && fs != 2; }

        // BUG #8 (SFS read-only penetration).  The SFS volume is a pre-built
        // boot image; nothing may be written to it.  The UI already hides the
        // mutating verbs, but the backend is the last line of defence, so it
        // refuses with the same EROFS code the kernel returns (-30) instead of
        // a generic -1.  Callers can tell "read-only medium" from "bad name".
        const int EROFS = -30;
        static bool FsWritable(int fs) { return fs == 0 || fs == 3; }

        // ---- directory-change watch (BUG #2: ghost new file) ------------
        // The managed shell caches a volume listing for one frame, so a file
        // created OUTSIDE the shell (installer, terminal, package install,
        // another process) would not show up until the user navigated away
        // and back.  A FileSystemWatcher on the writable volumes drops that
        // cache the moment the medium changes, so the next paint re-reads it.
        static readonly System.Collections.Generic.List<FileSystemWatcher> s_watchers
            = new System.Collections.Generic.List<FileSystemWatcher>();
        static bool s_watchers_on;

        static void EnsureWatchers()
        {
            if (s_watchers_on) return;
            s_watchers_on = true;
            try
            {
                string root = string.IsNullOrEmpty(FsRoot) ? AppContext.BaseDirectory : FsRoot;
                foreach (int v in new int[] { 0, 3 })
                {
                    string d = Path.Combine(root, v == 3 ? Path.Combine("mkfs", "Desktop") : "mkfs");
                    if (!System.IO.Directory.Exists(d)) continue;
                    var w = new FileSystemWatcher(d);
                    w.NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName
                                     | NotifyFilters.LastWrite | NotifyFilters.Size;
                    w.IncludeSubdirectories = (v == 3);
                    w.Changed += (s, e) => FileRefresh();
                    w.Created += (s, e) => FileRefresh();
                    w.Deleted += (s, e) => FileRefresh();
                    w.Renamed += (s, e) => FileRefresh();
                    w.EnableRaisingEvents = true;
                    s_watchers.Add(w);
                }
            }
            catch { }   // a watcher is an optimisation; never fatal
        }
        static void Load(int fs)
        {
            if (!FsOk(fs)) return;
            if (listing[fs] != null) return;
            try
            {
                var dir = new DirectoryInfo(Dir(fs));
                if (!dir.Exists) { listing[fs] = new string[0]; isdir[fs] = new bool[0]; return; }
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
            if (!FsOk(fs)) return 0;
            Load(fs);
            return listing[fs].Length;
        }

        public static string FileName(int fs, int idx)
        {
            if (!FsOk(fs)) return "";
            Load(fs);
            if (idx < 0 || idx >= listing[fs].Length) return "";
            return listing[fs][idx];
        }

        public static int FileIsDir(int fs, int idx)
        {
            if (!FsOk(fs)) return 0;
            Load(fs);
            if (idx < 0 || idx >= isdir[fs].Length) return 0;
            return isdir[fs][idx] ? 1 : 0;
        }

        public static int FileRefresh()
        {
            // BUG #2: start the directory watch on first use, then drop the
            // cached listings so the next query re-reads the medium.
            EnsureWatchers();
            listing[0] = null; listing[1] = null; listing[3] = null;
            return 1;
        }

        // File-system mutations (context-menu: new folder / delete / rename).
        // Backed by the real Windows directories under FsRoot, so the same
        // managed code path works on both the kernel and WinHost.
        public static int FileMkDir(int fs, string name)
        {
            if (!FsOk(fs) || string.IsNullOrEmpty(name)) return -1;
            if (!FsWritable(fs)) return EROFS;   // BUG #8: SFS is read-only
            try { Directory.CreateDirectory(Path.Combine(Dir(fs), name)); FileRefresh(); return 1; }
            catch { return -1; }
        }

        public static int FileDelete(int fs, string name)
        {
            if (!FsOk(fs) || string.IsNullOrEmpty(name)) return -1;
            if (!FsWritable(fs)) return EROFS;   // BUG #8: SFS is read-only
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
            if (!FsOk(fs) || string.IsNullOrEmpty(oldName) || string.IsNullOrEmpty(newName)) return -1;
            if (!FsWritable(fs)) return EROFS;   // BUG #8: SFS is read-only
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
            if (!FsOk(fs) || string.IsNullOrEmpty(name)) return "";
            try
            {
                string p = Path.Combine(Dir(fs), Path.GetFileName(name));
                if (!File.Exists(p)) return "";
                string t = File.ReadAllText(p);
                return t.Length > 4096 ? t.Substring(0, 4096) : t;
            }
            catch { return ""; }
        }

        // Raw bytes as a lower-case hex string (WinHost parity with the VM).
        public static string ReadHex(int fs, string name)
        {
            if (!FsOk(fs) || string.IsNullOrEmpty(name)) return "";
            try
            {
                string p = Path.Combine(Dir(fs), Path.GetFileName(name));
                if (!File.Exists(p)) return "";
                byte[] b = File.ReadAllBytes(p);
                int n = b.Length > 2048 ? 2048 : b.Length;
                System.Text.StringBuilder sb = new System.Text.StringBuilder(n * 2);
                for (int i = 0; i < n; i++) sb.Append(b[i].ToString("x2"));
                return sb.ToString();
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

        // Managed code opens an app (context-menu actions, Notepad from
        // the File Explorer).  The host must create a REAL window (WinRec),
        // mirroring gui.cpp launch_app in the VM; without the hook the app
        // would exist only as an unpainted C# instance.
        public static void OpenApp(int kind)
        {
            if (OpenAppHook != null) { OpenAppHook(kind); return; }
            Shell.Open(kind);
        }

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

        // Window-geometry action from the Alt+Space / title-bar menu.
        // The WinForms harness (ShellForm) owns WinRec geometry, so it
        // performs minimise / maximise / restore; others are no-ops here.
        public static void WinAction(int id, int code)
        {
            if (WinActionHook != null) WinActionHook(id, code);
        }

        // Push the already-painted backbuffer to the screen right now.        // The kernel Repaint() is a present-only internal call; here the
        // WinForms harness owns the frame, so invalidate and let the paint
        // handler blit immediately.  Safe to call from inside a paint.
        public static void Repaint() { if (RepaintHook != null) RepaintHook(); }

        // Toggle "whole window fully transparent" (the desktop shows through
        // every window's glassy chrome).  On the WinForms host the opacity is
        // applied by the shell form; 0 restores normal opacity.
        public static void SetWindowTransparent(int on)
        {
            if (WindowTransparentHook != null) WindowTransparentHook(on);
        }

        // System load 0..100 from the GUI loop's idle accounting.  The
        // WinForms host has no TSC idle counter; approximate with the
        // process CPU time share since the previous sample.
        static long s_cpuLastTicks;
        static long s_cpuLastMs = Environment.TickCount;
        public static int CpuLoad()
        {
            try
            {
                var p = System.Diagnostics.Process.GetCurrentProcess();
                long cpu = p.TotalProcessorTime.Ticks;
                long now = Environment.TickCount;
                long wall = now - s_cpuLastMs;
                if (wall <= 0) return 0;
                long used = cpu - s_cpuLastTicks;
                s_cpuLastTicks = cpu;
                s_cpuLastMs = now;
                // Ratio of CPU time to wall time, scaled to a 0..100 percent.
                long pct = used * 100 / (wall * TimeSpan.TicksPerMillisecond);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                return (int)pct;
            }
            catch { return 0; }
        }

        public static void Log(string s) { Console.WriteLine("[shell] " + s); }

        // Persist a text body to the volume selected by fs.
        //
        // BUG #2 (ghost new file): this used to ignore fs and name and call
        // File.WriteAllText(name, ...), which resolved against the PROCESS
        // WORKING DIRECTORY.  The file landed next to the .exe, so the volume
        // listing (which reads FsRoot/mkfs) never saw it: New file appeared to
        // do nothing, and refreshing did not help because the entry existed
        // outside the volume entirely.
        //
        // BUG #8: SFS (fs==1) is read-only, so a write there is refused with
        // EROFS instead of silently writing to the wrong place.
        public static int WriteText(int fs, string name, string text)
        {
            if (!FsOk(fs) || string.IsNullOrEmpty(name)) return -1;
            if (!FsWritable(fs)) return EROFS;   // BUG #8: SFS is read-only
            try
            {
                // Strip any directory part: the volume namespace is flat.
                string leaf = Path.GetFileName(name);
                if (string.IsNullOrEmpty(leaf)) return -1;
                string p = Path.Combine(Dir(fs), leaf);
                Directory.CreateDirectory(Dir(fs));
                File.WriteAllText(p, text);
                FileRefresh();                   // BUG #2: listing must see it
                return System.Text.Encoding.UTF8.GetByteCount(text);
            }
            catch { return -1; }
        }

        public static string CharStr(int ch)
        {
            if (ch <= 0 || ch > 0xFFFF) return "";
            return ((char)ch).ToString();
        }

        // Shared clipboard for the WinForms host (mirrors the kernel one).
        private static string g_clipboard = "";
        public static string GetClipboard() { return g_clipboard ?? ""; }
        public static void SetClipboard(string s) { g_clipboard = s ?? ""; }

        // Retro "pixel / CRT" framebuffer post-process.  On the VM this is
        // an InternalCall into the kernel; on the WinForms host there is no
        // kernel framebuffer, so it is a no-op.  Kept for Theme.ApplyPixel
        // parity between the two build targets.
        public static void SetPixel(int mode, int scale, int scan) { }

        // ---- network downloads ---------------------------------------
        // Backing store for the three kernel-facing download InternalCalls.
        // All real I/O is in NexOS.Net (NetStack.cs); these are thin,
        // signature-stable shims because the kernel MiniCLR cannot await.
        //
        //   DownloadFile   : synchronous, SmartDownloadRouter (5MB split)
        //   DownloadAsync  : fire-and-forget, MultiPathManager aggregation
        //   DownloadStatus : JSON progress snapshot for either path
        //   DownloadCancel : abort an in-flight DownloadAsync
        static readonly System.Collections.Generic.Dictionary<string, System.Threading.CancellationTokenSource> s_dlCancel
            = new System.Collections.Generic.Dictionary<string, System.Threading.CancellationTokenSource>();
        static readonly object s_dlLock = new object();

        // Resolve a managed destination path against the host FS root so a
        // kernel-relative name like "mkfs/a.bin" lands under the mapped tree.
        static string DlDest(string dest)
        {
            if (string.IsNullOrEmpty(dest)) return dest;
            try
            {
                if (System.IO.Path.IsPathRooted(dest)) return dest;
                string root = string.IsNullOrEmpty(FsRoot) ? AppContext.BaseDirectory : FsRoot;
                return System.IO.Path.GetFullPath(System.IO.Path.Combine(root, dest));
            }
            catch { return dest; }
        }

        // Synchronous download through the router: files under 5MB take the
        // single-socket fast path, larger ones get HTTP-Range sharding with
        // IDM-style resume.  Returns bytes written, or a negative error code.
        public static int DownloadFile(string url, string dest, int timeoutMs)
        {
            if (string.IsNullOrEmpty(url) || string.IsNullOrEmpty(dest)) return -1;
            string path = DlDest(dest);
            try
            {
                long n = NexOS.Net.SmartDownloadRouter
                    .DownloadFileAsync(url, path, timeoutMs, null)
                    .GetAwaiter().GetResult();
                return n > int.MaxValue ? int.MaxValue : (int)n;
            }
            catch (Exception ex)
            {
                Log("[dl] DownloadFile failed: " + ex.Message);
                return -2;
            }
        }

        // Start an aggregated multi-NIC download in the background and return
        // immediately (0 == started).  This is the body of the MCP tool
        // "download_async".  Poll DownloadStatus(dest) for progress.
        public static int DownloadAsync(string url, string dest)
        {
            if (string.IsNullOrEmpty(url) || string.IsNullOrEmpty(dest)) return -1;
            string path = DlDest(dest);
            var cts = new System.Threading.CancellationTokenSource();
            lock (s_dlLock) { s_dlCancel[path] = cts; }

            System.Threading.Tasks.Task.Run(() =>
            {
                try
                {
                    NexOS.Net.MultiPathManager
                        .DownloadAsync(url, path, null, cts.Token)
                        .GetAwaiter().GetResult();
                }
                catch (Exception ex)
                {
                    Log("[dl] multipath failed: " + ex.Message);
                }
                finally
                {
                    lock (s_dlLock) { s_dlCancel.Remove(path); }
                    cts.Dispose();
                }
            });
            return 0;
        }

        // JSON progress snapshot.  Prefers the multipath registry, then the
        // smart-downloader registry, then the persisted resume manifest, so a
        // download that survived an app restart still reports its offsets.
        public static string DownloadStatus(string dest)
        {
            if (string.IsNullOrEmpty(dest)) return "{\"code\":5}";
            string path = DlDest(dest);
            NexOS.Net.DownloadProgress p = null;
            if (!NexOS.Net.MultiPathManager.s_progress.TryGetValue(path, out p) || p == null)
                if (!NexOS.Net.SmartDownloadRouter.s_progress.TryGetValue(path, out p) || p == null)
                    p = NexOS.Net.SmartDownloader.Peek(path);
            if (p == null) return "{\"code\":4,\"state\":\"Idle\"}";
            return "{\"code\":0"
                 + ",\"state\":\"" + p.State + "\""
                 + ",\"total\":" + p.Total
                 + ",\"received\":" + p.Received
                 + ",\"percent\":" + p.Percent
                 + ",\"slices\":" + p.Slices
                 + ",\"done\":" + p.DoneSlices
                 + ",\"bps\":" + (long)p.ThroughputBps
                 + ",\"error\":\"" + (p.Error ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\""
                 + "}";
        }

        // Abort a running DownloadAsync.  The resume manifest is left on disk,
        // so a later DownloadFile to the same destination continues from the
        // recorded per-slice offsets instead of restarting.
        public static int DownloadCancel(string dest)
        {
            if (string.IsNullOrEmpty(dest)) return -1;
            string path = DlDest(dest);
            lock (s_dlLock)
            {
                System.Threading.CancellationTokenSource cts;
                if (s_dlCancel.TryGetValue(path, out cts))
                {
                    try { cts.Cancel(); } catch { }
                    return 0;
                }
            }
            return -2;
        }
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
        public static string StrSub(string s, int start, int len)
        {
            if (s == null) return null;
            int n = s.Length;
            if (start < 0) start = 0;
            if (start > n) start = n;
            if (len < 0) len = 0;
            if (start + len > n) len = n - start;
            return s.Substring(start, len);
        }
        public static string StrFlat(string s)
        {
            if (s == null) return null;
            var b = new System.Text.StringBuilder(s.Length);
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                b.Append((c < ' ' || c == (char)0x7F) ? ' ' : c);
            }
            return b.ToString();
        }
        public static bool StrEq(string a, string b) { return string.Equals(a, b, StringComparison.Ordinal); }
        public static string IntToStr(int v) { return v.ToString(System.Globalization.CultureInfo.InvariantCulture); }

        public static int TickCount() { return Environment.TickCount; }
    }
}



namespace NexOS.Forms
{
    // -----------------------------------------------------------------
    //  Backend  --  WinHost implementations of the NexOS.AI / NexOS.MCP
    //           internal calls.  The kernel build binds these names in
    //           clr.cpp via ic_ai_* / ic_mcp_*; here we give them real
    //           managed bodies on top of HttpClient so the *same* managed
    //           app sources (NexOS.AI / NexOS.MCP) run unchanged on the
    //           Windows host.  Signatures are referenced from AI.cs / MCP.cs
    //           and must stay in lock-step.
    // -----------------------------------------------------------------
    using System.Net.Http;
    using System.Threading.Tasks;

    public static class Backend
    {
        // AI gateway host-side address (matches nexos_ai.h AIGateway).
        const string GATEWAY = "http://127.0.0.1:19387";
        static int    s_mode = 1; // Balanced by default

        static readonly HttpClient s_http = new HttpClient();
        static readonly object     s_lock = new object();

        // ---- AI surface ----
        public static int AICurrentMode { get { return s_mode; } }
        public static void AISetMode(int m) { s_mode = m; }

        public static int AIQuota()
        {
            try
            {
                string r = s_http.GetStringAsync(GATEWAY + "/quota").Result;
                // naive int parse of "123" or {"quota":123}
                int i = r.LastIndexOf(':');
                if (i >= 0) r = r.Substring(i + 1).Trim('}', ' ', '"');
                int v; return int.TryParse(r, out v) ? v : 1000;
            }
            catch { return 1000; }
        }

        public static Task<NexOS.AI.AIResponse> AIRequestAsync(string prompt, NexOS.AI.AIMode mode)
        {
            return Task.Run(() =>
            {
                var resp = new NexOS.AI.AIResponse();
                try
                {
                    int m;
                    if (mode == NexOS.AI.AIMode.Fast) m = 0;
                    else if (mode == NexOS.AI.AIMode.Heavy) m = 2;
                    else m = 1;
                    s_mode = m;
                    var c = new StringContent(
                        "{\"prompt\":\"" + (prompt ?? "").Replace("\"", "\\\"") + "\",\"mode\":" + m + "}",
                        System.Text.Encoding.UTF8, "application/json");
                    var t = s_http.PostAsync(GATEWAY + "/ai", c).Result;
                    string body = t.IsSuccessStatusCode
                        ? t.Content.ReadAsStringAsync().Result
                        : "";
                    resp.code = t.IsSuccessStatusCode ? 0 : 5;
                    resp.text = body;
                    resp.mode = m;
                    resp.quotaLeft = AIQuota();
                }
                catch (System.Exception ex)
                {
                    resp.code = 5;
                    resp.text = ex.Message;
                    resp.mode = s_mode;
                    resp.quotaLeft = 0;
                }
                return resp;
            });
        }

        // ---- MCP tool registry ---------------------------------------
        // The resident identity on WinHost is just the assembly name. A real
        // deployment would derive this from the booted .mex; for parity with
        // the kernel it is read once at startup by ShellForm via SetMcpAppId.
        static string s_myAppId = "winhost";

        public static void SetMcpAppId(string id) { if (!string.IsNullOrEmpty(id)) s_myAppId = id; }
        public static string McpMyAppId() { return s_myAppId; }

        // Every host-served tool, in one table. This is the authoritative
        // list on the WinHost side; the kernel-side mirror lives in
        // apps.json (app_id "winhost") + g_mcp_impls[] in nexos_ai.cpp.
        // Keep the three in sync -- see docs/MCP_TOOLS.md.
        public sealed class McpToolSpec
        {
            public string Name;
            public string Description;
            public string ParametersSchema;   // JSON Schema literal
        }

        static readonly McpToolSpec[] s_localTools =
        {
            new McpToolSpec
            {
                Name = "download_file",
                Description = "Download a URL to a local path synchronously. Files under 5MB use a single-socket fast path; larger files are split into HTTP Range shards with an IDM-style resume state machine. Returns bytes written, or a negative error code.",
                ParametersSchema = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Absolute http(s) URL\"},\"dest\":{\"type\":\"string\",\"description\":\"Destination file path\"},\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Per-request timeout in ms (0 = default)\"}},\"required\":[\"url\",\"dest\"]}",
            },
            new McpToolSpec
            {
                Name = "download_async",
                Description = "Start an aggregated multi-NIC download in the background and return immediately. Detects every usable WiFi/Ethernet link, binds one socket per NIC, gives each a disjoint byte range, and fetches them in parallel to aggregate physical bandwidth (bypassing TCP head-of-line blocking). Poll download_status for progress.",
                ParametersSchema = "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"Absolute http(s) URL\"},\"dest\":{\"type\":\"string\",\"description\":\"Destination file path\"}},\"required\":[\"url\",\"dest\"]}",
            },
            new McpToolSpec
            {
                Name = "download_status",
                Description = "Report progress of a download started by download_file or download_async. Falls back to the persisted resume manifest so an interrupted (or restarted) download still reports its per-slice offsets.",
                ParametersSchema = "{\"type\":\"object\",\"properties\":{\"dest\":{\"type\":\"string\",\"description\":\"Destination file path used when starting the download\"}},\"required\":[\"dest\"]}",
            },
            new McpToolSpec
            {
                Name = "download_cancel",
                Description = "Abort a running download_async. The resume manifest is kept, so a later download_file to the same destination continues from the recorded per-slice offsets instead of restarting.",
                ParametersSchema = "{\"type\":\"object\",\"properties\":{\"dest\":{\"type\":\"string\",\"description\":\"Destination file path\"}},\"required\":[\"dest\"]}",
            },
            new McpToolSpec
            {
                Name = "net_status",
                Description = "Report the MCP bus connection state: whether the framed TCP bus is connected, last/min/max heartbeat round-trip latency in ms, heartbeat and reconnect counters, uptime, and how many requests are pending or queued for replay.",
                ParametersSchema = "{\"type\":\"object\",\"properties\":{}}",
            },
        };

        // The tool catalogue as a JSON array, so an AI agent (or a registry
        // dump) can enumerate what this host exposes without guessing.
        public static string McpListTools()
        {
            var sb = new StringBuilder("{\"code\":0,\"app_id\":\"");
            sb.Append(Escape(s_myAppId)).Append("\",\"tools\":[");
            for (int i = 0; i < s_localTools.Length; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append("{\"tool_name\":\"").Append(Escape(s_localTools[i].Name))
                  .Append("\",\"description\":\"").Append(Escape(s_localTools[i].Description))
                  .Append("\",\"parameters_schema\":").Append(s_localTools[i].ParametersSchema)
                  .Append('}');
            }
            return sb.Append("]}").ToString();
        }

        static bool IsLocalTool(string name)
        {
            for (int i = 0; i < s_localTools.Length; i++)
                if (s_localTools[i].Name == name) return true;
            return false;
        }

        public static string McpCallTool(string target_app_id, string tool_name, string json_args)
        {
            // ---- local tools ------------------------------------------
            // download_* and net_status are served by the host itself: the bus
            // gateway has no network stack, and the whole point of
            // download_async is that it binds the *local* WiFi/NIC sockets to
            // aggregate bandwidth.
            string local = McpLocalTool(target_app_id, tool_name, json_args);
            if (local != null) return local;

            try
            {
                var c = new StringContent(
                    "{\"source_app_id\":\"" + s_myAppId.Replace("\"", "\\\"") +
                    "\",\"tool_name\":\"" + (tool_name ?? "").Replace("\"", "\\\"") +
                    "\",\"json_args\":" + (json_args ?? "null") + "}",
                    System.Text.Encoding.UTF8, "application/json");
                var t = s_http.PostAsync(GATEWAY + "/mcp/" + (target_app_id ?? ""), c).Result;
                string body = t.IsSuccessStatusCode ? t.Content.ReadAsStringAsync().Result : "{\"code\":5}";
                return body;
            }
            catch (System.Exception ex)
            {
                return "{\"code\":5,\"json\":\"" + ex.Message.Replace("\"", "\\\"") + "\"}";
            }
        }

        // Host-served MCP tools.  Returns null when the tool is not ours so the
        // caller falls through to the gateway bus.
        static string McpLocalTool(string target_app_id, string tool_name, string json_args)
        {
            if (string.IsNullOrEmpty(tool_name)) return null;
            string t = tool_name.Trim();

            // Catalogue introspection: always answer, no parameters.
            if (t == "tools_list" || t == "mcp_list_tools") return McpListTools();

            if (!IsLocalTool(t)) return null;

            string url = JsonStr(json_args, "url");
            string dest = JsonStr(json_args, "dest");
            if (string.IsNullOrEmpty(dest)) dest = JsonStr(json_args, "path");
            if (string.IsNullOrEmpty(dest)) dest = JsonStr(json_args, "destination");

            switch (t)
            {
                case "net_status":
                    return NetStatusJson();

                case "download_async":
                {
                    if (string.IsNullOrEmpty(url) || string.IsNullOrEmpty(dest))
                        return "{\"code\":2,\"json\":\"missing url/dest\"}";
                    int rc = NexOS.Forms.Host.DownloadAsync(url, dest);
                    return "{\"code\":" + (rc == 0 ? 0 : 5) + ",\"json\":\"" +
                           (rc == 0 ? "started" : "failed") + "\",\"dest\":\"" +
                           Escape(dest) + "\"}";
                }
                case "download_file":
                {
                    if (string.IsNullOrEmpty(url) || string.IsNullOrEmpty(dest))
                        return "{\"code\":2,\"json\":\"missing url/dest\"}";
                    int n = NexOS.Forms.Host.DownloadFile(url, dest, 0);
                    return "{\"code\":" + (n >= 0 ? 0 : 5) + ",\"bytes\":" + n + ",\"dest\":\"" + Escape(dest) + "\"}";
                }
                case "download_status":
                    return NexOS.Forms.Host.DownloadStatus(dest);
                default:
                {
                    int c = NexOS.Forms.Host.DownloadCancel(dest);
                    return "{\"code\":" + (c == 0 ? 0 : 4) + ",\"json\":\"" + (c == 0 ? "cancelled" : "not running") + "\"}";
                }
            }
        }

        // Public wrappers used by NexOS.MCP.Net (MCP.cs).
        public static string McpNetStatus() { return NetStatusJson(); }

        // net_status payload: the MCP bus connection state plus heartbeat
        // latency. An AI agent can use this to distinguish "connected and
        // healthy" from "connected but the link is degrading" (rising RTT or
        // missed heartbeats) without opening a socket itself.
        static string NetStatusJson()
        {
            var st = NexOS.Net.McpBusHost.Status();
            if (st == null) return "{\"code\":5,\"error\":\"no bus status\"}";

            var sb = new StringBuilder("{\"code\":0");
            sb.Append(",\"connected\":").Append(st.Connected ? "true" : "false");
            sb.Append(",\"host\":\"").Append(Escape(st.Host)).Append('"');
            sb.Append(",\"port\":").Append(st.Port);
            sb.Append(",\"rtt_ms\":").Append(st.LastRttMs);
            sb.Append(",\"rtt_min_ms\":").Append(st.MinRttMs);
            sb.Append(",\"rtt_max_ms\":").Append(st.MaxRttMs);
            sb.Append(",\"rtt_samples\":").Append(st.RttSamples);
            sb.Append(",\"heartbeats_sent\":").Append(st.HeartbeatsSent);
            sb.Append(",\"heartbeats_missed\":").Append(st.HeartbeatsMissed);
            sb.Append(",\"connects\":").Append(st.ConnectCount);
            sb.Append(",\"reconnects\":").Append(st.ReconnectCount);
            sb.Append(",\"uptime_ms\":").Append(st.UptimeMs);
            sb.Append(",\"pending_requests\":").Append(st.PendingRequests);
            sb.Append(",\"outbox_depth\":").Append(st.OutboxDepth);
            // Coarse health verdict so the model does not have to interpret
            // raw counters itself.
            string health;
            if (!st.Connected) health = "down";
            else if (st.LastRttMs < 0) health = "connected";           // no sample yet
            else if (st.HeartbeatsMissed > 0) health = "degraded";
            else if (st.LastRttMs > 500) health = "slow";
            else health = "healthy";
            sb.Append(",\"health\":\"").Append(health).Append('"');
            return sb.Append('}').ToString();
        }

        // Minimal JSON string extractor (the args object is flat in practice).
        static string JsonStr(string json, string key)
        {
            if (string.IsNullOrEmpty(json)) return null;
            int i = json.IndexOf("\"" + key + "\"", StringComparison.Ordinal);
            if (i < 0) return null;
            int c = json.IndexOf(':', i);
            if (c < 0) return null;
            int q = json.IndexOf('"', c + 1);
            if (q < 0) return null;
            var sb = new StringBuilder();
            for (int k = q + 1; k < json.Length; k++)
            {
                char ch = json[k];
                if (ch == '\\' && k + 1 < json.Length)
                {
                    char nx = json[++k];
                    sb.Append(nx == 'n' ? '\n' : nx == 't' ? '\t' : nx);
                    continue;
                }
                if (ch == '"') break;
                sb.Append(ch);
            }
            return sb.ToString();
        }

        static string Escape(string s)
        {
            return (s ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"");
        }
    }
}
