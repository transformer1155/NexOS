// =====================================================================
//  Forms.cs  -  NexOS.Forms, a Windows-shaped UI toolkit for MiniCLR
// ---------------------------------------------------------------------
//  This is the managed half of the GUI.  The kernel (mforms.cpp) exposes
//  drawing and machine-state primitives as internal calls; everything a
//  user sees -- every window, control and application -- is C# built on
//  top of them.
//
//  Constraints the interpreter imposes (respected throughout this tree):
//    * bump-allocated managed heap, rewound every frame -> Paint() must
//      not keep anything it allocates; durable state is created in a
//      ctor / Init / Click and stored in fields.
//    * arrays hold 4-byte slots -> only int[] and reference[] are used.
//    * no floating point, no generics, no interfaces, no try/catch,
//      no string.Format; string offers Length, [i] and Concat only.
//    * static field initialisers do NOT run (no .cctor) -> statics are
//      assigned explicitly in Init().
// =====================================================================
using System.Runtime.CompilerServices;

namespace NexOS.Forms
{
#if !WINHOST
    // -----------------------------------------------------------------
    //  Gfx -- drawing surface.  Coordinates are client pixels; the host
    //  adds the window origin and clips to the content area.
    //
    //  NOTE: when WINHOST is defined this file is being compiled for the
    //  Windows WinForms harness (csharp/winhost), which supplies real
    //  GDI+ implementations of Gfx/Host with identical signatures.  The
    //  internal-call declarations below are therefore skipped there.
    // -----------------------------------------------------------------
    public static class Gfx
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void FillRect(int x, int y, int w, int h, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void FillRound(int x, int y, int w, int h, int r, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void DrawRound(int x, int y, int w, int h, int r, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void DrawRect(int x, int y, int w, int h, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void DrawLine(int x0, int y0, int x1, int y1, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Gradient(int x, int y, int w, int h, uint top, uint bot);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Text(int x, int y, string s, uint fg);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void TextBg(int x, int y, string s, uint fg, uint bg);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void TextCenter(int x, int y, int w, string s, uint fg);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void FillCircle(int cx, int cy, int r, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void DrawCircle(int cx, int cy, int r, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Icon(int x, int y, int sz, uint bg, int letter, uint lc);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Progress(int x, int y, int w, int h, int pct, uint c);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  HasImage(int id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Image(int id, int x, int y, int w, int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  Measure(string s);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  Width();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  Height();
        // Absolute screen size, independent of the current client context
        // (inside a window Gfx.Width/Height report the window's client
        // size, which is wrong for positioning popup menus).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  ScreenW();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  ScreenH();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  MouseX();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  MouseY();
        // Origin of the current client context (window offset), so controls
        // can translate their local coordinates to screen space.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  OriginX();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  OriginY();
        // Synthetically position the pointer (used by voice / automation
        // clicks so controls that hit-test via Gfx.MouseX/Y still fire).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void SetMouse(int x, int y);
    }

    // -----------------------------------------------------------------
    //  Host -- read-only view of the machine, plus a couple of actions.
    // -----------------------------------------------------------------
    public static class Host
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int MemTotalKb();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int PagesFree();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int PagesUsed();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int PagesTotal();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapAlloc();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapFree();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapAllocCnt();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapFreeCnt();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Optimize();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Hour();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Minute();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Second();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string OsName();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string CpuVendor();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string DiskModel();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int DiskSizeMb();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Is64Bit();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int PciCount();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int NicPresent();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Ticks();
        // Monotonic milliseconds (host-calibrated); used for double-click
        // detection.  Wraps after ~49 days.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int TickMs();
        // Request the host to keep repainting so managed animations
        // (AI desktop thinking dots / typewriter reveal) can progress.
        // The GUI loop throttles render_all() to ~30 fps while set.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void SetAnim(int on);
        // Bit i is set when a window of Kind i is open; drives the
        // running-app indicators under the taskbar buttons.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int RunningMask();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileCount(int fs);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string FileName(int fs, int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileIsDir(int fs, int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileRefresh();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string ReadText(int fs, string name);
        // Persist a UTF-8 text body to stable storage.  Returns bytes written
        // (>=0) or -1 on error.  Used by the shell to save personalization
        // settings ("nexos.cfg") and Notepad documents.  fs selects the
        // volume (0=mkfs).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int WriteText(int fs, string name, string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string Exec(string cmd);
        // Execute a native Windows PE image (.exe) through the kernel's
        // win32 / win64 PE loader and surface the windows it creates on the
        // desktop -- the same path as `winapp foo.exe` at the prompt.  This
        // is what a double-click on an .exe runs, so programs actually
        // execute instead of opening in Notepad.  Returns the number of
        // desktop windows created, or a negative loader error code.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int RunExe(string name);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Shutdown();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Reboot();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Log(string s);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string CharStr(int ch);
        // Shared clipboard (mirrors the kernel terminal clipboard) so copy in
        // one app can be pasted in another and in the text terminal.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string GetClipboard();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void   SetClipboard(string s);
        // File-system mutations, backed by the kernel's SFS (mkfs).  These
        // let the kernel-native context menus rename / delete / create
        // files the same way the terminal's rm/copy/mkdir do.  fs selects
        // the volume the File Explorer is currently showing (0=mkfs,1=sfs).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  FileMkDir(int fs, string name);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  FileDelete(int fs, string name);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  FileRename(int fs, string oldName, string newName);
        // Ask the kernel to open (or focus) an application window of this
        // Kind.  Needed when managed code itself decides to launch an app
        // (e.g. Notepad from the File Explorer): Shell.Open() alone would
        // only create a C# instance with no native window to paint it.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void OpenApp(int kind);
        // Ask the kernel to close every window of this Kind (taskbar
        // right-click "Close window" / "End process").
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void CloseApp(int kind);
        // Ask the kernel to leave GUI mode and return to the text terminal.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void ExitGui();
        // Window-geometry action from the Alt+Space / title-bar menu
        // (Restore / Minimize / Maximize).  The kernel owns the window rect
        // in both the VM (gui.cpp) and the WinForms harness, so it performs
        // the minimise / maximise / restore here.  code is a WAct.* value.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void WinAction(int id, int code);
        // Synchronous HTTP GET for the Browser control.  Returns the
        // response body, or "" on error / when offline.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string HttpGet(string url);
        // ---- sign-in ------------------------------------------------
        // The lock screen (Login.cs) draws the UI; the kernel keeps the
        // accounts and the password hashes.  LoginCheck returns the uid
        // and commits the session when the credentials match, or -1 when
        // they do not.  LoginUid reports the session already signed in
        // (-1 while locked), so re-entering the desktop from the text
        // terminal does not ask for the password a second time.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int    LoginCheck(string user, string pass);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int    LoginUid();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int    UserCount();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string UserName(int idx);
        // Push the retro "pixel / CRT monitor" settings down to the kernel
        // framebuffer post-process: mode (on/off), scale (block size), scan (scanlines).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void   SetPixel(int mode, int scale, int scan);
    }
#endif // !WINHOST

    // =================================================================
    //  Btn - global "press" animation state for every button control.
    // -----------------------------------------------------------------
    //  A left click (routed by the host / kernel bridge through
    //  PressScreen with SCREEN coordinates) arms a timed press.  Each
    //  button control asks ScaleAt() whether the last press landed on it
    //  and, if so, gets a scale% that shrinks it to half in 100 ms, holds
    //  it there for 500 ms and springs it back in 150 ms.  All phases are
    //  millisecond-driven (Host.TickMs), so the animation is identical on
    //  the VM and the WinForms host regardless of frame rate.
    // =================================================================
    public static class Btn
    {
        static int pressX, pressY, pressMs;   // last left click, screen space

        public static void PressScreen(int sx, int sy)
        { pressX = sx; pressY = sy; pressMs = Host.TickMs(); }

        public static bool Pressing()
        {
            int e = Host.TickMs() - pressMs;
            return pressMs > 0 && e >= 0 && e < 750;
        }

        // sx,sy = button origin in SCREEN space; w,h = button size.
        // Returns the scale% to draw at (100 = full, 50 = half).
        public static int ScaleAt(int sx, int sy, int w, int h)
        {
            int e = Host.TickMs() - pressMs;
            if (pressMs <= 0 || e < 0 || e >= 750) return 100;
            if (sx > pressX || pressX >= sx + w || sy > pressY || pressY >= sy + h)
                return 100;
            if (e < 100) return 100 - 50 * e / 100;   // press: -> 50
            if (e < 600) return 50;                   // hold 500 ms
            return 50 + 50 * (e - 600) / 150;         // restore: -> 100
        }
    }

    // =================================================================
    //  TBox - a tiny single/multi-line text editor model shared by every
    //  managed input box.  Provides a caret, a selection range and a
    //  single-step undo stack so the terminal-style shortcuts
    //  (Ctrl+C copy, Ctrl+V paste, Ctrl+Z undo, Ctrl+A select-all) behave
    //  identically everywhere.  The MiniCLR does not run static
    //  initialisers, so the undo arrays are allocated in the constructor.
    // =================================================================
    public class TBox
    {
        public string text = "";
        public int cursor = 0;            // caret index (0..len)
        public int selA = 0, selB = 0;    // selection [selA, selB); equal => none
        string[] undoText;
        int[] undoCur;
        int undoSp = 0;
        const int UNDO_MAX = 32;

        public TBox() {
            undoText = new string[UNDO_MAX];
            undoCur = new int[UNDO_MAX];
            undoSp = 0;
        }

        void PushUndo() {
            if (undoSp < UNDO_MAX) {
                undoText[undoSp] = text;
                undoCur[undoSp] = cursor;
                undoSp++;
            }
        }
        public void Undo() {
            if (undoSp > 0) {
                undoSp--;
                text = undoText[undoSp];
                cursor = undoCur[undoSp];
                selA = selB = cursor;
            }
        }
        // Insert s at the caret, replacing any active selection.
        public void Insert(string s) {
            int a = selA, b = selB; if (a > b) { int t = a; a = b; b = t; }
            PushUndo();
            string r = "";
            int i = 0;
            for (i = 0; i < a; i++) r = U.Cat(r, Host.CharStr((int)text[i]));
            r = U.Cat(r, s);
            for (i = b; i < text.Length; i++) r = U.Cat(r, Host.CharStr((int)text[i]));
            text = r;
            cursor = a + s.Length;
            selA = selB = cursor;
        }
        public void Backspace() {
            if (selA != selB) { Insert(""); return; }
            if (cursor > 0) {
                PushUndo();
                string r = "";
                int i = 0;
                for (i = 0; i < cursor - 1; i++) r = U.Cat(r, Host.CharStr((int)text[i]));
                for (i = cursor; i < text.Length; i++) r = U.Cat(r, Host.CharStr((int)text[i]));
                text = r;
                cursor = cursor - 1;
                selA = selB = cursor;
            }
        }
        public void Delete() {
            if (selA != selB) { Insert(""); return; }
            if (cursor < text.Length) {
                PushUndo();
                string r = "";
                int i = 0;
                for (i = 0; i < cursor; i++) r = U.Cat(r, Host.CharStr((int)text[i]));
                for (i = cursor + 1; i < text.Length; i++) r = U.Cat(r, Host.CharStr((int)text[i]));
                text = r;
                selA = selB = cursor;
            }
        }
        public string GetSelection() {
            if (selA == selB) return "";
            int a = selA, b = selB; if (a > b) { int t = a; a = b; b = t; }
            string r = "";
            for (int i = a; i < b; i++) r = U.Cat(r, Host.CharStr((int)text[i]));
            return r;
        }
        public void SelectAll() { selA = 0; selB = text.Length; }
        public void MoveLeft()  { if (cursor > 0) cursor--; selA = selB = cursor; }
        public void MoveRight() { if (cursor < text.Length) cursor++; selA = selB = cursor; }
        public void Home() { cursor = 0; selA = selB = 0; }
        public void End()  { cursor = text.Length; selA = selB = cursor; }

        // Route a keystroke.  ch is a codepoint (>=32), a negative virtual
        // key, or -1/-8 (backspace).  Returns true if consumed.
        public bool Key(int ch) {
            if (ch == -1 || ch == 8)  { Backspace(); return true; }                              // backspace
            if (ch == -3) { string s = GetSelection(); if (s == "") s = text; Host.SetClipboard(s); return true; } // Ctrl+C
            if (ch == -4) { Insert(Host.GetClipboard()); return true; }                          // Ctrl+V
            if (ch == -5) { Undo(); return true; }                                                // Ctrl+Z
            if (ch == -6) { SelectAll(); return true; }                                           // Ctrl+A
            if ((ch >= 32 && ch < 127) || (ch >= 0x80 && ch <= 0xFFFF)) { Insert(Host.CharStr(ch)); return true; }
            return false;
        }
    }

    // -----------------------------------------------------------------
    //  Theme.  const so Roslyn inlines the value: no .cctor required.
    // -----------------------------------------------------------------
    // WinUI 3 standard colour palette (Windows App SDK / Fluent).  Values are
    // aligned to win11-ui/winui3-tokens.json (extracted from WinUIonWeb's
    // theme.css) so every NexOS surface renders standard WinUI 3.
    public static class C
    {
    public const uint WinBg     = 0xF3F3F3;   // window client background (mica-ish grey)
        public const uint Card      = 0xFFFFFF;   // raised surface
        public const uint CardAlt   = 0xFAFAFA;   // subtle alternate row
        public const uint Border    = 0xE6E6E6;   // hairline (WinUI3 ControlStroke 0.06 alpha)
        public const uint BorderMid = 0xCFCFCF;
        public const uint Accent    = 0x0067C0;   // standard WinUI 3 accent blue
        public const uint AccentHi  = 0x0B6FB8;   // hovered accent (accent @0.90)
        public const uint AccentLo  = 0x0A639F;   // pressed accent (accent @0.80)
        public const uint AccentText= 0x0067C0;   // WinUI3 AccentTextFill
        public const uint Text      = 0x171717;   // primary text (rgba(0,0,0,0.89))
        public const uint TextSub   = 0x5E5E5E;   // secondary text (rgba(0,0,0,0.62))
        public const uint TextFaint = 0x737373;   // tertiary/disabled (rgba(0,0,0,0.45))
        public const uint White     = 0xFFFFFF;
        public const uint Hover     = 0xF5F5F5;   // control hover (WinUI3 SubtleFill 0.04)
        public const uint Sel       = 0xE5EFF8;   // selected row (accent @0.10)
        public const uint Good      = 0x0F7B0F;   // green (WinUI3 system success)
        public const uint Warn      = 0x9D5A00;   // amber (WinUI3 system caution)
        public const uint Danger    = 0xC42B1C;   // red (WinUI3 system critical)
    }

    // -----------------------------------------------------------------
    //  Theme  -  live, mutable design tokens.  Stored in plain static
    //  fields (assigned explicitly, no .cctor required) so the same
    //  sources run under MiniCLR and on the WinForms host.  The shell's
    //  paint code reads these each frame; the Settings app and the
    //  WinHost context menus write them.
    // -----------------------------------------------------------------
    public static class Theme
    {
        public static uint  WallTop   = 0x218FD9;   // wallpaper gradient top (Win11 blue)
        public static uint  WallBot   = 0x05216B;   // wallpaper gradient base (Win11 deep blue)
        public static uint  Accent    = 0x0067C0;   // primary accent (standard WinUI 3 blue)
        public static int    Dark      = 1;         // 0 light, 1 dark (default dark)
        public static int    TaskbarLeft = 0;       // 0 centred, 1 left-aligned
        public static int    ShowLabels = 1;        // taskbar labels (reserved)
        public static int    ActiveNet = 0;         // 0 Ethernet, 1 Wi-Fi
        public static int    VoiceOn   = 0;         // microphone listening
        // Desktop layout: 0 = Simple (classic left-aligned icon grid, the
        // earlier Win11 desktop), 1 = Busy (the current Portal surface:
        // search bar + wordmark + tiles + nav tabs + live cards).
        public static int    DesktopMode = 0;       // 0 = clean Win11 desktop, 1 = Portal launcher

        // Retro "pixel / CRT monitor" render mode.  Pushed to the kernel via
        // Host.SetPixel so the single framebuffer post-process applies to every
        // UI surface (managed desktop, native window chrome, cursor).
        //   PixelMode  : 0 off, 1 on (default ON -- the user wants the look)
        //   PixelScale : block size in pixels (1 = full spatial detail)
        //   PixelScan  : 0 off, 1 on (CRT scanline darkening)
        public static int    PixelMode  = 1;
        public static int    PixelScale = 1;
        public static int    PixelScan  = 0;

        // Terminal (GNOME-Terminal-style) presentation, persisted so it
        // survives reboot.  NOTE: the kernel bitmap font is fixed-size, so
        // TermCellH is a *layout zoom* of the terminal grid (cell spacing
        // scales), not a true glyph-size change.  TermBgMode 1 blends the
        // Ubuntu purple with the live wallpaper hue behind the window, a
        // pure-C# approximation of a transparent terminal background.
        public static int    TermCellH  = 18;   // 12..28, default 18
        public static int    TermBgMode = 0;    // 0 solid, 1 wallpaper-tint

        // A small Fluent accent ramp, indexed by the Settings swatches.
        public static uint[] Accents()
        {
            uint[] a = new uint[6];
            a[0] = 0x0067C0; a[1] = 0x8B5CF6; a[2] = 0x0EA5E9;
            a[3] = 0x107C10; a[4] = 0xE11D8A; a[5] = 0xF59E0B;
            return a;
        }

        // -----------------------------------------------------------------
        //  Persistence.  Personalization is serialized to a tiny "nexos.cfg"
        //  text file on the MKFS data disk so it survives reboot.  The
        //  format is one "key=value" line per field; values are plain
        //  decimal integers (colours fit in a signed int: max 0xFFFFFF).
        // -----------------------------------------------------------------
        public static string CfgName = "nexos.cfg";

        public static void Save()
        {
            string cfg = "";
            cfg = NexOS.Sys.StrConcat(cfg, "walltop=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr((int)WallTop));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "wallbot=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr((int)WallBot));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "accent=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr((int)Accent));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "dark=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(Dark));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "taskbarleft=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(TaskbarLeft));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "showlabels=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(ShowLabels));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "activenet=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(ActiveNet));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "voiceon=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(VoiceOn));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "desktopmode=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(DesktopMode));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "pixelmode=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(PixelMode));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "pixelscale=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(PixelScale));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "pixelscan=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(PixelScan));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "termcellh=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(TermCellH));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "termbgmode=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(TermBgMode));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            Host.WriteText(0, CfgName, cfg);
        }

        public static void Load()
        {
            string s = Host.ReadText(0, CfgName);
            if (s == null) return;
            if (NexOS.Sys.StrLen(s) == 0) return;
            int n = NexOS.Sys.StrLen(s), i = 0;
            while (i < n) {
                int start = i;
                while (i < n && NexOS.Sys.StrCharAt(s, i) != '\n') i++;
                string line = NexOS.Sys.StrSub(s, start, i - start);
                i++; // consume '\n'
                int ln = NexOS.Sys.StrLen(line), eq = 0;
                while (eq < ln && NexOS.Sys.StrCharAt(line, eq) != '=') eq++;
                if (eq >= ln) continue;
                string key = NexOS.Sys.StrSub(line, 0, eq);
                string val = NexOS.Sys.StrSub(line, eq + 1, ln - eq - 1);
                int v = ParseInt(val);
                if      (NexOS.Sys.StrEq(key, "walltop"))     WallTop     = (uint)v;
                else if (NexOS.Sys.StrEq(key, "wallbot"))     WallBot     = (uint)v;
                else if (NexOS.Sys.StrEq(key, "accent"))      Accent      = (uint)v;
                else if (NexOS.Sys.StrEq(key, "dark"))        Dark        = v;
                else if (NexOS.Sys.StrEq(key, "taskbarleft")) TaskbarLeft = v;
                else if (NexOS.Sys.StrEq(key, "showlabels"))  ShowLabels  = v;
                else if (NexOS.Sys.StrEq(key, "activenet"))   ActiveNet   = v;
                else if (NexOS.Sys.StrEq(key, "voiceon"))     VoiceOn     = v;
                else if (NexOS.Sys.StrEq(key, "desktopmode")) DesktopMode = v;
                else if (NexOS.Sys.StrEq(key, "pixelmode"))   PixelMode  = v;
                else if (NexOS.Sys.StrEq(key, "pixelscale"))  PixelScale = (v > 1) ? v : 1;
                else if (NexOS.Sys.StrEq(key, "pixelscan"))   PixelScan  = v;
                else if (NexOS.Sys.StrEq(key, "termcellh"))   { TermCellH = v; if (TermCellH < 12) TermCellH = 12; if (TermCellH > 28) TermCellH = 28; }
                else if (NexOS.Sys.StrEq(key, "termbgmode"))  TermBgMode = v;
            }
        }

        // Push the current pixel-mode settings to the kernel framebuffer
        // post-process.  Call after Load() and after any Settings change.
        public static void ApplyPixel()
        {
            Host.SetPixel(PixelMode, PixelScale, PixelScan);
        }

        // Decimal (optionally signed) integer parse.  No BCL int.Parse in
        // MiniCLR, so this is a hand-rolled scanner over the managed string
        // API.  Stops at the first non-digit.
        private static int ParseInt(string s)
        {
            int n = NexOS.Sys.StrLen(s), i = 0, v = 0, sign = 1;
            while (i < n && (NexOS.Sys.StrCharAt(s, i) == ' ' ||
                             NexOS.Sys.StrCharAt(s, i) == '\t')) i++;
            if (i < n && NexOS.Sys.StrCharAt(s, i) == '-') { sign = -1; i++; }
            while (i < n) {
                char c = NexOS.Sys.StrCharAt(s, i);
                if (c < '0' || c > '9') break;
                v = v * 10 + (c - '0');
                i++;
            }
            return v * sign;
        }
    }

    // -----------------------------------------------------------------
    //  Tex - texture ids shared with the native host (gui.cpp / WinHost).
    //  These map to sfs_files/tex_*.tex (see tools/tex_pack.py).  Any id
    //  whose asset is missing simply reports HasImage()==0 and the paint
    //  code falls back to the flat theme colours.
    // -----------------------------------------------------------------
    public static class Tex
    {
        public const int Wall   = 0;
        public const int Task   = 1;
        public const int Menu   = 2;
        public const int Chrome = 3;
        public const int WinBg  = 4;
        public const int Icon   = 100;   // + Kind
    }

    // -----------------------------------------------------------------
    //  U -- tiny utilities the missing BCL would otherwise provide.
    // -----------------------------------------------------------------
    public static class U
    {
        public static string I(int v) { return NexOS.Sys.IntToStr(v); }

        // Linearly interpolate a packed 0xRRGGBB colour.  t is 0..1000.
        public static uint LerpColor(uint c0, uint c1, int t)
        {
            if (t <= 0) return c0;
            if (t >= 1000) return c1;
            int r0 = (int)((c0 >> 16) & 0xFF), g0 = (int)((c0 >> 8) & 0xFF), b0 = (int)(c0 & 0xFF);
            int r1 = (int)((c1 >> 16) & 0xFF), g1 = (int)((c1 >> 8) & 0xFF), b1 = (int)(c1 & 0xFF);
            int r = r0 + ((r1 - r0) * t) / 1000;
            int g = g0 + ((g1 - g0) * t) / 1000;
            int b = b0 + ((b1 - b0) * t) / 1000;
            return (uint)((r << 16) | (g << 8) | b);
        }

        // Re-pack a colour with an explicit alpha byte (0..255).
        public static uint Alpha(uint c, int a)
        {
            if (a < 0) a = 0; if (a > 255) a = 255;
            return (uint)((a << 24) | (c & 0x00FFFFFF));
        }

        // Fade a colour by `t` (0..1000) from fully transparent -> opaque,
        // preserving its original alpha at t=1000.
        public static uint Fade(uint c, int t)
        {
            if (t <= 0) return (c & 0x00FFFFFF);          // alpha 0
            if (t >= 1000) return c;
            int a = (int)((c >> 24) & 0xFF);
            int na = (a * t) / 1000;
            return (uint)((na << 24) | (c & 0x00FFFFFF));
        }

        public static string Cat(string a, string b) { return NexOS.Sys.StrConcat(a, b); }
        public static string Cat(string a, string b, string c)
        { return NexOS.Sys.StrConcat(NexOS.Sys.StrConcat(a, b), c); }
        public static string Cat(string a, string b, string c, string d)
        { return NexOS.Sys.StrConcat(NexOS.Sys.StrConcat(a, b), NexOS.Sys.StrConcat(c, d)); }
        public static string Cat(string a, string b, string c, string d, string e)
        { return NexOS.Sys.StrConcat(Cat(a, b, c, d), e); }
        public static string Cat(string a, string b, string c, string d, string e, string f)
        { return NexOS.Sys.StrConcat(Cat(a, b, c, d, e), f); }
        public static string Cat(string a, string b, string c, string d, string e, string f, string g)
        { return NexOS.Sys.StrConcat(Cat(a, b, c, d, e), NexOS.Sys.StrConcat(f, g)); }

        // Single-allocation substring / newline flattening.  ALWAYS prefer
        // these over a `for (...) r = Cat(r, CharStr(s[i]))` loop: that
        // pattern is O(n^2) BYTES on the CLR bump heap and exhausting the
        // heap faults the managed shell.
        public static string Sub(string s, int start, int len)
        { return NexOS.Sys.StrSub(s, start, len); }
        public static string Flat(string s) { return NexOS.Sys.StrFlat(s); }

        // Point-in-rect test used by every click handler.
        public static bool In(int mx, int my, int x, int y, int w, int h)
        { return mx >= x && mx < x + w && my >= y && my < y + h; }

        // True when the file name ends in ".exe" (case-insensitive).  An
        // .exe is a PROGRAM, not a document: the shell runs it through the
        // kernel's PE loader instead of opening it in Notepad.  Written
        // with plain char compares because the interpreter has no
        // string.EndsWith / ToLower.
        public static bool IsExe(string nm)
        {
            if (nm == null) return false;
            int n = nm.Length;
            if (n < 5) return false;                    // shortest is "a.exe"
            if (nm[n - 4] != '.') return false;
            char c1 = nm[n - 3], c2 = nm[n - 2], c3 = nm[n - 1];
            if (c1 != 'e' && c1 != 'E') return false;
            if (c2 != 'x' && c2 != 'X') return false;
            if (c3 != 'e' && c3 != 'E') return false;
            return true;
        }

        // Shift a packed 0xRRGGBB colour lighter (+d) or darker (-d).
        public static uint Shade(uint col, int d)
        {
            int r = (int)((col >> 16) & 0xFF) + d;
            int g = (int)((col >> 8) & 0xFF) + d;
            int b = (int)(col & 0xFF) + d;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            return ((uint)r << 16) | ((uint)g << 8) | (uint)b;
        }

        // A right-padded thousands-free KB/MB label, e.g. 2048 -> "2 MB".
        public static string Mb(int kb)
        {
            if (kb >= 1024) return Cat(I(kb / 1024), " MB");
            return Cat(I(kb), " KB");
        }
    }

    // -----------------------------------------------------------------
    //  W -- immediate-mode widgets.  Each Draw* call renders one control
    //  from the state passed in; hit-testing is the caller's job via
    //  U.In, so nothing is retained between frames.  This is what keeps
    //  the toolkit compatible with a rewinding heap.
    // -----------------------------------------------------------------
    public static class W
    {
        public const int RowH = 34;

        // Flat window backdrop.
        public static void Clear() {
            if (Gfx.HasImage(Tex.WinBg) != 0) Gfx.Image(Tex.WinBg, 0, 0, Gfx.Width(), Gfx.Height());
            else Gfx.FillRect(0, 0, Gfx.Width(), Gfx.Height(), C.WinBg);
        }

        // A rounded card with a hairline border.
        public static void Card(int x, int y, int w, int h)
        {
            Gfx.FillRound(x, y, w, h, 8, C.Card);
            Gfx.DrawRound(x, y, w, h, 8, C.Border);
        }

        public static void Panel(int x, int y, int w, int h, uint bg)
        { Gfx.FillRound(x, y, w, h, 8, bg); }

        public static void Label(int x, int y, string s) { Gfx.Text(x, y, s, C.Text); }
        public static void Sub(int x, int y, string s)   { Gfx.Text(x, y, s, C.TextSub); }

        // Section heading with an accent tick to its left.
        public static void Header(int x, int y, string s)
        {
            Gfx.FillRound(x, y + 1, 4, 16, 2, C.Accent);
            Gfx.Text(x + 12, y, s, C.Text);
        }

        // Primary (filled) button.  hover/press recolour the fill; a press
        // plays the Btn shrink-to-half-and-restore animation.
        public static void Primary(int x, int y, int w, int h, string label)
        {
            int sx = x, sy = y, sw = w, sh = h;
            int sc = Btn.ScaleAt(x + Gfx.OriginX(), y + Gfx.OriginY(), w, h);
            if (sc != 100)
            {
                sw = w * sc / 100; sh = h * sc / 100;
                sx = x + (w - sw) / 2; sy = y + (h - sh) / 2;
            }
            uint fill = U.LerpColor(C.Accent, C.AccentHi, Hover(x, y, w, h));
            Gfx.FillRound(sx, sy, sw, sh, 6, fill);
            Gfx.TextCenter(sx, sy + (sh - 16) / 2, sw, label, C.White);
            if (App.Current != null) App.Current.RegisterHit(0, x, y, w, h);
        }

        // Secondary (outlined) button.
        public static void Button(int x, int y, int w, int h, string label)
        {
            int sx = x, sy = y, sw = w, sh = h;
            int sc = Btn.ScaleAt(x + Gfx.OriginX(), y + Gfx.OriginY(), w, h);
            if (sc != 100)
            {
                sw = w * sc / 100; sh = h * sc / 100;
                sx = x + (w - sw) / 2; sy = y + (h - sh) / 2;
            }
            uint fill = U.LerpColor(C.Card, C.Hover, Hover(x, y, w, h));
            Gfx.FillRound(sx, sy, sw, sh, 6, fill);
            Gfx.DrawRound(sx, sy, sw, sh, 6, C.BorderMid);
            Gfx.TextCenter(sx, sy + (sh - 16) / 2, sw, label, C.Text);
            if (App.Current != null) App.Current.RegisterHit(0, x, y, w, h);
        }

        // Big square keypad key (calculator).  accent=true tints it blue.
        public static void Key(int x, int y, int w, int h, string label, bool accent)
        {
            uint fill;
            if (accent) fill = U.LerpColor(C.Accent, C.AccentHi, Hover(x, y, w, h));
            else        fill = U.LerpColor(C.Card, C.Hover, Hover(x, y, w, h));
            uint fg = accent ? C.White : C.Text;
            int sx = x, sy = y, sw = w, sh = h;
            int sc = Btn.ScaleAt(x + Gfx.OriginX(), y + Gfx.OriginY(), w, h);
            if (sc != 100)
            {
                sw = w * sc / 100; sh = h * sc / 100;
                sx = x + (w - sw) / 2; sy = y + (h - sh) / 2;
            }
            Gfx.FillRound(sx, sy, sw, sh, 6, fill);
            if (!accent) Gfx.DrawRound(sx, sy, sw, sh, 6, C.Border);
            Gfx.TextCenter(sx, sy + (sh - 16) / 2, sw, label, fg);
            if (App.Current != null) App.Current.RegisterHit(0, x, y, w, h);
        }

        // A single list row; selected rows get an accent bar + tint.
        public static void Row(int x, int y, int w, string text, bool selected)
        {
            if (selected)
            {
                Gfx.FillRound(x, y, w, RowH - 2, 6, C.Sel);
                Gfx.FillRound(x + 3, y + 6, 3, RowH - 14, 2, C.Accent);
            }
            else
            {
                uint fc = U.LerpColor(C.Card, C.Hover, Hover(x, y, w, RowH - 2));
                Gfx.FillRound(x, y, w, RowH - 2, 6, fc);
            }
            Gfx.Text(x + 14, y + (RowH - 2 - 16) / 2, text, C.Text);
            if (App.Current != null) App.Current.RegisterHit(0, x, y, w, RowH - 2);
        }

        // Labelled progress meter (memory, disk, etc.).
        public static void Meter(int x, int y, int w, string label, int pct, uint fill)
        {
            Gfx.Text(x, y, label, C.TextSub);
            Gfx.Text(x + w - 40, y, U.Cat(U.I(pct), "%"), C.Text);
            Gfx.Progress(x, y + 20, w, 10, pct, fill);
        }

        // True when the pointer is inside the rect this frame.
        public static bool Hot(int x, int y, int w, int h)
        {
            int mx = Gfx.MouseX(), my = Gfx.MouseY();
            return mx >= x && mx < x + w && my >= y && my < y + h;
        }

        // Animated hover amount 0..1000 (lerps over 150ms via Anim.Hover).
        // Use this for colour/scale transitions instead of the hard Hot() bool.
        public static int Hover(int x, int y, int w, int h)
        {
            int k = Anim.Key(x, y, w, h);
            int hot = Hot(x, y, w, h) ? 1 : 0;
            return Anim.Hover(k, hot);
        }

        // -----------------------------------------------------------------
        //  Voice binding -- the ONLY way a control becomes voice-enabled.
        //  Callers that pass a non-empty `cmd` opt this control in; every
        //  other control stays silent (the default).  The rect is the same
        //  one the control was drawn with, so the synthetic click lands
        //  dead-centre on it.  The Voice engine decides window-local vs
        //  screen coordinates from App.Current (null == desktop surface).
        // -----------------------------------------------------------------
        public static void Voice(string cmd, int x, int y, int w, int h)
        {
            if (cmd == null) return;
            if (NexOS.Sys.StrLen(cmd) == 0) return;
            NexOS.Forms.Voice.Register(cmd, x, y, w, h);
        }

        // -----------------------------------------------------------------
        //  Animated state controls.  Each takes a stable `id` (use a unique
        //  constant per control, or Anim.Key(x,y,w,h) for fixed layouts) and
        //  interpolates its visual state via Anim so toggling is never a hard
        //  cut.  Callers own the click handling (these only draw + hit-test).
        // -----------------------------------------------------------------

        // ToggleSwitch.  Knob slides with a Back-overshoot; track colour
        // lerps from BorderMid (off) to Accent (on) over 200ms.
        public static int Toggle(int x, int y, int w, int h, int on, int id)
        {
            int tgt = (on != 0) ? 1000 : 0;
            Anim.Set(id, tgt, 200, 1);                 // 1 = Back easing
            int t = Anim.Get(id);
            if (t < 0) t = 0; if (t > 1000) t = 1000;
            uint track = U.LerpColor(C.BorderMid, C.Accent, t);
            int r = h / 2;
            Gfx.FillRound(x, y, w, h, r, track);
            int kx = x + r + ((w - h) * t) / 1000;     // knob centre x
            int ky = y + r;
            Gfx.FillCircle(kx, ky, r - 2, C.White);
            return Hot(x, y, w, h) ? 1 : 0;
        }

        // CheckBox.  Box recolours to Accent when on; the check mark scales in
        // with a Back overshoot.
        public static int Check(int x, int y, int s, int on, int id)
        {
            int tgt = (on != 0) ? 1000 : 0;
            Anim.Set(id, tgt, 120, 1);
            int t = Anim.Get(id);
            if (t < 0) t = 0; if (t > 1000) t = 1000;
            uint box = U.LerpColor(C.BorderMid, C.Accent, t);
            int r = s / 2;
            Gfx.FillRound(x, y, s, s, 4, box);
            // check mark: two segments meeting at the centre, scaled by t
            int cx = x + r, cy = y + r;
            int len = (s * t) / 1000;
            if (len > 2)
            {
                int x1 = cx - s / 4, y1 = cy + s / 8;
                int x2 = cx - s / 10, y2 = cy + s / 4;
                int x3 = cx + s / 3, y3 = cy - s / 6;
                Gfx.DrawLine(x1, y1, x2, y2, C.White);
                Gfx.DrawLine(x2, y2, x3, y3, C.White);
            }
            return Hot(x, y, s, s) ? 1 : 0;
        }

        // RadioButton.  Outer ring; filled centre dot scales in when selected.
        public static int Radio(int x, int y, int s, int on, int id)
        {
            int tgt = (on != 0) ? 1000 : 0;
            Anim.Set(id, tgt, 120, 1);
            int t = Anim.Get(id);
            if (t < 0) t = 0; if (t > 1000) t = 1000;
            int r = s / 2;
            Gfx.DrawCircle(x + r, y + r, r - 1, C.BorderMid);
            if (t > 0)
            {
                int dr = (r * t) / 1000;
                Gfx.FillCircle(x + r, y + r, dr, C.Accent);
            }
            return Hot(x, y, s, s) ? 1 : 0;
        }

        // Slider.  Track + thumb; thumb grows on hover (1.0->1.15).  The value
        // (`pct` 0..100) is supplied by the caller and tracked live (no easing,
        // so dragging stays glued to the pointer).
        public static int Slider(int x, int y, int w, int h, int pct, int id)
        {
            int r = h / 2;
            Gfx.FillRound(x, y, w, h, r, C.BorderMid);
            int filled = (w * pct) / 100;
            Gfx.FillRound(x, y, filled < r ? r : filled, h, r, C.Accent);
            int hv = Hover(x, y, w, h);
            int tr = r - 2 + (r * 15 * hv) / 10000;   // up to +15% on hover
            int tx = x + (w * pct) / 100;
            Gfx.FillCircle(tx, y + r, tr, C.White);
            Gfx.DrawCircle(tx, y + r, tr, C.BorderMid);
            return Hot(x, y, w, h) ? 1 : 0;
        }

        // ProgressBar.  The displayed fill eases toward `pct` (0..100) over
        // 300ms so value changes glide instead of snapping.
        public static int Progress(int x, int y, int w, int h, int pct, uint c, int id)
        {
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            Anim.Set(id, pct * 10, 300, 0);
            int disp = Anim.Get(id) / 10;
            if (disp < 0) disp = 0; if (disp > 100) disp = 100;
            Gfx.Progress(x, y, w, h, disp, c);
            return disp;
        }
    }

    // -----------------------------------------------------------------
    //  VK -- virtual-key codes the input layer routes to App.OnKey when a
    //  key has no literal character (Ctrl/Alt combos, arrows, F-keys...).
    //  Positive values are literal codepoints; these negatives are the
    //  synthetic codes ShellForm (WinHost) and the kernel bridge emit.
    //  Shared by TerminalApp and every other consumer.
    // -----------------------------------------------------------------
    public static class VK
    {
        public const int Back   = 8;
        public const int Enter  = 13;
        public const int Esc    = 27;
        public const int Delete = -26;   // forward delete
        public const int CtrlC  = -3;    // copy selection, else SIGINT
        public const int CtrlV  = -4;    // paste
        public const int CtrlZ  = -5;    // undo (TBox) / suspend (term)
        public const int CtrlA  = -6;    // home (term) / select-all (TBox)
        public const int CtrlE  = -7;    // end
        public const int CtrlU  = -8;    // clear to line start
        public const int CtrlK  = -9;    // clear to line end
        public const int CtrlW  = -10;   // delete word back
        public const int CtrlL  = -11;   // clear screen
        public const int CtrlD  = -12;   // EOF / close if empty
        public const int CtrlR  = -13;   // reverse-i-search
        public const int AltF   = -15;   // word forward
        public const int AltB   = -16;   // word back
        public const int Tab    = -17;   // completion
        public const int Up = -18, Down = -19, Left = -20, Right = -21;
        public const int HomeK = -22, EndK = -23, PageUp = -24, PageDown = -25;
        public const int CsC = -30, CsV = -31, CsT = -32, CsW = -33;
        public const int F1 = -40, F2 = -41, F3 = -42, F4 = -43, F5 = -44,
                         F6 = -45, F7 = -46, F8 = -47, F9 = -48, F10 = -49,
                         F11 = -50, F12 = -51;
        // Terminal zoom (Ctrl +/-/0).  Must not collide with the -3..-33 and
        // -40..-51 ranges already used above.
        public const int CtrlPlus  = -60;   // zoom in  (Ctrl '+')
        public const int CtrlMinus = -61;   // zoom out (Ctrl '-')
        public const int Ctrl0     = -62;   // zoom reset
    }

    // -----------------------------------------------------------------
    //  App -- base class for every window.  The shell keeps one instance
    //  per open window and dispatches to these virtuals.
    // -----------------------------------------------------------------
    public class App
    {
        public int id;                         // window id, set by Shell.Open
        public int KindId;                     // launch kind, set by Shell.Open
        public virtual void OnPaint() { }
        public virtual void OnClick(int mx, int my) { }
        public virtual void OnKey(int ch) { }
        // New input surfaces the terminal emulator needs.  Default to
        // no-ops so every existing app keeps compiling and behaving.
        public virtual void OnMouseDown(int btn, int mx, int my) { }
        public virtual void OnMouseUp(int btn, int mx, int my) { }
        public virtual void OnMouseMove(int mx, int my) { }
        public virtual void OnWheel(int dy) { }
        public virtual string GetTitle() { return "App"; }

        // Context-menu hooks: the selected file in a file-browser window.
        public virtual string SelectedFile() { return ""; }
        public virtual int    SelectedFs()   { return -1; }
        public virtual int    SelectedIsDir(){ return 0; }
        // Right-click entry point (kernel-native context menus).
        // mx,my are window-local; ox,oy the window's screen origin.
        // Default: if the cursor is over a control registered during the
        // last Paint, pop a generic "Refresh / Close" menu so no button
        // is a dead zone.  Apps override for richer behaviour (e.g. the
        // File Explorer shows a file-action menu on its rows).
        public virtual void OnRightClick(int mx, int my, int ox, int oy)
        {
            if (HitControl(mx, my) >= 0) Desktop.OpenWinMenu(id, ox + mx, oy + my);
        }
        // Perform a file-action code from the file context menu.
        public virtual void DoFileAction(int code) { }
        // Perform a generic window-context action (Alt+Space menu).
        // Refresh / Close are handled directly; the geometry-bearing
        // actions (Minimize / Maximize / Restore / Move / Size) are
        // delegated to the host, which owns window position in both the
        // VM (gui.cpp) and the WinForms harness (ShellForm.ToggleMax /
        // WinRec.Minimized).  Move / Size are live drag gestures the
        // immediate-mode shell cannot capture, so the host surfaces them
        // as a no-op when unsupported - the menu still closes cleanly.
        public virtual void DoWinAction(int code)
        {
            if (code == WAct.Refresh)      OnCtxRefresh();
            else if (code == WAct.Close)   Shell.Close(id);
            else if (code == WAct.Minimize) Host.WinAction(id, WAct.Minimize);
            else if (code == WAct.Maximize) Host.WinAction(id, WAct.Maximize);
            else if (code == WAct.Restore)  Host.WinAction(id, WAct.Restore);
            else if (code == WAct.Move || code == WAct.Size)
                Host.WinAction(id, code);
        }
        // Override to reload window content on "Refresh".
        public virtual void OnCtxRefresh() { }

        // ---- immediate-mode hit registry -------------------------------
        // The W.* draw helpers register every control rect they paint
        // into these instance arrays.  The heap rewinds each frame, but
        // instance state survives, so the rects from the last Paint stay
        // valid for the next right-click.  OnRightClick consults them so
        // a right-click lands on the very control the user is pointing at.
        public static App  Current;           // set by Shell before Paint/Click
        public int[] hitId, hitX, hitY, hitW, hitH;
        public int    hitN;
        public App()
        {
            hitId = new int[64]; hitX = new int[64]; hitY = new int[64];
            hitW = new int[64]; hitH = new int[64]; hitN = 0;
        }
        public void RegisterHit(int id, int x, int y, int w, int h)
        {
            if (hitN >= 64) return;
            hitId[hitN] = id; hitX[hitN] = x; hitY[hitN] = y;
            hitW[hitN] = w; hitH[hitN] = h; hitN++;
        }
        public int HitControl(int mx, int my)
        {
            for (int i = 0; i < hitN; i++)
                if (U.In(mx, my, hitX[i], hitY[i], hitW[i], hitH[i])) return hitId[i];
            return -1;
        }
    }

    // Action codes for the generic window context menu (Alt+Space / title
    // bar right-click).  Mirrors the Win11 window menu: Restore / Move /
    // Size / Minimize / Maximize / Close - with the greyed states the OS
    // shows for the current window state.
    public static class WAct
    {
        public const int Restore  = 10;   // un-maximise / un-minimise
        public const int Move     = 11;   // enter drag-move (host gesture)
        public const int Size     = 12;   // enter drag-resize (host gesture)
        public const int Minimize = 13;   // minimise to taskbar
        public const int Maximize = 14;   // maximise to fill
        public const int Refresh  = 15;   // re-run OnCtxRefresh()
        public const int Close    = 17;   // Shell.Close(id)
    }
}
