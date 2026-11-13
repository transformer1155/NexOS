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
        // Frosted-glass rounded panel (Win11 Mica/Acrylic): blurs the
        // backdrop behind it.  tint = base colour, alpha = 0..255 opacity,
        // blur = blur radius in px.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Glass(int x, int y, int w, int h, int r, uint tint, int alpha, int blur);
        // UTF-8 text at an explicit glyph height (default Gfx.Text is 16px).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void TextPx(int x, int y, string s, uint fg, int px);
        // Pixel width of a UTF-8 string at glyph height px (for centring).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  MeasurePx(string s, int px);
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
        // System load 0..100 (TSC idle-accounting from the GUI loop).  0 == idle.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int CpuLoad();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Ticks();
        // Monotonic milliseconds (host-calibrated); used for double-click
        // detection.  Wraps after ~49 days.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int TickMs();
        // Request the host to keep repainting so managed animations
        // (AI desktop thinking dots / typewriter reveal) can progress.
        // The GUI loop throttles render_all() to ~30 fps while set.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void SetAnim(int on);
        // Drop the host's cached wallpaper/desktop-layer copy.  The host keeps a
        // full-screen snapshot of the managed desktop and re-blits it instead of
        // re-running the (expensive) managed paint; anything that changes the
        // wallpaper CONTENT must invalidate it or the host keeps showing the old
        // frame.  The desktop switch (classic <-> AI desktop) and the animated AI
        // desktop (which redraws every frame) are the two callers.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void WallInvalid();
        // Current content scroll offset applied to this paint (0 when the window
        // is not scrolled).  The host shifts every primitive by (-panX,-panY);
        // viewport-fixed chrome must add it back to stay pinned.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int PanX();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int PanY();
        // Number of user wallpapers packed from assets/wallpapers/ (see
        // tools/tex_pack.py).  Wallpaper index 0 is the built-in one, then
        // 1..count are the folder's images in sorted file-name order.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int WallpaperCount();
        // Push the already-painted backbuffer to the screen right now.  Used by
        // the "semi-blocking" pattern: draw a busy placeholder, Repaint() it,
        // then run the long blocking call -- the placeholder stays visible for
        // the whole stall instead of the window looking frozen.
        // Present-only by design: it must be callable from inside a paint.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void Repaint();
        // Bit i is set when a window of Kind i is open; drives the
        // running-app indicators under the taskbar buttons.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int RunningMask();
        // BUG-7: pinyin IME language mode -- 1 = Chinese, 0 = English.
        // The kernel owns the IME, so the taskbar reads it here to draw the
        // CN / EN indicator (previously the indicator was native-only and
        // gated off the managed desktop, so it never showed).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ImeMode();
        // IME candidate / pre-edit popup anchor.  A focused text control
        // publishes its caret (or input-box) origin in CLIENT coordinates
        // while painting; the native compositor converts it to screen space
        // and places the candidate bar directly above that field instead of
        // pinning it to the top of the screen.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void SetImeAnchor(int x, int y);
        // BUG-3: device pixel ratio for the font rasteriser.  1 = 1:1, 2 = each
        // glyph is rendered at 2x and box-filtered down (the fix for the blurry
        // pre-rendered font).  The kernel mirrors this into g_dpr and calls
        // vec_set_dpr() + clears the glyph cache, so it is safe to call at any
        // time; changing it re-rasterises every glyph on the next frame.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void SetDpi(int dpr);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileCount(int fs);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string FileName(int fs, int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileIsDir(int fs, int idx);
        // Folder navigation: idx < 0 = volume root, else the listing entry to
        // enter (must be a directory).  DirUp returns 1 at the volume root.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int EnterDir(int fs, int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int DirUp(int fs);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string CwdPath(int fs);
        // Re-assert a window's directory (each Explorer instance keeps its own).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int EnterPath(int fs, string path);
        // Chunked copy support: size / type by name, and a byte-range copy so a
        // large file streams in pieces (and a progress bar can be painted).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileSize(int fs, string name);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int IsDirByName(int fs, string name);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int CopyRange(int srcFs, string srcPath, string name, int dstFs, string dstPath, int off, int len);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FileRefresh();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string ReadText(int fs, string name);
        // Raw bytes of a file as a lower-case hex string (2 chars/byte), so a
        // viewer can tell text from binary (ReadText scrubs control bytes).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string ReadHex(int fs, string name);
        // Persist a UTF-8 text body to stable storage.  Returns bytes written
        // (>=0) or -1 on error.  Used by the shell to save personalization
        // settings ("nexos.cfg") and Notepad documents.  fs selects the
        // volume (0=mkfs).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int WriteText(int fs, string name, string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string Exec(string cmd);

        // ---- asynchronous local-model inference ------------------------
        // AiStart arms a background, frame-driven decode job and returns at
        // once; AiPoll returns the text produced since the last poll;
        // AiStatus: 0 idle, 1 running, 2 done, 3 error; AiCancel aborts.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AiStart(string prompt);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string AiPoll();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AiStatus();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void AiCancel();
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

        // ---- OS API layer 0: error channel / ABI / handles --------------
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void SetLastError(int code);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  LastError();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void ClearError();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  ApiVersion();
        // Bit n set = capability n present.  Gate every optional icall on it.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  CapsMask();
        public static bool HasCap(int cap) { return (CapsMask() & (1 << cap)) != 0; }
        // Handles: kind/slot/rights in, packed h_t out (0 = failure).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  HandleAlloc(int kind, int slot, int rights);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int  HandleClose(int h);
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
        // Multi-instance: always opens another window of that app instead of
        // focusing the existing one (two File Explorers side by side).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void OpenAppNew(int kind);
        // Focus the window that hosts this managed instance (one taskbar button
        // per live window, so multi-instance apps stay reachable).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void ActivateManaged(int mid);
        // Show/hide the drag ghost (the dragged file follows the pointer).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void DragGhost(int on, string name);
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
        // P2 text-gamma selector for the 64-bit glyph rasteriser.
        //   sel: 0 = off (historical look), 1 = 1.15, 2 = 1.30, 3 = 1.45.
        //   Any other value falls back to 2; the value actually stored is
        //   returned so the caller can show what took effect.
        // 64-BIT ONLY: the kernel implementation and its registration both live
        // inside `#if defined(__x86_64__)` in mforms.cpp, so this icall is NOT
        // bound in the 32-bit image.  Calling it there would fault, so every
        // call site must be guarded by `if (Host.Is64Bit() != 0)`.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int    SetTextGamma(int sel);
        // Toggle "whole window fully transparent":  on != 0 drops every window's
        // glassy chrome to zero opacity so the desktop shows straight through it.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void   SetWindowTransparent(int on);

        // ---- network downloads ---------------------------------------
        // SmartDownloadRouter: files under 5MB take a single-socket fast
        // path; larger files are sharded with HTTP Range plus an IDM-style
        // resume state machine (per-slice Offset persisted to <dest>.resume).
        // Returns bytes written (>=0) or a negative error code.  Blocking:
        // callers should Repaint() a busy placeholder first.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int    DownloadFile(string url, string dest, int timeoutMs);
        // MultiPathManager: detect and bind every usable WiFi / NIC socket,
        // give each a disjoint byte range of the file, fetch concurrently and
        // stitch -- bypassing TCP head-of-line blocking to aggregate the
        // physical bandwidth of all links.  Returns 0 when the background
        // transfer started; poll DownloadStatus(dest) for progress.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int    DownloadAsync(string url, string dest);
        // JSON progress snapshot: {"code","state","total","received",
        // "percent","slices","done","bps","error"}.  Falls back to the
        // persisted resume manifest so a restarted download still reports.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string DownloadStatus(string dest);
        // Abort a running DownloadAsync; the resume manifest is kept so a
        // later DownloadFile continues from the recorded slice offsets.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int    DownloadCancel(string dest);

        // ---- Linux guest / VNC framebuffer (WebView control backbone) ---------
        // MiniCLR returns scalars only (no out/ref/struct), so we expose four
        // separate readers for the framebuffer metadata.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxStart(string url);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void LinuxStop();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxActive();
        // LinuxFb: returns raw uint32* framebuffer ptr (cast to int, 32-bit VM).
        // Returns 0 when no framebuffer is ready yet.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxFb();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxFbW();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxFbH();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxFbCap();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxInput(int mx, int my, int buttons, int key);
        // LinuxBlit: host-side row-by-row blit from LinuxGuestFB into managed window.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int LinuxBlit(int dstX, int dstY, int dstW, int dstH, int srcX, int srcY);

        // ---- audio output -------------------------------------------------
        // Only the 64-bit kernel has a sound driver (the 32-bit one's load
        // window is full), so AudioAvailable() is 0 there and every caller
        // MUST check it before offering playback controls.
        //
        //   AudioAvailable()   1 when an HD Audio codec is up.
        //   AudioInfo()        one-line driver status for the Control Panel.
        //   AudioPlay(pcm, n)  pcm is a string holding 16-bit little-endian
        //                      stereo samples; n is the frame count offered.
        //                      Returns bytes consumed, or -1 when unavailable.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AudioAvailable();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string AudioInfo();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AudioPlay(string pcm, int nframes);

        // ---- standalone application registry (sfs_files/apps.man) --------
        // The 64-bit kernel parses the manifest at boot; the shell has NO
        // compiled-in app list.  These icalls are the only way managed code
        // learns which applications exist.  32-bit builds return 0 / "".
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AppCount();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AppKind(int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string AppName(int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string AppMex(int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AppColor(int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AppLetter(int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string AppDesc(int idx);
        // Reverse lookup used by .lnk shortcuts whose body is a .mex name.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int AppKindByMex(string mex);

        // ---- APMS (Application Problem / Performance Management) ----------
        // Kernel-side operations service: event ring, serial-log tail,
        // framebuffer screenshot and the Markdown bug report on the data
        // disk.  ApmsCapture(scale) writes apms_shot.ppm and returns its
        // byte count.  ApmsReport(text) combines the user's description
        // with telemetry/events/log and writes apms_report.md.
        // ApmsStatus() returns the number of recorded events.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ApmsStatus();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string ApmsEvent(int idx);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string ApmsLogTail();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ApmsCapture(int scale);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ApmsReport(string text);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ApmsClear();
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
        // Newline as a named constant: char literals with an escape are
        // mangled by the MiniCLR compiler, and a silent zero would make
        // every line-lookup return 0.
        const char NL = (char)10;

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
        // Redo restores the state captured by the most recent Undo.  Full undo/
        // redo stacks are more machinery than this editor needs, and MiniCLR has
        // no generics to build them with.
        int    redoLen = -1;
        string redoText;
        int    redoCursor;
        public void Redo()
        {
            if (redoLen < 0) return;
            text = redoText;
            cursor = redoCursor;
            selA = selB = cursor;
            redoLen = -1;
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
        // A MiniCLR string is a UTF-8 BYTE array, so rebuilding the text by
        // per-byte Host.CharStr(text[i]) loops -- as Insert/Backspace/Delete
        // once did -- re-UTF-8-encoded every byte of an existing CJK char
        // (E4 -> C3 A4 ...), corrupting the whole buffer on every edit made
        // after the first Chinese character.  Rebuild with byte-based StrSub
        // slices instead: they pass the raw UTF-8 through untouched, and are
        // O(n) with one allocation rather than the O(n^2) Cat loop.
        public void Insert(string s) {
            int a = selA, b = selB; if (a > b) { int t = a; a = b; b = t; }
            PushUndo();
            string head = NexOS.Sys.StrSub(text, 0, a);
            string tail = NexOS.Sys.StrSub(text, b, text.Length - b);
            text = U.Cat(U.Cat(head, s), tail);
            cursor = a + s.Length;
            selA = selB = cursor;
        }
        public void Backspace() {
            if (selA != selB) { Insert(""); return; }
            if (cursor > 0) {
                PushUndo();
                // Remove one CODEPOINT: back over UTF-8 continuation bytes so
                // a CJK char (3 bytes) goes away whole instead of leaving a
                // broken sequence behind.
                int k = cursor;
                while (k > 0 && ((int)text[k - 1] & 0xC0) == 0x80) k--;
                if (k > 0) k--;
                string head = NexOS.Sys.StrSub(text, 0, k);
                string tail = NexOS.Sys.StrSub(text, cursor, text.Length - cursor);
                text = U.Cat(head, tail);
                cursor = k;
                selA = selB = cursor;
            }
        }
        public void Delete() {
            if (selA != selB) { Insert(""); return; }
            if (cursor < text.Length) {
                PushUndo();
                // Remove the whole codepoint after the caret (1 ASCII byte or
                // one multi-byte UTF-8 sequence).
                int k = cursor + 1;
                while (k < text.Length && ((int)text[k] & 0xC0) == 0x80) k++;
                string head = NexOS.Sys.StrSub(text, 0, cursor);
                string tail = NexOS.Sys.StrSub(text, k, text.Length - k);
                text = U.Cat(head, tail);
                selA = selB = cursor;
            }
        }
        public string GetSelection() {
            if (selA == selB) return "";
            int a = selA, b = selB; if (a > b) { int t = a; a = b; b = t; }
            // Raw UTF-8 bytes of the selection (byte-safe for CJK).
            return NexOS.Sys.StrSub(text, a, b - a);
        }
        public void SelectAll() { selA = 0; selB = text.Length; }
        public void MoveLeft()  { if (cursor > 0) cursor--; selA = selB = cursor; }
        public void MoveRight() { if (cursor < text.Length) cursor++; selA = selB = cursor; }
        public void Home() { cursor = 0; selA = selB = 0; }
        public void End()  { cursor = text.Length; selA = selB = cursor; }

        // Route a keystroke.  ch is a codepoint (>=32), a negative virtual
        // key, or -1/-8 (backspace).  Returns true if consumed.
        // ---- line / column helpers ---------------------------------------
        // A caret is an offset into the text, so "current line" and "column"
        // are a single scan.  These live on TBox rather than in one app so every
        // editor shares one definition of where the caret is.
        public int LineOf(int pos)
        {
            if (pos < 0) pos = 0;
            if (pos > text.Length) pos = text.Length;
            int line = 0;
            for (int i = 0; i < pos; i++) if (text[i] == NL) line++;
            return line;
        }
        public int ColOf(int pos)
        {
            if (pos < 0) pos = 0;
            if (pos > text.Length) pos = text.Length;
            int col = 0;
            for (int i = 0; i < pos; i++) { if (text[i] == NL) col = 0; else col++; }
            return col;
        }
        public int LineCount()
        {
            int n = 1;
            for (int i = 0; i < text.Length; i++) if (text[i] == NL) n++;
            return n;
        }
        public int LineStart(int want)
        {
            int line = 0, i = 0;
            while (i < text.Length)
            {
                if (line == want) return i;
                if (text[i] == NL) line++;
                i++;
            }
            return text.Length;
        }
        // Move the caret to the SAME COLUMN on another line, clamped to that
        // line's length.  Snapping to the line start would make Up/Down feel
        // broken on any line shorter than the one the caret left.
        public void MoveVertical(int delta)
        {
            int line = LineOf(cursor);
            int col  = ColOf(cursor);
            int want = line + delta;
            if (want < 0) want = 0;
            int last = LineCount() - 1;
            if (want > last) want = last;
            int start = LineStart(want);
            int len = 0;
            while (start + len < text.Length && text[start + len] != NL) len++;
            int target = start + (col < len ? col : len);
            if (target > text.Length) target = text.Length;
            cursor = target;
            selA = selB = cursor;
        }
        public void MoveHome() { cursor = LineStart(LineOf(cursor)); selA = selB = cursor; }
        public void MoveEnd()
        {
            int start = LineStart(LineOf(cursor));
            int p = start;
            while (p < text.Length && text[p] != NL) p++;
            cursor = p; selA = selB = cursor;
        }
        // Shift+arrow: keep the anchor, move the caret, selection follows.
        public void ExtendVertical(int delta)
        {
            int anchor = selA;
            MoveVertical(delta);
            selA = anchor; selB = cursor;
            if (selA > selB) { int t = selA; selA = selB; selB = t; }
        }

        // Character constants instead of '\n' literals: MiniCLR's char handling
        // is fragile and an escape inside a char literal was silently dropped.
        public bool Key(int ch) {
            if (ch == -1 || ch == 8)  { Backspace(); return true; }                              // backspace
            if (ch == -3) { string s = GetSelection(); if (s == "") s = text; Host.SetClipboard(s); return true; } // Ctrl+C
            if (ch == -4) { Insert(Host.GetClipboard()); return true; }                          // Ctrl+V
            if (ch == -5) { Undo(); return true; }                                                // Ctrl+Z
            if (ch == -6) { SelectAll(); return true; }                                           // Ctrl+A
            // Caret movement.  These codes come from Program.cs's key mapping
            // (the kernel forwards -1..-17 for the non-character keys).
            if (ch == -7)  { MoveLeft();      return true; }                                      // Left
            if (ch == -8)  { MoveRight();     return true; }                                      // Right
            if (ch == -11) { MoveVertical(-1);  return true; }                                    // Up
            if (ch == -12) { MoveVertical(1);   return true; }                                    // Down
            if (ch == -13) { MoveHome();      return true; }                                      // Home
            if (ch == -14) { MoveEnd();       return true; }                                      // End
            if (ch == -15) { MoveVertical(-10); return true; }                                    // PageUp
            if (ch == -16) { MoveVertical(10);  return true; }                                    // PageDown
            if (ch == -17) { Delete();        return true; }                                      // Delete
            // Ctrl+X (cut).  MiniCLR has neither Substring nor Remove, so the
            // removal is rebuilt here: prefix via StrSub + suffix via StrSub.
            if (ch == -18) {
                int a = selA, b = selB;
                if (a > b) { int t = a; a = b; b = t; }
                string cut = NexOS.Sys.StrSub(text, a, b - a);
                Host.SetClipboard(cut);
                string head2 = NexOS.Sys.StrSub(text, 0, a);
                string tail2 = NexOS.Sys.StrSub(text, b, text.Length - b);
                text = U.Cat(head2, tail2);
                cursor = a; selA = selB = a;
                return true;
            }
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
        // Ink for text drawn on a WHITE panel (input boxes, dialogs).  The
        // desktop's own Ink is near-white, so reusing it on a white input box
        // produced invisible text -- always use this on light surfaces.
        public const uint OnLight   = 0xFF1B1B1B;
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
        // Which wallpaper to draw: 0 = original bloom texture, 1 = mesh
        // gradient, 2 = dusk ridge.  Cycled from the desktop right-click menu
        // and persisted in nexos.cfg.
        public static int   WallIdx   = 0;
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
        // P2: glyph anti-aliasing gamma for the 64-bit rasteriser.
        //   0 = off, 1 = 1.15, 2 = 1.30 (default), 3 = 1.45.
        // Only meaningful where Host.SetTextGamma exists (64-bit); the 32-bit
        // image has no such icall and simply ignores this value.
        // NOTE: the "= GammaDefault" below is DOCUMENTATION ONLY under MiniCLR --
        // static initialisers never run (measured: "NOT APPLIED (0/4) | pStr=null"),
        // so the value really has to be established by Theme.Load(), which primes it
        // before parsing nexos.cfg.  0 is a *legal* selector ("off"), so an unprimed
        // 0 would silently ship the feature disabled AND persist that wrong value.
        public const  int    GammaDefault = 2;
        public static int    TextGamma  = GammaDefault;

        // Clamp a gamma selector coming from the cfg file (or anywhere else).
        // Out-of-range and unparseable values (ParseInt returns 0 for junk, so a
        // corrupt cfg would otherwise look like "off") fall back to 1.30.
        // Range test written without `v + 1` so it cannot overflow.
        public static int ClampGamma(int v)
        {
            return (v >= 0 && v <= 3) ? v : GammaDefault;
        }
        // BUG-3: device pixel ratio used by the kernel font rasteriser.
        // 1 = rasterise 1:1 (fastest), 2 = supersample 2x and box-filter down
        // (sharpest small glyphs -- the default), 3/4 = even smoother at more
        // CPU cost.  Only the vector (msyh.ttf) path honours it; the bitmap
        // fallbacks ignore it.
        public static int    FontDpr    = 2;
        public static int    WidgetsOn  = 1;   // 1 = show desktop telemetry widget

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
        // const, NOT static.  The mini-CLR does not run static field
        // initialisers (measured by InitProbe: they are dropped), and a dropped
        // string literal becomes NULL -- which handed Host.WriteText/ReadText an
        // empty file name and silently broke settings persistence, because the
        // literal is the only place this name was ever assigned.  `const` is
        // inlined at compile time and is not a static field at all, so it is
        // immune to that whole class of bug.  Same pattern as Desktop.cs's
        // `const string CFG_NAME`.
        public const string CfgName = "nexos.cfg";

        // Advance to the next built-in wallpaper.  The caller invalidates the
        // host's cached desktop layer so the new image is painted immediately.
        public static void CycleWallpaper()
        {
            // 1 built-in wallpaper + however many PNGs are in assets/wallpapers/.
            int total = 1 + Host.WallpaperCount();
            if (total < 1) total = 1;
            WallIdx = (WallIdx + 1) % total;
            Save();
        }

        public static void Save()
        {
            string cfg = "";
            cfg = NexOS.Sys.StrConcat(cfg, "walltop=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr((int)WallTop));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "wallbot=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr((int)WallBot));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "wallidx=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(WallIdx));
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
            cfg = NexOS.Sys.StrConcat(cfg, "fontdpr=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(FontDpr));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "textgamma=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(TextGamma));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "widgetson=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(WidgetsOn));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "termcellh=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(TermCellH));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            cfg = NexOS.Sys.StrConcat(cfg, "termbgmode=");
            cfg = NexOS.Sys.StrConcat(cfg, NexOS.Sys.IntToStr(TermBgMode));
            cfg = NexOS.Sys.StrConcat(cfg, "\n");
            Host.WriteText(0, CfgName, cfg);
            // P2-17: force immediate repaint so Theme changes (wallpaper, accent,
            // dark/light) are visible without waiting for the next mouse event.
            Host.Repaint();
        }

        public static void Load()
        {
            // Establish the default FIRST.  Static initialisers are dropped by
            // MiniCLR (see Theme.TextGamma above), so without this line TextGamma
            // would still be 0 when ApplyPixel() pushes it, and 0 means "gamma off"
            // -- the feature would ship disabled on a fresh install and the wrong
            // value would then be persisted to nexos.cfg by the first Save().
            // Parsing the cfg below still overrides it, so a saved choice wins.
            TextGamma = GammaDefault;
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
                else if (NexOS.Sys.StrEq(key, "wallidx"))     WallIdx = (v >= 0 && v <= 2) ? v : 0;
                else if (NexOS.Sys.StrEq(key, "dark"))        Dark        = v;
                else if (NexOS.Sys.StrEq(key, "taskbarleft")) TaskbarLeft = v;
                else if (NexOS.Sys.StrEq(key, "showlabels"))  ShowLabels  = v;
                else if (NexOS.Sys.StrEq(key, "activenet"))   ActiveNet   = v;
                else if (NexOS.Sys.StrEq(key, "voiceon"))     VoiceOn     = v;
                else if (NexOS.Sys.StrEq(key, "desktopmode")) DesktopMode = v;
                else if (NexOS.Sys.StrEq(key, "pixelmode"))   PixelMode  = v;
                else if (NexOS.Sys.StrEq(key, "pixelscale"))  PixelScale = (v > 1) ? v : 1;
                else if (NexOS.Sys.StrEq(key, "pixelscan"))   PixelScan  = v;
                else if (NexOS.Sys.StrEq(key, "fontdpr"))     FontDpr = (v >= 1 && v <= 4) ? v : 2;
                else if (NexOS.Sys.StrEq(key, "textgamma"))
                {
                    // A corrupt value must NOT reach the clamp: ParseInt("abc")
                    // returns 0 and 0 is a legitimate selector ("off"), so without
                    // this shape check a damaged cfg silently disables gamma.
                    // An out-of-range number is still handled by ClampGamma.
                    TextGamma = LooksLikeInt(val) ? ClampGamma(v) : GammaDefault;
                }
                else if (NexOS.Sys.StrEq(key, "widgetson"))   WidgetsOn = v;
                else if (NexOS.Sys.StrEq(key, "termcellh"))   { TermCellH = v; if (TermCellH < 12) TermCellH = 12; if (TermCellH > 28) TermCellH = 28; }
                else if (NexOS.Sys.StrEq(key, "termbgmode"))  TermBgMode = v;
            }
        }

        // Boundary self-test for the gamma persistence path.  Called once from
        // Shell.Init() after Theme.Load(); prints exactly one line so it can be
        // grepped from the serial log ("gamma selfcheck=").  It exercises the SAME
        // ClampGamma / LooksLikeInt code the parser uses -- not a re-implementation
        // -- so a pass means the shipped logic is right, and a fail names the case.
        // Expect: 16 ok cases, bad=0.
        public static int GammaSelfCheck()
        {
            int bad = 0;
            // valid selectors must pass through untouched, including 0 ("off")
            if (ClampGamma(0) != 0) bad++;
            if (ClampGamma(1) != 1) bad++;
            if (ClampGamma(2) != 2) bad++;
            if (ClampGamma(3) != 3) bad++;
            // just outside the range on both sides, plus extremes -> default 2
            if (ClampGamma(4) != GammaDefault) bad++;
            if (ClampGamma(-1) != GammaDefault) bad++;
            if (ClampGamma(99) != GammaDefault) bad++;
            if (ClampGamma(2147483647) != GammaDefault) bad++;
            if (ClampGamma(-2147483648) != GammaDefault) bad++;
            // shape checks: what the parser refuses before it even clamps
            if (LooksLikeInt("0") != true) bad++;
            if (LooksLikeInt("3") != true) bad++;
            if (LooksLikeInt("-1") != true) bad++;
            if (LooksLikeInt("") != false) bad++;
            if (LooksLikeInt("abc") != false) bad++;
            if (LooksLikeInt("2x") != false) bad++;
            if (LooksLikeInt("-") != false) bad++;
            return bad;
        }

        // Push the current pixel-mode settings to the kernel framebuffer
        // post-process.  Call after Load() and after any Settings change.
        public static void ApplyPixel()
        {
            Host.SetPixel(PixelMode, PixelScale, PixelScan);
            // BUG-3: keep the font rasteriser and the AA flag in sync with the
            // retro mode.  Turning pixel mode ON without this is what made the
            // pre-rendered font look blurred: the post-process quantised every
            // glyph coverage ramp to 5/6/5 bits.
            Host.SetDpi(FontDpr);
            // P2 text gamma.  Guarded because the icall only exists in the
            // 64-bit build (see the declaration above); the assignment keeps
            // Theme in sync with whatever the kernel actually accepted.
            if (Host.Is64Bit() != 0) TextGamma = Host.SetTextGamma(TextGamma);
        }

        // Decimal (optionally signed) integer parse.  No BCL int.Parse in
        // MiniCLR, so this is a hand-rolled scanner over the managed string
        // API.  Stops at the first non-digit.
        // True when s is a non-empty, optionally sign-prefixed decimal integer.
        // Written with the managed string API only (MiniCLR has no ranges or
        // foreach over strings) and it never allocates.
        public static bool LooksLikeInt(string s)
        {
            int n = NexOS.Sys.StrLen(s), i = 0;
            if (n == 0) return false;
            char c0 = NexOS.Sys.StrCharAt(s, 0);
            if (c0 == '-' || c0 == '+')
            {
                if (n < 2) return false;      // a bare sign is not a number
                i = 1;
            }
            for (; i < n; i++)
            {
                char c = NexOS.Sys.StrCharAt(s, i);
                if (c < '0' || c > '9') return false;
            }
            return true;
        }

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
        // User wallpapers: Tex.WallDyn + i, i = 0..Host.WallpaperCount()-1.
        // The host loads a contiguous block starting at id 10 (TEX_WP_BASE).
        public const int WallDyn = 10;
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
            return (uint)(((uint)a << 24) | (c & 0x00FFFFFF));
        }

        // Fade a colour by `t` (0..1000) from fully transparent -> opaque,
        // preserving its original alpha at t=1000.
        public static uint Fade(uint c, int t)
        {
            if (t <= 0) return (c & 0x00FFFFFF);          // alpha 0
            if (t >= 1000) return c;
            int a = (int)((c >> 24) & 0xFF);
            int na = (a * t) / 1000;
            return (uint)(((uint)na << 24) | (c & 0x00FFFFFF));
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
        // VIEWPORT-LOCKED backdrop.
        //
        // The host implements window scrolling by shifting every primitive by
        // (-panX,-panY), so a fill at (0,0) scrolls away with the content and leaves
        // the strip it vacated unpainted -- the window's translucent glass then shows
        // through ("the background disappears / turns transparent when scrolled").
        // Filling at (panX,panY) cancels the shift and always lands exactly on the
        // visible client area.
        int px = Host.PanX(), py = Host.PanY();
        if (Gfx.HasImage(Tex.WinBg) != 0) Gfx.Image(Tex.WinBg, px, py, Gfx.Width(), Gfx.Height());
        else Gfx.FillRect(px, py, Gfx.Width(), Gfx.Height(), C.WinBg);
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
#if !STANDALONE_APP
            // Voice.cs is shell-side code (it drives synthetic clicks through
            // Desktop/Shell).  A standalone application still compiles against
            // the same control API, it just registers no voice binding: the
            // menu/button renders and responds to the mouse exactly as before,
            // only voice activation is unavailable until the voice engine is
            // made a Host service rather than a shell module.
            NexOS.Forms.Voice.Register(cmd, x, y, w, h);
#endif
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
        // Window drag & drop (screen coords).  phase 0=press, 1=move, 2=release;
        // hitId/hx/hy identify the managed window under the pointer on release,
        // so an app can drop something into ANOTHER app window.
        public virtual void OnDrag(int phase, int mx, int my, int hitId, int hx, int hy) { }
        // Another window dropped a file on this one.  Apps that accept drops
        // (File Explorer) override it; the default refuses.
        // (Virtual dispatch instead of an 'as' cast: the mini CLR does not
        // reliably implement isinst.)
        public virtual int AcceptDrop(int srcFs, string srcPath, string name) { return -1; }
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
#if !STANDALONE_APP
            // The desktop owns the shared window menu.  A standalone application
            // has no Desktop class (that is shell code), so it simply does not
            // offer this menu; the button is not a dead zone either way because
            // the app's own handler still ran.
            if (HitControl(mx, my) >= 0) Desktop.OpenWinMenu(id, ox + mx, oy + my);
#endif
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
            // WAct.Close goes through the Host, not through Shell.Close().
            //
            // Shell.Close() lives in Shell.cs, and Forms.cs must be compilable
            // WITHOUT the shell so a application can be built as its own
            // assembly (NexOS.Forms + corelib only).  Host.WinAction already
            // carries the same request to the kernel, and the comment below
            // states that unsupported actions are no-ops, so this is the same
            // behaviour with one less link-time dependency.
            else if (code == WAct.Close)   Host.WinAction(id, WAct.Close);
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
    // Small pure helpers shared by more than one application.  They live in
    // the toolkit rather than in a sibling application's class so that no
    // app has to link another app's source just to reach a function.
    public static class AppUtil
    {
        // Terminal font size steps, shared by the Terminal (its own
        // Ctrl+Plus / Ctrl+Minus) and the Control Panel (terminal settings).
        public static int TermZoomStep(int cur, int dir)
        {
            int[] steps = new int[6];
            steps[0] = 14; steps[1] = 16; steps[2] = 18;
            steps[3] = 20; steps[4] = 22; steps[5] = 24;
            int idx = 2;
            for (int i = 0; i < 6; i++) if (steps[i] == cur) { idx = i; break; }
            idx += dir; if (idx < 0) idx = 0; if (idx > 5) idx = 5;
            return steps[idx];
        }
    }

    // WAct: the host gesture codes the shell understands.  The desktop
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

    // =================================================================
    //  OS API layer 0+1 (ABI v1): error codes, capabilities, handles,
    //  read-only permission facade.  All icalls are name+signature bound;
    //  changing any signature requires NEX_ABI_VERSION++.
    // =================================================================
    public static class Err
    {
        public const int OK          = 0;
        public const int NoEntry     = 1;
        public const int BadArg      = 2;
        public const int BadHandle   = 3;
        public const int PermDenied  = 4;
        public const int NameExists  = 5;
        public const int NotFound    = 6;
        public const int NoSpace     = 7;
        public const int Busy        = 8;
        public const int Broken      = 9;
        public const int Timeout     = 10;
        public const int TooBig      = 11;
        public const int NoMem       = 12;
        public const int NotSupported= 13;
        public const int IoError     = 14;
        public const int WouldBlock  = 15;
        public const int CrossDev    = 16;
        public const int NotDir      = 17;
        public const int IsDir       = 18;
        public const int Quota       = 19;
        public const int Deadlk      = 20;
        public const int Stale       = 21;
        public const int ClrFault    = 22;
    }

    public static class Cap
    {
        public const int Err = 0, Handle = 1, Perm = 2, Proc = 3, Thread = 4;
        public const int Sync = 5, Job = 6, Fs = 7, Mem = 8, Ipc = 9;
        public const int Net = 10, Dev = 11, Clock = 12, Gfx = 13, Pkg = 14;
        public const int Log = 15, Cfg = 16, Clr = 17, Dbg = 18;
    }

    // Read-only handle value; only ever obtained from an alloc icall.
    // kind:8 | slot:16 | gen:8  packed into a 32-bit int.
    public struct Handle
    {
        public int Raw;
        public int Kind() { return Raw & 0xFF; }
        public int Gen()  { return (Raw >> 24) & 0xFF; }
        public bool Valid() { return Raw != 0; }
        public static Handle None() { Handle h; h.Raw = 0; return h; }
    }

    // Perm: strictly read-only security facade.  There is deliberately NO
    // SetBound*/SetToken/SetCaps here -- boundaries change only from ring 0.
    public static class Perm
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Uid();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FsVolMask();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FsPrefixCount();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FsPrefixVol(int i);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FsPrefixMode(int i);
        // Active check: can the current token access `path` with `mode`
        // (FSB_RO=1 read, FSB_RW=2 write)?  Segment-boundary prefix match.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int FsCanPath(string path, int mode);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int DevAllowed(int devClass, int devId);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int NetPortAllowed(int port);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int NetAddrAllowed(int ipBe);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int SyscallAllowed(int cls);
        public static bool CanRead(string path)  { return FsCanPath(path, 1) != 0; }
        public static bool CanWrite(string path) { return FsCanPath(path, 2) != 0; }
    }

    // =================================================================
    //  OS API layer 2: sync primitives, async jobs, timers.
    //  The kernel is cooperative (no preemption), so blocking waits are
    //  bounded: Mutex/Sem/Cond fail fast (Busy/WouldBlock) instead of
    //  deadlocking; Event supports a bounded busy wait.
    // =================================================================
    public static class Sync
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int MutexNew();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Lock(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Unlock(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int SemNew(int init, int max);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int SemWait(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int SemPost(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int EventNew();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int EventWait(int h, int ms, int clear);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int EventSet(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int EventClear(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int CondNew();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int CondWait(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int CondSignal(int h, int all);
    }

    public static class Job
    {
        public const int Run = 0, Done = 1, Err = 2, Cancel = 3;
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Start(int kind, int goal);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Status(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Progress(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ProgressTotal(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int CancelJob(int h);
        // driven automatically each frame by the host paint overlay
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Tick(int budget);
    }

    public static class Timer
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int New(int ms, int periodic);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Poll(int h);
    }

    // =================================================================
    //  OS API layer 3: VFS resolve, atomic file layer, directories.
    //  Paths are canonical absolute ("/data/..."-style); ".." never
    //  escapes the root.  All icalls set LastError on failure.
    // =================================================================
    public static class Fs
    {
        // Resolve ".."/"." segments; returns canonical path or null (err set).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string Resolve(string path);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int DirOpen(string path);
        // One entry per call; null at end of listing.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string DirNext(int h);
        // Glob: newline-joined matches of `pattern` over the listing; null on error.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string Glob(string dir, string pattern);
    }

    public static class FileFlags
    {
        public const int RD = 1, WR = 2, Append = 4, Trunc = 8;
        public const int Creat = 16, Excl = 32;
    }

    public static class File
    {
        // Atomic open.  O_CREAT|O_EXCL on an existing name fails with
        // Err.NameExists.  Returns handle 0 on failure (see LastError).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Open(string path, int flags);
        // Read/Write use int[] buffers (MiniCLR has no byte[]); each element
        // carries one byte value.  Returns bytes moved, 0 at EOF.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Read(int h, int[] buf, int len);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Write(int h, int[] buf, int len);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Seek(int h, int off, int whence);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Size(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Flush(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Close(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Lock(int h, int op);  // 1=SH 2=EX 0=UN
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Rename(string a, string b);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Unlink(string path);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int MkDir(string path);
    }

    // ---- L2 processes: cooperative CLR-slot processes -------------------
    // One .mex image per process, loaded into its own CLR slot.  Wait is a
    // poll (this kernel has no preemption): -1 while running, >=0 exit code.
    public static class Proc
    {
        public const int Running = -1;
        public const int BadPid  = -2;
        public const int Failed  = -3;
        public const int Killed  = -4;

        public const int StateRunning = 0;
        public const int StateExited  = 1;

        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Spawn(string mex);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Wait(int pid);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Kill(int pid);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Exit(int code); // from inside own process
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Count();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int PidAt(int i);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int StateAt(int pid);
    }

    // =================================================================
    //  OS API layer 12: persistent configuration store.
    //  Logical name "t" maps to the canonical /data/cfg/t.cfg (boundary
    //  checked); the flat MKFS stores "t.cfg".  Open() caches the whole
    //  file, Set/Del mutate the cache, Flush() writes t.cfg.tmp and
    //  renames it to t.cfg so a torn write cannot corrupt the old copy.
    //
    //  ABI note: MiniCLR is a 32-bit stack machine (icall args are int32
    //  slots), so handles are the packed 32-bit h_t here, exactly as in
    //  File/Sync.  Wrap() gives callers the read-only Handle view.
    //  Every failing call sets Host.LastError(); 0 is the invalid handle.
    // =================================================================
    public static class Cfg
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Open(string name);
        // null => no such key / end of keys (Host.LastError == Err.NoEntry)
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string Get(int h, string key);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Set(int h, string key, string val);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Del(int h, string key);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern string KeyAt(int h, int i);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Flush(int h);
        // Close flushes first, then invalidates the handle (gen++).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Close(int h);
        // ---- read-only accessors over the cached CfgEntry[] ----
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Count(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Dirty(int h);

        public static Handle Wrap(int h) { Handle x; x.Raw = h; return x; }
    }

    // =================================================================
    //  OS API layer 4: memory.
    //  Managed code NEVER sees a pointer -- only packed int handles.
    //  Bytes are reached through HeapPoke/HeapPeek (offset + bounds check),
    //  arena allocations return a byte offset, shm segments are named,
    //  and Mmap only records a mapping (the kernel is identity-mapped).
    //
    //  Free/Close ALWAYS bumps the generation: a recycled slot can never
    //  make an old handle resolve again (Host.LastError == Err.Stale).
    //  This is a separate real heap (kernel kmalloc), NOT the CLR bump
    //  heap that Paint() uses -- the two tracks never mix.
    //  ABI: int handles for the same 32-bit-stack-machine reason as Cfg.
    // =================================================================
    public static class Mem
    {
        public const int WhatFb   = 1;
        public const int WhatMmio = 2;

        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapAlloc(int sz);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapFree(int h);
        // Returns a NEW handle; the old one is invalidated even if the
        // allocation stayed in the same slot.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapRealloc(int h, int sz);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapSize(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapPoke(int h, int off, int val);
        // 0..255 on success, -1 on a bad/stale handle (check LastError).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int HeapPeek(int h, int off);

        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ArenaNew(int cap);
        // 0 is a VALID offset; -1 means failure (check LastError).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ArenaAlloc(int h, int sz);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ArenaMark(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ArenaRelease(int h, int mark);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ArenaUsed(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ArenaFree(int h);

        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ShmCreate(string name, int sz);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ShmOpen(string name);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ShmSize(int h);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int ShmClose(int h);

        // Returns a positive mapping id, or 0 on failure (dev_class GUARD,
        // overlap, out of range).  Unmap invalidates just that id.
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Mmap(int what, int addr, int len);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Unmap(int id);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Mappings();
        // Read-only: number of live L4 blocks (leak sentinel).
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int Live();

        public static Handle Wrap(int h) { Handle x; x.Raw = h; return x; }
    }
}
