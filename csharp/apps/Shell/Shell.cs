// =====================================================================
//  Shell.cs  -  entry points the kernel (mforms.cpp) calls into
// ---------------------------------------------------------------------
//  One resident assembly, one dispatcher.  The kernel owns window
//  chrome, dragging and the taskbar; it asks the shell to instantiate an
//  app (Open), then forwards paint and input to that app by id.  All
//  per-window state lives here on the managed side.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    // Application kinds.  Values agree with the native AppType order so
    // gui.cpp can map an icon straight to Shell.Open(kind).
    public static class Kind
    {
        public const int ControlPanel = 0;
        public const int FileExplorer = 1;
        public const int TaskManager  = 2;
        public const int Terminal     = 3;
        public const int Calculator   = 4;
        public const int About        = 5;
        public const int MemOptimizer = 6;
        public const int Notepad      = 7;
        public const int Browser      = 8;
        public const int AiSetup      = 9;   // one-tap AI enablement wizard
        public const int AiAgent      = 10;  // AI Agent runner (Planner/Actor/Critic)
        public const int Demo         = 11;  // button-shrink + reply "啊" demo window
    }

    public static class Shell
    {
        const int MAX = 16;
        static App[] apps;      // live window instances, indexed by id
        static int   count;     // high-water mark of ids handed out

        // Carried request for the next ControlPanel / Notepad instance:
        // lets the host open a settings page or a specific file.
        static int   pendingSettingsPage = -1;
        static string pendingNotepadFile = null;

        // ---- lifecycle ------------------------------------------------
        public static void Init()
        {
            Lang.Init();   // load language pack before any Lang.T() call
            // Static field initialisers never run under MiniCLR, so every
            // Theme field would stay 0 (black wallpaper, no accent).  Prime
            // the defaults here; Settings > ApplyTheme() overrides later.
            Theme.WallTop    = 0x218FD9;   // wallpaper gradient (Win11 blue)
            Theme.WallBot    = 0x05216B;
            Theme.Accent     = 0x0078D4;   // Fluent blue
            Theme.Dark       = 1;            // dark is the default shipped theme
            Theme.TaskbarLeft = 0;
            Theme.ShowLabels = 1;
            Theme.ActiveNet  = 0;
            // Master microphone switch.  When 0 NOTHING registers or fires;
            // when 1, only the controls explicitly tagged with W.Voice(...)
            // respond.  Toggle at runtime with the `voice on` / `voice off`
            // shell commands.  Default ON so the curated seed controls work
            // out of the box; untagged controls never respond regardless.
            Theme.VoiceOn    = 1;
            Theme.DesktopMode = 0;   // clean Win11 desktop by default; toggle in R-click / Settings
            Theme.PixelMode  = 1;    // retro pixel / CRT-monitor look ON by default
            Theme.PixelScale = 1;    // full spatial detail (no chunkiness) by default
            Theme.PixelScan  = 0;    // scanlines off by default (toggle in Settings)

            // Re-apply any persisted personalization from a previous session
            // (nexos.cfg on the MKFS data disk).  Safe no-op if the file is
            // missing or empty.
            Theme.Load();
            Theme.ApplyPixel();      // push pixel-mode settings to the kernel

            apps = new App[MAX];   // heap_alloc zeroes -> all null
            count = 0;
            Voice.Init();           // allocate the voice registry / queue
            Desktop.Init();        // wallpaper / icon / taskbar tables
            Login.Init();          // lock screen (no-op if already signed in)
            Toast.Init();          // transient notification stack
            Toast.Show("NexOS", "系统就绪", 3000);
            Host.Log("NexOS.Forms.Shell initialised");
        }

        // Instantiate an app and return its window id, or -1 on overflow.
        public static int Open(int kind)
        {
            int id = -1;
            for (int i = 0; i < MAX; i++) { if (apps[i] == null) { id = i; break; } }
            if (id < 0) return -1;

            App a = Make(kind);
            if (a == null) return -1;
            a.id = id;
            a.KindId = kind;
            apps[id] = a;
            if (id + 1 > count) count = id + 1;
            return id;
        }

        static App Make(int kind)
        {
            if (kind == Kind.Calculator)   return new CalculatorApp();
            if (kind == Kind.About)        return new AboutApp();
            if (kind == Kind.TaskManager)  return new TaskManagerApp();
            if (kind == Kind.FileExplorer) return new FileExplorerApp();
            if (kind == Kind.ControlPanel) return new ControlPanelApp();
            if (kind == Kind.Terminal)     return new TerminalApp();
            if (kind == Kind.MemOptimizer) return new MemOptimizerApp();
            if (kind == Kind.Notepad)      return new NotepadApp();
            if (kind == Kind.Browser)      return new BrowserApp();
            if (kind == Kind.AiSetup)      return new AiSetupApp();
            if (kind == Kind.AiAgent)      return new AiAgentApp();
            if (kind == Kind.Demo)         return new DemoApp();
            return null;
        }

        public static void Close(int id)
        {
            if (id >= 0 && id < MAX) apps[id] = null;
        }

        // Close every open window whose launch kind matches (used when an
        // app is uninstalled from the Control Panel so its windows vanish too).
        public static void CloseKind(int kind)
        {
            for (int i = 0; i < MAX; i++)
            {
                App a = apps[i];
                if (a != null && a.KindId == kind) apps[i] = null;
            }
        }

        // Return the live window instance by id (null if none).  Used by
        // the WinHost --termtest harness to drive and introspect a window.
        public static App Get(int id)
        {
            if (id < 0 || id >= MAX) return null;
            return apps[id];
        }

        // ---- per-frame dispatch --------------------------------------
        public static void Paint(int id, int w, int h)
        {
            if (id < 0 || id >= MAX) return;
            App a = apps[id];
            if (a != null) { App.Current = a; a.hitN = 0; a.OnPaint(); }
        }

        public static int Click(int id, int mx, int my)
        {
            if (id < 0 || id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnClick(mx, my);
            return 1;
        }

        public static int Key(int id, int ch)
        {
            if (id < 0 || id >= MAX) { return 0; }
            App a = apps[id];
            if (a == null) { return 0; }
            a.OnKey(ch);
            return 1;
        }

        // New input surfaces for the terminal emulator.  They mirror Click
        // (set App.Current, guard the id, dispatch to the virtual) so the
        // kernel bridge and the WinHost can feed richer events without any
        // behavioural change to apps that don't override them.
        public static int MouseDown(int id, int btn, int mx, int my)
        {
            if (id < 0 || id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnMouseDown(btn, mx, my);
            return 1;
        }
        public static int MouseUp(int id, int btn, int mx, int my)
        {
            if (id < 0 || id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnMouseUp(btn, mx, my);
            return 1;
        }
        public static int MouseMove(int id, int mx, int my)
        {
            if (id < 0 || id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnMouseMove(mx, my);
            return 1;
        }
        public static int Wheel(int id, int dy)
        {
            if (id < 0 || id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnWheel(dy);
            return 1;
        }

        // Right-click entry point.  mx,my are window-local; ox,oy the
        // window's screen origin (so the popup can be positioned).
        public static void RightClick(int id, int mx, int my, int ox, int oy)
        {
            if (id < 0 || id >= MAX) return;
            App a = apps[id];
            if (a != null) a.OnRightClick(mx, my, ox, oy);
        }

        // Forward a file-context-menu action to the owning window.
        public static void FileAction(int id, int code)
        {
            if (id < 0 || id >= MAX) return;
            App a = apps[id];
            if (a != null) a.DoFileAction(code);
        }

        // Forward a generic window-context action (Refresh / Close) to
        // the owning window.
        public static void WinAction(int id, int code)
        {
            if (id < 0 || id >= MAX) return;
            App a = apps[id];
            if (a != null) a.DoWinAction(code);
        }

        public static string Title(int id)
        {
            if (id < 0 || id >= MAX) return "";
            App a = apps[id];
            if (a == null) return "";
            return a.GetTitle();
        }

        // ---- desktop surface ------------------------------------------
        // The whole Win11 shell -- wallpaper, icons, taskbar, Start menu
        // -- is managed code in Desktop.cs.  gui.cpp paints it in two
        // layers so windows sit between the wallpaper and the taskbar.
        //
        // The lock screen sits in front of all of it.  While Login is
        // active it draws the wallpaper layer itself and the overlay
        // (taskbar / Start menu) is suppressed entirely, so there is no
        // way to reach the desktop without a valid credential.
        public static int  HasDesktop() { return 1; }

        public static void PaintDesktop(int w, int h)
        {
            // Advance all control tweens once per frame (drives hover/press
            // transitions and keeps Host.SetAnim alive while anything moves).
            Anim.Tick();
            if (Login.IsActive() != 0) { Login.Paint(w, h); return; }
            // Start a fresh voice frame: clear last frame's control registry
            // (the desktop surface has no App instance, so force App.Current
            // null so W.Voice() registers entries as screen-coordinate
            // desktop controls rather than window controls).
            App.Current = null;
            Voice.BeginFrame();
            Desktop.Paint(w, h);
        }

        public static void PaintOverlay(int w, int h)
        {
            if (Login.IsActive() != 0) return;   // no taskbar while locked
            App.Current = null;
            Desktop.PaintOverlay(w, h);
            // All controls (desktop + windows + taskbar/Start) have
            // re-registered this frame; dispatch any pending voice phrases
            // as synthetic clicks on the matched control.
            Voice.Drain();
        }

        // -1 == "handled, do not open anything"; -2 == "not mine".
        public static int DesktopClick(int mx, int my)
        {
            if (Login.IsActive() != 0) { Login.Click(mx, my); return -1; }
            return Desktop.Click(mx, my);
        }

        // ---- voice engine bridge (called from the kernel via clr_call) --
        // These are the single, uniform ingestion points any interaction
        // backend uses: a recognised phrase (Say) or a master on/off (Set).
        public static void VoiceSay(string phrase) { Voice.Say(phrase); }
        public static void VoiceSet(int    on)     { Voice.SetEnabled(on != 0); }

        public static int  DesktopMenuOpen() { return Desktop.IsMenuOpen() | Desktop.ContextOpen(); }

        public static int DesktopRClick(int mx, int my)
        {
            if (Login.IsActive() != 0) return 0;   // no context menus while locked
            Desktop.OnRightClick(mx, my);
            return 0;
        }

        // Re-arm the lock screen (Start menu -> Sign out).
        public static void LockSession() { Login.Lock(); }

        // ---- context-menu / launcher helpers (used by the WinHost host) ---
        // Open the Control Panel pre-navigated to a page (Personalize,
        // Taskbar settings, ...).  page = -1 returns to the tile grid.
        public static void OpenSettings(int page)
        {
            pendingSettingsPage = page;
            Host.OpenApp(Kind.ControlPanel);
        }
        public static int TakeSettingsPage()
        { int p = pendingSettingsPage; pendingSettingsPage = -1; return p; }

        // Open Notepad with a specific file already loaded.  The kernel
        // must create the native window, so we hand the request back to
        // the host instead of Shell.Open() (which would only make a C#
        // instance with nothing to paint it).
        public static void OpenNotepad(string file)
        {
            Host.Log(U.Cat("[FILES] opening in Notepad: ", file));
            pendingNotepadFile = file;
            Host.OpenApp(Kind.Notepad);
        }
        public static string TakeNotepadFile()
        { string f = pendingNotepadFile; pendingNotepadFile = null; return f; }

        // Close every window of the given managed Kind (taskbar
        // right-click "Close window" / "End process").
        public static void CloseApp(int kind) { Host.CloseApp(kind); }

        // Leave GUI mode and return to the text terminal.
        public static void ExitGui() { Host.ExitGui(); }

        // The selected file in window `id`, for the File right-click menu.
        public static string FileContext(int id)
        {
            if (id < 0 || id >= MAX) return "";
            App a = apps[id];
            return a == null ? "" : a.SelectedFile();
        }
        public static int FileContextFs(int id)
        {
            if (id < 0 || id >= MAX) return -1;
            App a = apps[id];
            return a == null ? -1 : a.SelectedFs();
        }
        public static int FileContextDir(int id)
        {
            if (id < 0 || id >= MAX) return 0;
            App a = apps[id];
            return a == null ? 0 : a.SelectedIsDir();
        }
    }
}
