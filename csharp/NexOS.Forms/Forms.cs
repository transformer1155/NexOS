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
        // Bit i is set when a window of Kind i is open; drives the
        // running-app indicators under the taskbar buttons.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int RunningMask();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileCount(int fs);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string FileName(int fs, int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileIsDir(int fs, int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileRefresh();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string ReadText(int fs, string name);
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
    }
#endif // !WINHOST

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
    public static class C
    {
        public const uint WinBg     = 0xF3F3F3;   // window client background (mica-ish grey)
        public const uint Card      = 0xFFFFFF;   // raised surface
        public const uint CardAlt   = 0xFAFAFA;   // subtle alternate row
        public const uint Border    = 0xE1E1E1;   // hairline separators
        public const uint BorderMid = 0xCFCFCF;
        public const uint Accent    = 0x0078D4;   // Windows accent blue
        public const uint AccentHi  = 0x1A86D9;   // hovered accent
        public const uint AccentLo  = 0x005FB0;   // pressed accent
        public const uint Text      = 0x1B1B1B;   // primary text
        public const uint TextSub   = 0x606060;   // secondary text
        public const uint TextFaint = 0x909090;
        public const uint White     = 0xFFFFFF;
        public const uint Hover     = 0xEEF3FA;   // control hover fill
        public const uint Sel       = 0xE1EDFB;   // selected row fill
        public const uint Good      = 0x107C10;   // green (success)
        public const uint Warn      = 0xC29A00;   // amber
        public const uint Danger    = 0xC42B1C;   // red (destructive / stop)
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
        public static uint  WallTop   = 0x05162C;   // wallpaper gradient top
        public static uint  WallBot   = 0x0B4A83;   // wallpaper gradient base
        public static uint  Accent    = 0x0078D4;   // primary accent (Fluent blue)
        public static int    Dark      = 0;         // 0 light, 1 dark
        public static int    TaskbarLeft = 0;       // 0 centred, 1 left-aligned
        public static int    ShowLabels = 1;        // taskbar labels (reserved)
        public static int    ActiveNet = 0;         // 0 Ethernet, 1 Wi-Fi
        public static int    VoiceOn   = 0;         // microphone listening

        // A small Fluent accent ramp, indexed by the Settings swatches.
        public static uint[] Accents()
        {
            uint[] a = new uint[6];
            a[0] = 0x0078D4; a[1] = 0x8B5CF6; a[2] = 0x0EA5E9;
            a[3] = 0x107C10; a[4] = 0xE11D8A; a[5] = 0xF59E0B;
            return a;
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

        public static string Cat(string a, string b) { return NexOS.Sys.StrConcat(a, b); }
        public static string Cat(string a, string b, string c)
        { return NexOS.Sys.StrConcat(NexOS.Sys.StrConcat(a, b), c); }
        public static string Cat(string a, string b, string c, string d)
        { return NexOS.Sys.StrConcat(NexOS.Sys.StrConcat(a, b), NexOS.Sys.StrConcat(c, d)); }
        public static string Cat(string a, string b, string c, string d, string e)
        { return NexOS.Sys.StrConcat(Cat(a, b, c, d), e); }
        public static string Cat(string a, string b, string c, string d, string e, string f, string g)
        { return NexOS.Sys.StrConcat(Cat(a, b, c, d, e), NexOS.Sys.StrConcat(f, g)); }

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

        // Primary (filled) button.  hover/press recolour the fill.
        public static void Primary(int x, int y, int w, int h, string label)
        {
            uint fill = C.Accent;
            if (Hot(x, y, w, h)) fill = C.AccentHi;
            Gfx.FillRound(x, y, w, h, 6, fill);
            Gfx.TextCenter(x, y + (h - 16) / 2, w, label, C.White);
            if (App.Current != null) App.Current.RegisterHit(0, x, y, w, h);
        }

        // Secondary (outlined) button.
        public static void Button(int x, int y, int w, int h, string label)
        {
            uint fill = Hot(x, y, w, h) ? C.Hover : C.Card;
            Gfx.FillRound(x, y, w, h, 6, fill);
            Gfx.DrawRound(x, y, w, h, 6, C.BorderMid);
            Gfx.TextCenter(x, y + (h - 16) / 2, w, label, C.Text);
            if (App.Current != null) App.Current.RegisterHit(0, x, y, w, h);
        }

        // Big square keypad key (calculator).  accent=true tints it blue.
        public static void Key(int x, int y, int w, int h, string label, bool accent)
        {
            uint fill;
            if (accent) fill = Hot(x, y, w, h) ? C.AccentHi : C.Accent;
            else        fill = Hot(x, y, w, h) ? C.Hover    : C.Card;
            uint fg = accent ? C.White : C.Text;
            Gfx.FillRound(x, y, w, h, 6, fill);
            if (!accent) Gfx.DrawRound(x, y, w, h, 6, C.Border);
            Gfx.TextCenter(x, y + (h - 16) / 2, w, label, fg);
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
            else if (Hot(x, y, w, RowH - 2))
            {
                Gfx.FillRound(x, y, w, RowH - 2, 6, C.Hover);
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
    }

    // -----------------------------------------------------------------
    //  App -- base class for every window.  The shell keeps one instance
    //  per open window and dispatches to these virtuals.
    // -----------------------------------------------------------------
    public class App
    {
        public int id;                         // window id, set by Shell.Open
        public virtual void OnPaint() { }
        public virtual void OnClick(int mx, int my) { }
        public virtual void OnKey(int ch) { }
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
        // Perform a generic window-context action (Refresh / Close).
        public virtual void DoWinAction(int code)
        {
            if (code == WAct.Refresh) OnCtxRefresh();
            else if (code == WAct.Close) Shell.Close(id);
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

    // Action codes for the generic window context menu.
    public static class WAct
    {
        public const int Refresh = 15;   // re-run OnCtxRefresh()
        public const int Close   = 17;   // Shell.Close(id)
    }
}
