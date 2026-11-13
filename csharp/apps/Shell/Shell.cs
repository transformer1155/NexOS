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
        public const int Demo         = 11;  // button-shrink + reply demo window
        public const int NetTools     = 12;  // network diagnostics (ipconfig/netstat/ping)
        public const int AiLauncher   = 13;  // AI command console / launcher
        public const int NetDefense    = 14;  // network routing & attack-defense sim
        public const int WebView       = 15;  // Linux guest VNC framebuffer viewer
        // ---- media players (added 2026-11-07) ----------------------------
        // Deliberately NOT pinned to the taskbar and NOT seeded onto the
        // desktop: the user asked for these to be reachable but not to clutter
        // the default surface, so they live only under Start > All apps.
        // Desktop.LoadDefaults() and the taskbar pin list must keep excluding
        // them; both iterate KINDS, so "excluded" means "no explicit Put()".
        public const int VideoPlayer    = 16;  // video file previewer/player
        public const int AudioPlayer    = 17;  // audio file previewer/player
        public const int AiChat         = 18;  // local GGUF model chat window
        public const int BugReport      = 19;  // APMS bug report (standalone)
        public const int AiWorkbench    = 20;  // AI workbench: sessions + ws_ tools + docs
    }

    public static class Shell
    {
        const int MAX = 16;
        static App[] apps;      // live window instances, indexed by id
        static int   count;     // high-water mark of ids handed out

        // Carried request for the next ControlPanel / Notepad instance:
        // lets the host open a settings page or a specific file.
        // NOTE: MiniCLR zero-fills statics and never runs field
        // initialisers / .cctor, so "no request" must be representable by
        // all-zero.  An `armed` flag does that; a plain "= -1" default
        // silently became 0 and forced the panel onto the System page.
        static int   pendingSettingsPage;
        static int   pendingSettingsArmed;
        static string pendingNotepadFile = null;
        static int    pendingNotepadFs   = 0;   // volume the file came from (0=MKFS, 3=Desktop)

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
            Theme.WidgetsOn  = 1;    // desktop telemetry widget ON by default

            // Re-apply any persisted personalization from a previous session
            // (nexos.cfg on the MKFS data disk).  Safe no-op if the file is
            // missing or empty.
            Theme.Load();
            // One-line boundary report for the gamma persistence path (see
            // Theme.GammaSelfCheck).  bad=0 means every case matched expectations.
            NexOS.Sys.Print("[SHELL] gamma selfcheck bad=");
            NexOS.Sys.PrintInt(Theme.GammaSelfCheck());
            NexOS.Sys.Print(" textgamma=");
            NexOS.Sys.PrintInt(Theme.TextGamma);
            Theme.ApplyPixel();      // push pixel-mode settings to the kernel

            // ---- OS API layer 0+1 managed self-test (one serial line) ----
            // Exercises the error channel, ABI probes and the Perm facade.
            // NOTE: the shell runs on the default token (no prefixes), so
            // FsCanPath allows everything; denial paths are covered by the
            // kernel-side osapi self-test (serial: "fails=0").
            {
                int ok = 1;
                Host.ClearError();
                Host.SetLastError(Err.Busy);
                if (Host.LastError() != Err.Busy) ok = 0;
                if (Host.ApiVersion() < 1) ok = 0;
                if (!Host.HasCap(Cap.Err) || !Host.HasCap(Cap.Perm)) ok = 0;
                if (Perm.Uid() < 0) ok = 0;
                if (Perm.FsCanPath("/data/calc/x", 1) == 0) ok = 0;
                int h = Host.HandleAlloc(1 /*HK_HEAP*/, 3, 1);
                if (h == 0) ok = 0;
                if (Host.HandleClose(h) != 1) ok = 0;
                if (Host.HandleClose(h) == 1) ok = 0;   // double close must fail
                if (Host.LastError() != Err.BadHandle) ok = 0;
                // ---- L2: mutex + event + job ----
                int mu = Sync.MutexNew();
                if (mu == 0 || Sync.Lock(mu) != 1) ok = 0;
                else if (Sync.Lock(mu) == 1) ok = 0;             // double lock must fail (Deadlk)
                else if (Sync.Unlock(mu) != 1) ok = 0;
                int ev = Sync.EventNew();
                Sync.EventSet(ev);
                if (Sync.EventWait(ev, 5, 1) != 1) ok = 0;
                if (Sync.EventWait(ev, 2, 1) == 1) ok = 0;       // cleared -> Timeout
                int jb = Job.Start(0, 10);
                if (jb == 0 || Job.Status(jb) != Job.Run) ok = 0;
                Job.Tick(10);                                     // complete it
                if (Job.Status(jb) != Job.Done || Job.Progress(jb) != 10) ok = 0;                // ---- L3: vfs resolve + atomic create ----
                string rp = Fs.Resolve("/a/b/../c");
                if (rp == null || !(rp == "/a/c")) ok = 0;
                string esc = Fs.Resolve("/../x");
                if (esc != null) ok = 0;                          // must refuse root escape
                int fo = File.Open("/osapi_t.txt", FileFlags.Creat | FileFlags.Excl | FileFlags.WR);
                if (fo != 0) {
                    int[] wb = new int[4];
                    wb[0] = 'N'; wb[1] = 'E'; wb[2] = 'X'; wb[3] = 'O';
                    if (File.Write(fo, wb, 4) != 4) ok = 0;
                    if (File.Close(fo) != 1) ok = 0;
                    int fo2 = File.Open("/osapi_t.txt", FileFlags.Creat | FileFlags.Excl | FileFlags.WR);
                    if (fo2 != 0 || Host.LastError() != Err.NameExists) ok = 0;   // hard: O_EXCL
                    int frd = File.Open("/osapi_t.txt", FileFlags.RD);
                    if (frd != 0) {
                        int[] rb = new int[4];
                        if (File.Read(frd, rb, 4) != 4) ok = 0;
                        if (rb[0] != 'N' || rb[3] != 'O') ok = 0;
                        File.Close(frd);
                    } else ok = 0;
                    File.Unlink("/osapi_t.txt");
                } else ok = 0;
                NexOS.Sys.Print("[SHELL] osapi l0l1 ok=");
                NexOS.Sys.PrintInt(ok);
                NexOS.Sys.Print(" abi=");
                NexOS.Sys.PrintInt(Host.ApiVersion());
                NexOS.Sys.Print("\n");
            }

            // ---- OS API L2 process + L3 glob managed self-test ----
            {
                int ok2 = 1;
                // create a real file, then glob for it (listing content is
                // cwd-dependent, so a static name would be flaky)
                int fh = File.Open("/mgt1.txt", FileFlags.Creat | FileFlags.Excl | FileFlags.WR);
                if (fh != 0){
                    int[] wb = new int[4]; wb[0]='t'; wb[1]='e'; wb[2]='s'; wb[3]='t';
                    File.Write(fh, wb, 4);
                    File.Close(fh);
                }
                string g = Fs.Glob("/", "*.txt");
                if (g == null || NexOS.Sys.StrLen(g) == 0) ok2 = 0;
                File.Unlink("/mgt1.txt");
                // wait/kill of a bad pid must report BadPid
                if (Proc.Wait(0) != Proc.BadPid) ok2 = 0;
                if (Proc.Kill(0) != 0) ok2 = 0;
                NexOS.Sys.Print("[SHELL] osapi proc glob ok=");
                NexOS.Sys.PrintInt(ok2);
                NexOS.Sys.Print("\n");
            }

            // ---- OS API L12: migrate legacy in-memory state into Cfg ----
            // Old state is only a DEFAULT source: any key already persisted
            // in Cfg wins.  Theme.Save()/Host.WriteText("assoc.cfg") keep
            // working unchanged; this is the first read of the new store.
            {
                int cs = Cfg.Open("shell");
                if (cs != 0)
                {
                    CfgSeedInt(cs, "dark",        Theme.Dark);
                    CfgSeedInt(cs, "pixelmode",   Theme.PixelMode);
                    CfgSeedInt(cs, "textgamma",   Theme.TextGamma);
                    CfgSeedInt(cs, "widgetson",   Theme.WidgetsOn);
                    CfgSeedInt(cs, "taskbarleft", Theme.TaskbarLeft);
                    Cfg.Flush(cs);
                    Cfg.Close(cs);
                }
                int ca = Cfg.Open("assoc");
                if (ca != 0)
                {
                    string old = Host.ReadText(0, "assoc.cfg");
                    if (old != null)
                    {
                        int i = 0, n = NexOS.Sys.StrLen(old);
                        while (i < n)
                        {
                            int j = i;
                            while (j < n && NexOS.Sys.StrCharAt(old, j) != '\n') j++;
                            int eq = -1;
                            for (int k = i; k < j; k++)
                                if (NexOS.Sys.StrCharAt(old, k) == '=') { eq = k; break; }
                            if (eq > i && eq + 1 < j)
                            {
                                string kk = NexOS.Sys.StrSub(old, i, eq - i);
                                string vv = NexOS.Sys.StrSub(old, eq + 1, j - eq - 1);
                                if (Cfg.Get(ca, kk) == null) Cfg.Set(ca, kk, vv);
                            }
                            i = j + 1;
                        }
                    }
                    Cfg.Flush(ca);
                    Cfg.Close(ca);
                }
            }

            // ---- OS API L12 managed self-test: Set -> Flush -> reopen -> Get ----
            {
                int ok3 = 1;
                int c = Cfg.Open("cfgt");
                if (c == 0) ok3 = 0;
                else
                {
                    if (Cfg.Set(c, "k", "v") != 1) ok3 = 0;
                    if (Cfg.Dirty(c) != 1) ok3 = 0;
                    if (Cfg.Flush(c) != 1) ok3 = 0;
                    if (Cfg.Dirty(c) != 0) ok3 = 0;
                    int d = Cfg.Open("cfgt");
                    if (d == 0) ok3 = 0;
                    else
                    {
                        string gv = Cfg.Get(d, "k");
                        if (gv == null || !(gv == "v")) ok3 = 0;
                        if (Cfg.Count(d) < 1) ok3 = 0;
                        Cfg.Close(d);
                    }
                    Cfg.Close(c);
                }
                if (!Host.HasCap(Cap.Cfg)) ok3 = 0;
                NexOS.Sys.Print("[SHELL] osapi L12 cfg ok=");
                NexOS.Sys.PrintInt(ok3);
                NexOS.Sys.Print("\n");
            }

            // ---- OS API L4 managed self-test ----------------------------
            // The kernel-side pure-C self-test asserts the numeric gen++
            // already; this one proves the managed icall path sees the same
            // stale-handle behaviour (no pointers ever cross the boundary).
            {
                int ok4 = 1;
                int h = Mem.HeapAlloc(64);
                if (h == 0) ok4 = 0;
                else
                {
                    if (Mem.HeapSize(h) != 64) ok4 = 0;
                    if (Mem.HeapPoke(h, 0, 90) != 1) ok4 = 0;
                    if (Mem.HeapPeek(h, 0) != 90) ok4 = 0;
                    int h2 = Mem.HeapRealloc(h, 128);
                    if (h2 == 0) { ok4 = 0; Mem.HeapFree(h); }
                    else
                    {
                        if (Mem.HeapSize(h2) != 128) ok4 = 0;
                        if (Mem.HeapPeek(h2, 0) != 90) ok4 = 0;   // data kept
                        if (Mem.HeapSize(h) != 0) ok4 = 0;          // old h dead
                        if (Mem.HeapFree(h2) != 1) ok4 = 0;
                        if (Mem.HeapFree(h2) != 0) ok4 = 0;         // double free
                        int e = Host.LastError();
                        if (!(e == Err.BadHandle || e == Err.Stale)) ok4 = 0;
                    }
                }
                int ar = Mem.ArenaNew(256);
                if (ar == 0) ok4 = 0;
                else
                {
                    if (Mem.ArenaAlloc(ar, 32) != 0) ok4 = 0;
                    int m = Mem.ArenaMark(ar);
                    if (m != 32) ok4 = 0;
                    if (Mem.ArenaAlloc(ar, 64) != 32) ok4 = 0;
                    if (Mem.ArenaRelease(ar, m) != 1) ok4 = 0;
                    if (Mem.ArenaUsed(ar) != 32) ok4 = 0;
                    if (Mem.ArenaFree(ar) != 1) ok4 = 0;
                }
                int sh = Mem.ShmCreate("sh4t", 64);
                if (sh == 0) ok4 = 0;
                else
                {
                    int sh2 = Mem.ShmOpen("sh4t");
                    if (sh2 == 0) ok4 = 0;
                    if (Mem.ShmSize(sh) != 64) ok4 = 0;
                    if (Mem.ShmClose(sh) != 1) ok4 = 0;
                    if (sh2 != 0)
                    {
                        if (Mem.ShmSize(sh2) != 64) ok4 = 0;       // alias lives
                        if (Mem.ShmClose(sh2) != 1) ok4 = 0;
                    }
                }
                int mp = Mem.Mmap(Mem.WhatFb, 4096, 4096);
                if (mp <= 0) ok4 = 0;
                else
                {
                    if (Mem.Unmap(mp) != 1) ok4 = 0;
                    if (Mem.Mappings() != 0) ok4 = 0;
                }
                if (Mem.Live() != 0) ok4 = 0;                      // no leaks
                if (!Host.HasCap(Cap.Mem)) ok4 = 0;
                NexOS.Sys.Print("[SHELL] osapi L4 mem ok=");
                NexOS.Sys.PrintInt(ok4);
                NexOS.Sys.Print("\n");
            }

            apps = new App[MAX];   // heap_alloc zeroes -> all null
            count = 0;
            Voice.Init();           // allocate the voice registry / queue
            Desktop.Init();        // wallpaper / icon / taskbar tables
            Login.Init();          // lock screen (no-op if already signed in)
            Login.BypassForHost();  // auto-GUI boot: skip lock screen for VM testing
            Toast.Init();          // transient notification stack
            Toast.Show("NexOS", "系统就绪", 3000);
            // Keep the overlay repainting every frame so the desktop telemetry
            // HUD (clock / meters / sparkline) updates live.  The HUD also
            // re-arms SetAnim(1) from PaintOverlay as a self-healing fallback.
            if (Theme.WidgetsOn != 0) Host.SetAnim(1);
            Host.Log("NexOS.Forms.Shell initialised");
        }

        // L12 migration helper: seed a key only when the persistent store
        // has no value yet.  Legacy in-memory state is a default, not an
        // override, so a saved choice always wins on the next boot.
        static void CfgSeedInt(int h, string key, int val)
        {
            if (Cfg.Get(h, key) == null) Cfg.Set(h, key, U.I(val));
        }

        // ALL applications are independent .mex images now.  The shell does
        // NOT instantiate app classes any more: mforms_open(kind) resolves
        // the kind through sfs_files/apps.man and loads that .mex into its
        // own MiniCLR slot.  Shell.Open() remains as a managed-side shortcut
        // for the few places that used to call it directly.
        //
        // The Windows preview harness (winhost) is the one exception: it has
        // no MiniCLR slot allocator and no apps.man, so it keeps embedding
        // the application classes in-process.  Without this split,
        // Shell.Open -> Host.OpenApp -> LaunchOrFocus -> Shell.Open would
        // recurse forever in that harness.
#if WINHOST
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
            if (kind == Kind.NetTools)     return new NetToolsApp();
            if (kind == Kind.AiLauncher)   return new AiLauncherApp();
            if (kind == Kind.AiChat)       return new AiChatApp();
            if (kind == Kind.NetDefense)   return new NetDefenseApp();
            if (kind == Kind.WebView)      return new WebViewApp();
            if (kind == Kind.VideoPlayer)  return new VideoPlayerApp();
            if (kind == Kind.AudioPlayer)  return new AudioPlayerApp();
            return null;
        }
#else
        // On NexOS the kernel owns launching: it resolves the kind through
        // apps.man and loads that .mex into a fresh MiniCLR slot.
        public static int Open(int kind)
        {
            Host.OpenApp(kind);
            return -1;
        }

        // No in-shell app classes exist.  Kept as an explicit null factory so
        // a future caller cannot accidentally reintroduce embedded apps.
        static App Make(int kind)
        {
            return null;
        }
#endif

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

        // Window drag & drop from the host.  The source app gets the phases; on
        // release the host also names the window under the pointer so a drop can
        // target a DIFFERENT instance (file dragged between two Explorers).
        public static int WindowDrag(int id, int phase, int mx, int my, int hitId, int hx, int hy)
        {
            if (id < 0 || id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnDrag(phase, mx, my, hitId, hx, hy);
            return 1;
        }

        // Look an app up by window id (drop targets).
        public static App ById(int id) { return (id >= 0 && id < MAX) ? apps[id] : null; }

        // ---- running-window enumeration (taskbar buttons) ----------------
        // One entry per live window, so TWO File Explorers get two buttons.
        public static int RunningCount()
        {
            int n = 0;
            for (int i = 0; i < MAX; i++) if (apps[i] != null) n++;
            return n;
        }
        // id of the nth running app (0-based), or -1.
        public static int RunningId(int nth)
        {
            int n = 0;
            for (int i = 0; i < MAX; i++)
                if (apps[i] != null) { if (n == nth) return i; n++; }
            return -1;
        }
        public static string RunningTitle(int nth)
        {
            int id = RunningId(nth);
            return (id >= 0) ? apps[id].GetTitle() : "";
        }
        // Is this instance the one the user is interacting with?
        public static bool IsActive(int id)
        { return id >= 0 && id < MAX && apps[id] != null && apps[id] == App.Current; }
        public static int RunningKind(int nth)
        {
            int id = RunningId(nth);
            return (id >= 0) ? apps[id].KindId : -1;
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
            if (id < 0)
            {
                // P2-15: finish a Start-menu tile drag.
                return Desktop.DragEnd(mx, my) ? 1 : 0;
            }
            if (id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnMouseUp(btn, mx, my);
            return 1;
        }
        public static int MouseMove(int id, int mx, int my)
        {
            if (id < 0)
            {
                // P2-15: tile drag tracking when no window has focus.
                return Desktop.DragMove(mx, my) ? 1 : 0;
            }
            if (id >= MAX) return 0;
            App a = apps[id];
            if (a == null) return 0;
            App.Current = a;
            a.OnMouseMove(mx, my);
            return 1;
        }
        public static int Wheel(int id, int dy)
        {
            if (id < 0)
            {
                // P0-3: wheel scrolls Start menu All-apps list when no window
                // has focus but the menu is open in All-apps view.
                return Desktop.WheelAllApps(dy) ? 1 : 0;
            }
            if (id >= MAX) return 0;
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

        // Split desktop-layer entry points (see Desktop.PaintWall/PaintIcons).
        //
        // Anim.Tick() lives in PaintIcons (see below), NOT here.  PaintIcons is
        // the pass that runs on EVERY desktop repaint -- the host calls only
        // PaintIcons when it re-blits the cached wallpaper -- so ticking there
        // advances every tween exactly once per desktop frame.  Ticking in
        // PaintWall as well would double the animation rate on full repaints;
        // NOT ticking on an icons-only repaint froze every tween instead (that
        // is what left the desktop-icon highlight stuck at its accent state).
        public static void PaintWall(int w, int h)
        {
            if (Login.IsActive() != 0) { Login.Paint(w, h); return; }
            App.Current = null;
            Voice.BeginFrame();
            Desktop.PaintWall(w, h);
        }

        // Icons-only pass over an already-present wallpaper.  This is the pass
        // that runs on EVERY desktop repaint, so it owns Anim.Tick() (see above).
        // A voice frame is still opened because this pass is what registers the
        // desktop icon controls on repaints that reuse the host's wallpaper cache.
        public static void PaintIcons(int w, int h)
        {
            Anim.Tick();
            if (Login.IsActive() != 0) return;   // lock screen owns the surface
            App.Current = null;
            Voice.BeginFrame();
            Desktop.PaintIcons(w, h);
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
        // Desktop drag phases from the host: 0 = down (arm), 1 = move, 2 = up.
        // Returns 1 when the desktop consumed it (a drag actually started).
        public static int DesktopDrag(int mx, int my, int phase)
        { return Desktop.Drag(mx, my, phase); }

        public static int DesktopClick(int mx, int my)
        {
            if (Login.IsActive() != 0) { Login.Click(mx, my); return -1; }
            return Desktop.Click(mx, my);
        }

        // Keyboard control of the Start menu / desktop UI (Win key + arrows).
        //   code 0 = toggle, 1 = up, 2 = down, 3 = activate, 4 = close.
        public static int DesktopMenu(int code)
        {
            if (Login.IsActive() != 0) return -1;
            return Desktop.Menu(code);
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
            pendingSettingsArmed = 1;
            Host.OpenApp(Kind.ControlPanel);
        }
        public static int TakeSettingsPage()
        {
            int armed = pendingSettingsArmed;
            pendingSettingsArmed = 0;
            return armed != 0 ? pendingSettingsPage : -1;
        }

        // Open Notepad with a specific file already loaded.  The kernel
        // must create the native window, so we hand the request back to
        // the host instead of Shell.Open() (which would only make a C#
        // instance with nothing to paint it).
        public static void OpenNotepad(string file) { OpenNotepadFs(0, file); }

        // Open a file from a KNOWN volume.  The volume matters: Save() writes
        // back to it.  It used to always use fs 0, so a document opened from
        // the desktop (fs 3) was read from the wrong place and then "saved"
        // into the MKFS root instead of its own folder -- data looked lost.
        public static void OpenNotepadFs(int fs, string file)
        {
            Host.Log(U.Cat("[FILES] opening in Notepad: ", file));
            pendingNotepadFile = file;
            pendingNotepadFs   = fs;
            Host.OpenApp(Kind.Notepad);
        }
        public static string TakeNotepadFile()
        { string f = pendingNotepadFile; pendingNotepadFile = null; return f; }
        public static int TakeNotepadFs()
        { int f = pendingNotepadFs; pendingNotepadFs = 0; return f; }

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
