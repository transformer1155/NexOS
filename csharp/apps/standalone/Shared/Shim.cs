// =====================================================================
//  Shim.cs -- compatibility surface for applications that used to be
//             compiled INSIDE shell.mex
// ---------------------------------------------------------------------
//  Every standalone .mex links NexOS.Forms (Forms.cs / Anim.cs) plus this
//  file, and NOTHING from Desktop.cs / Shell.cs / Popup.cs.  A handful of
//  legacy applications still mention Desktop.*, Shell.* and Popup.* in
//  their code.  Rather than rewrite 19 applications, this shim provides
//  the same names with shell-independent implementations backed by Host
//  icalls.  Context menus and cross-window shortcuts deliberately degrade
//  to no-ops: the desktop owns those surfaces, an application does not.
//
//  MiniCLR has NO static field initialisers.  Every table below is built
//  lazily by Ensure() on first use.
// =====================================================================
namespace NexOS.Forms
{
    // Stable numeric ids -- identical to Kind in the desktop shell.  These
    // are app ids carried by .lnk shortcuts, not class identities.
    public static class Kind
    {
        public const int ControlPanel = 0;
        public const int FileExplorer = 1;
        public const int TaskManager = 2;
        public const int Terminal = 3;
        public const int Calculator = 4;
        public const int About = 5;
        public const int MemOptimizer = 6;
        public const int Notepad = 7;
        public const int Browser = 8;
        public const int AiSetup = 9;
        public const int AiAgent = 10;
        public const int Demo = 11;
        public const int NetTools = 12;
        public const int AiLauncher = 13;
        public const int NetDefense = 14;
        public const int WebView = 15;
        public const int VideoPlayer = 16;
        public const int AudioPlayer = 17;
        public const int AiChat = 18;
        public const int BugReport = 19;
        public const int AiWorkbench = 20;
    }

    public static class Desktop
    {
        // File-action codes.  Only compared against, never emitted here.
        public const int A_F_OPEN = 200, A_F_TERM = 201, A_F_COPY = 202, A_F_DEL = 203;
        public const int A_F_RENAME = 204, A_F_PROPS = 205, A_F_MKDIR = 206;
        public const int A_F_EDIT = 207, A_F_OPENWITH = 208;
        public const int A_F_NOTEPAD = 209, A_F_NEWFILE = 214;
        public const int A_F_CUT = 215, A_F_PASTE = 216, A_F_SHARE = 217;
        public const int A_F_CLASSIC = 218;
        public const int OWNER_DESKTOP = 0, OWNER_TASKBAR = 1, OWNER_TRAY = 2;
        public const int OWNER_FILE = 3, OWNER_WIN = 4;
        public const int OWNER_DESKTOP_FILE = 5;

        static string[] names;
        static int[] cols;
        static int[] lets;
        static int n;

        static void Ensure()
        {
            if (names != null) return;
            int cnt = Host.AppCount();
            if (cnt < 0 || cnt > 32) cnt = 20;
            n = cnt;
            names = new string[32];
            cols = new int[32];
            lets = new int[32];
            for (int i = 0; i < 32; i++) { names[i] = "App"; cols[i] = 0x4A6FA5; lets[i] = 'A'; }
            for (int i = 0; i < n; i++)
            {
                int k = Host.AppKind(i);
                if (k < 0 || k >= 32) continue;
                names[k] = Host.AppName(i);
                cols[k] = Host.AppColor(i);
                lets[k] = Host.AppLetter(i);
            }
            // The manifest is authoritative, but keep sane defaults when the
            // kernel has not published it yet (e.g. 32-bit WinHost harness).
            if (names[0] == "App")
            {
                names[0] = "Settings"; names[1] = "This PC"; names[2] = "Task Mgr";
                names[3] = "Terminal"; names[4] = "Calculator"; names[5] = "About";
                names[6] = "Optimizer"; names[7] = "Notepad"; names[8] = "Browser";
                names[9] = "AI Setup"; names[10] = "AI Agent"; names[11] = "Demo";
                names[12] = "Net Tools"; names[13] = "AI 命令台"; names[14] = "NetDefense";
                names[15] = "WebView"; names[16] = "Video Player"; names[17] = "Audio Player";
                names[18] = "AI 对话"; names[19] = "Bug 上报"; names[20] = "AI 工作台";
            }
        }

        public static int KindCount { get { Ensure(); return n; } }

        public static string KindName(int k)
        {
            Ensure();
            if (k >= 0 && k < 32) return names[k];
            return "Unknown";
        }

        public static uint AppColor(int k)
        {
            Ensure();
            if (k >= 0 && k < 32) return (uint)cols[k];
            return 0x4A6FA5u;
        }

        public static int AppLetter(int k)
        {
            Ensure();
            if (k >= 0 && k < 32) return lets[k];
            return 'A';
        }

        public static string AppVersion(int k) { return "1.0"; }
        public static string AppDesc(int k) { Ensure(); return U.Cat("独立应用：", KindName(k)); }
        public static int IsInstalled(int k) { return 1; }
        public static void SetInstalled(int k, int v) { }

        // The desktop owns context menus.  In a standalone process these are
        // logged rather than silently swallowed, so a missing menu is at
        // least diagnosable from the serial console.
        public static void OpenFileMenu(int id, int fs, int sel, int sx, int sy)
        {
            Host.Log("[standalone] file context menu is owned by the desktop");
        }

        public static void OpenFolderMenu(int id, int fs, int sx, int sy)
        {
            Host.Log("[standalone] folder context menu is owned by the desktop");
        }
    }

    public static class Shell
    {
        // Applications live in their own CLR slot now: the old in-process
        // window table does not exist.  ById() can therefore only return null.
        public static App ById(int id) { return null; }

        public static int Open(int kind) { Host.OpenApp(kind); return 0; }
        public static void Close(int id) { Host.WinAction(id, WAct.Close); }
        public static void CloseKind(int kind) { }

        public static int TakeSettingsPage() { return 0; }
        public static void OpenNotepadFs(int fs, string file) { Host.OpenApp(Kind.Notepad); }
        public static string TakeNotepadFile() { return ""; }
        public static int TakeNotepadFs() { return 0; }
    }

    public static class Popup
    {
        public static void Open(int owner, int px, int py, string[] labs, int[] codes, int count)
        {
            Host.Log("[standalone] popup owned by the desktop");
        }
        public static void Close() { }
    }
}
