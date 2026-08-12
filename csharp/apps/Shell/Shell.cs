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
            // Static field initialisers never run under MiniCLR, so every
            // Theme field would stay 0 (black wallpaper, no accent).  Prime
            // the defaults here; Settings > ApplyTheme() overrides later.
            Theme.WallTop    = 0x05162C;   // wallpaper gradient (light theme)
            Theme.WallBot    = 0x0B4A83;
            Theme.Accent     = 0x0078D4;   // Fluent blue
            Theme.Dark       = 0;
            Theme.TaskbarLeft = 0;
            Theme.ShowLabels = 1;
            Theme.ActiveNet  = 0;
            Theme.VoiceOn    = 0;

            apps = new App[MAX];   // heap_alloc zeroes -> all null
            count = 0;
            Desktop.Init();        // wallpaper / icon / taskbar tables
            Login.Init();          // lock screen (no-op if already signed in)
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
            return null;
        }

        public static void Close(int id)
        {
            if (id >= 0 && id < MAX) apps[id] = null;
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
            if (Login.IsActive() != 0) { Login.Paint(w, h); return; }
            Desktop.Paint(w, h);
        }

        public static void PaintOverlay(int w, int h)
        {
            if (Login.IsActive() != 0) return;   // no taskbar while locked
            Desktop.PaintOverlay(w, h);
        }

        // -1 == "handled, do not open anything"; -2 == "not mine".
        public static int DesktopClick(int mx, int my)
        {
            if (Login.IsActive() != 0) { Login.Click(mx, my); return -1; }
            return Desktop.Click(mx, my);
        }

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
