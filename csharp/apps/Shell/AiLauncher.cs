// =====================================================================
//  AiLauncher.cs  -  AI command console / natural-language launcher
// ---------------------------------------------------------------------
//  Type anything and press Run (or Enter):
//    * an app name        -> opens that app (Shell.Open)
//    * a shell verb       -> runs it in the kernel (Host.Exec)
//    * a settings phrase  -> toggles a personalisation live
//    * anything else      -> falls through to the local multi-agent
//                            pipeline (Host.Exec("agent run <text>"))
//  MiniCLR rules (see Forms.cs): no static initialisers, no floats, no
//  generics / interfaces / try-catch; strings only offer Length, [i]
//  and Concat (U.Cat).  Durable state lives in fields set by the ctor.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class AiLauncherApp : App
    {
        TBox   t;        // command / request editor
        string log;      // last result or explanation
        int    editMode; // 0 idle, 1 typing the command

        // Accent palette index for the "切换强调色" phrase (static field,
        // zero-initialised by the heap allocator; no static initialiser).
        static int accentIdx = 0;

        public AiLauncherApp()
        {
            t = new TBox();
            editMode = 1;
            log = "Type an app name, a command, or a request, then press Run (or Enter).";
        }

        public override string GetTitle() { return "AI 命令台"; }

        // ---- layout helpers (Paint / Click kept in sync) ----------------
        int  Pad()  { return 16; }
        int  BoxY()  { return 78; }
        int  BoxH()  { return 36; }
        int  BoxW()  { return Gfx.Width() - 2 * Pad() - 118 - 8; }
        int  BoxX()  { return Pad(); }
        int  RunW()  { return 118; }
        int  RunX()  { return BoxX() + BoxW() + 8; }
        int  CardY() { return BoxY() + BoxH() + 16; }

        // Cap an exec transcript so we never exhaust the 512 KB bump heap.
        static string SafeCap(string res)
        {
            if (res == null) return "";
            if (res.Length <= 480) return res;
            int cut = 480;
            while (cut > 0 && ((int)res[cut] & 0xC0) == 0x80) cut--;  // UTF-8 safe
            return U.Cat(U.Sub(res, 0, cut), "...");
        }

        // Word-wrap a transcript into the output card.
        //
        // Uses U.Sub (one allocation per line) instead of the obvious
        // `buf = U.Cat(buf, CharStr(s[i]))` accumulator.  That accumulator is
        // O(n^2) BYTES on the CLR bump heap: an 80-column line costs ~3.2 KB
        // of garbage for 80 bytes of text, and the managed shell faults with
        // "managed heap exhausted" once a frame cannot fit.  Slicing also
        // keeps every multi-byte glyph intact because we only ever cut on
        // character boundaries.
        static void Wrap(int x, int y, int maxw, int maxLines, string s, uint c)
        {
            if (s == null) return;
            int cols = maxw / 8 - 2; if (cols < 8) cols = 8;
            int i = 0, line = 0;
            while (i < s.Length && line < maxLines)
            {
                int ch = (int)s[i];
                if (ch == '\n' || ch == '\r') { i++; continue; }   // hard break: blank line
                int start = i, n = 0;
                while (i < s.Length && n < cols)
                {
                    int cc = (int)s[i];
                    if (cc == '\n' || cc == '\r') break;
                    i++; n++;
                }
                Gfx.Text(x, y + line * 18, U.Sub(s, start, i - start), c);
                line++;
            }
        }

        // ---- tiny string helpers (MiniCLR: only Length / [i] / Concat) --
        static string Lower(string s)
        {
            string r = "";
            for (int i = 0; i < s.Length; i++)
            {
                int c = (int)s[i];
                if (c >= 0x41 && c <= 0x5A) c = c + 0x20;   // A-Z -> a-z
                r = U.Cat(r, Host.CharStr(c));
            }
            return r;
        }
        static int Has(string hay, string needle)
        {
            int n = needle.Length, m = hay.Length;
            if (n == 0 || n > m) return 0;
            for (int i = 0; i + n <= m; i++)
            {
                int ok = 1;
                for (int j = 0; j < n; j++) if ((int)hay[i + j] != (int)needle[j]) { ok = 0; break; }
                if (ok != 0) return 1;
            }
            return 0;
        }
        static int StartsWith(string s, string p)
        {
            if (p.Length > s.Length) return 0;
            for (int i = 0; i < p.Length; i++) if ((int)s[i] != (int)p[i]) return 0;
            return 1;
        }
        static int StrEq(string a, string b)
        {
            if (a.Length != b.Length) return 0;
            for (int i = 0; i < a.Length; i++) if ((int)a[i] != (int)b[i]) return 0;
            return 1;
        }
        static string Trim(string s)
        {
            int a = 0, b = s.Length;
            while (a < b) { int c = (int)s[a]; if (c == ' ' || c == '\t' || c == '\n' || c == '\r') a++; else break; }
            while (b > a) { int c = (int)s[b - 1]; if (c == ' ' || c == '\t' || c == '\n' || c == '\r') b--; else break; }
            return U.Sub(s, a, b - a);
        }

        // Map a free-text query to an app Kind, or -1.
        static int MatchApp(string q)
        {
            string l = Lower(q);
            if (Has(l, "calculator") != 0 || Has(l, "calc") != 0 || Has(l, "计算") != 0) return Kind.Calculator;
            if (Has(l, "notepad") != 0    || Has(l, "记事本") != 0) return Kind.Notepad;
            if (Has(l, "terminal") != 0   || Has(l, "终端") != 0) return Kind.Terminal;
            if (Has(l, "browser") != 0    || Has(l, "浏览器") != 0) return Kind.Browser;
            if (Has(l, "task") != 0       || Has(l, "任务") != 0 || Has(l, "进程") != 0) return Kind.TaskManager;
            if (Has(l, "file") != 0       || Has(l, "explorer") != 0 || Has(l, "文件") != 0) return Kind.FileExplorer;
            if (Has(l, "setting") != 0    || Has(l, "control") != 0 || Has(l, "设置") != 0 || Has(l, "控制面板") != 0) return Kind.ControlPanel;
            if (Has(l, "about") != 0      || Has(l, "关于") != 0) return Kind.About;
            if (Has(l, "net") != 0        || Has(l, "网络") != 0 || Has(l, "诊断") != 0) return Kind.NetTools;
            if (Has(l, "agent") != 0      || Has(l, "ai") != 0 || Has(l, "助手") != 0) return Kind.AiAgent;
            if (Has(l, "memory") != 0     || Has(l, "内存") != 0 || Has(l, "optim") != 0 || Has(l, "优化") != 0) return Kind.MemOptimizer;
            if (Has(l, "demo") != 0) return Kind.Demo;
            return -1;
        }

        // True when `q` looks like a kernel command (run verbatim).
        static int IsCommand(string q)
        {
            string l = Lower(q);
            if (StrEq(l, "help") != 0 || StrEq(l, "clear") != 0 || StrEq(l, "ver") != 0 ||
                StrEq(l, "whoami") != 0 || StrEq(l, "mem") != 0 || StrEq(l, "time") != 0 ||
                StrEq(l, "date") != 0 || StrEq(l, "ls") != 0 || StrEq(l, "meminfo") != 0) return 1;
            if (Has(l, " ") != 0)
            {
                if (StartsWith(l, "ping") != 0 || StartsWith(l, "net") != 0 || StartsWith(l, "cat") != 0 ||
                    StartsWith(l, "echo") != 0 || StartsWith(l, "setip") != 0 || StartsWith(l, "agent") != 0 ||
                    StartsWith(l, "model") != 0 || StartsWith(l, "ip") != 0 || StartsWith(l, "ifconfig") != 0 ||
                    StartsWith(l, "arp") != 0 || StartsWith(l, "route") != 0 || StartsWith(l, "download") != 0 ||
                    StartsWith(l, "dl") != 0 || StartsWith(l, "cd") != 0 || StartsWith(l, "sh") != 0) return 1;
            }
            return 0;
        }

        // Detect a personalisation phrase, returning a code or 0.
        static int IsSetting(string q)
        {
            string l = Lower(q);
            if (Has(l, "dark") != 0 || Has(l, "暗") != 0 || Has(l, "夜间") != 0 || Has(l, "黑") != 0) return 1; // dark
            if (Has(l, "light") != 0 || Has(l, "亮") != 0 || Has(l, "白天") != 0) return 2;                    // light
            if (Has(l, "widget") != 0 || Has(l, "小工具") != 0 || Has(l, "挂件") != 0 || Has(l, "遥测") != 0) return 3; // toggle HUD
            if (Has(l, "accent") != 0 || Has(l, "强调") != 0 || Has(l, "主题色") != 0) return 4;               // cycle accent
            return 0;
        }

        static string AppName(int kind)
        {
            if (kind == Kind.Calculator)   return "Calculator";
            if (kind == Kind.Notepad)      return "Notepad";
            if (kind == Kind.Terminal)     return "Terminal";
            if (kind == Kind.Browser)      return "Browser";
            if (kind == Kind.TaskManager)  return "Task Manager";
            if (kind == Kind.FileExplorer) return "File Explorer";
            if (kind == Kind.ControlPanel) return "Settings";
            if (kind == Kind.About)        return "About";
            if (kind == Kind.NetTools)     return "Net Tools";
            if (kind == Kind.AiAgent)      return "AI Agent";
            if (kind == Kind.MemOptimizer) return "Memory Optimizer";
            if (kind == Kind.Demo)         return "Demo";
            return "App";
        }

        static uint AccentAt(int i)
        {
            if (i == 0) return 0x0078D4u;   // Fluent blue
            if (i == 1) return 0x1FB85Au;   // green
            if (i == 2) return 0x9B6BFFu;   // violet
            if (i == 3) return 0xE5534Bu;   // red
            return 0xF2A33Cu;               // amber
        }
        static string AccentName(int i)
        {
            if (i == 0) return "蓝";
            if (i == 1) return "绿";
            if (i == 2) return "紫";
            if (i == 3) return "红";
            return "橙";
        }

        // Apply a settings code and return a human-readable confirmation.
        string ApplySetting(int which)
        {
            if (which == 1) { Theme.Dark = 1; Theme.ApplyPixel(); Theme.Save(); return "已切换到暗色主题。"; }
            if (which == 2) { Theme.Dark = 0; Theme.ApplyPixel(); Theme.Save(); return "已切换到亮色主题。"; }
            if (which == 3)
            {
                Theme.WidgetsOn = Theme.WidgetsOn != 0 ? 0 : 1;
                Theme.Save();
                if (Theme.WidgetsOn != 0) Host.SetAnim(1);
                return Theme.WidgetsOn != 0 ? "桌面遥测挂件：开。" : "桌面遥测挂件：关。";
            }
            // cycle accent through a small palette
            accentIdx = accentIdx + 1; if (accentIdx >= 5) accentIdx = 0;
            Theme.Accent = AccentAt(accentIdx);
            Theme.Save();
            return U.Cat("强调色已切换为：", AccentName(accentIdx), "。");
        }

        // Interpret the command box and act on it.
        void DoRun()
        {
            editMode = 0;
            string raw = Trim(t.text);
            if (raw.Length == 0) { log = "请输入指令或需求。"; return; }

            int app = MatchApp(raw);
            if (app >= 0) { Shell.Open(app); log = U.Cat("已打开 ", AppName(app), "。"); return; }

            if (IsCommand(raw) != 0) { log = SafeCap(Host.Exec(raw)); return; }

            int set = IsSetting(raw);
            if (set != 0) { log = ApplySetting(set); return; }

            log = SafeCap(Host.Exec(U.Cat("agent run ", raw)));
        }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height(), pad = Pad();

            W.Header(pad, pad, "AI 命令台");
            Gfx.Text(pad, pad + 26,
                     "自然语言启动器：应用名开应用，命令直跑内核，设置直接切换，其余交给 agent run。",
                     C.TextSub);

            // Command box.
            Gfx.FillRound(BoxX(), BoxY(), BoxW(), BoxH(), 8, 0xFFFFFFFFu);
            Gfx.DrawRound(BoxX(), BoxY(), BoxW(), BoxH(), 8, 0x9B6BFFu);
            string shown = t.text;
            if (shown.Length == 0) shown = "例如：打开浏览器 / net info / 切到暗色 / 帮我写份报告";
            Gfx.Text(BoxX() + 10, BoxY() + 10, shown, editMode == 1 ? C.Text : C.TextSub);
            if (editMode == 1 && (Host.Ticks() / 30) % 2 == 0)
            {
                string before = "";
                for (int i = 0; i < t.cursor; i++) before = U.Cat(before, Host.CharStr((int)t.text[i]));
                int cx = BoxX() + 10 + Gfx.Measure(before);
                Gfx.FillRect(cx, BoxY() + 10, 2, 18, C.Text);
            }
            W.Primary(RunX(), BoxY(), RunW(), BoxH(), "运行");
            W.Voice("运行 run", RunX(), BoxY(), RunW(), BoxH());

            // Output card.
            int cy = CardY();
            int chh = h - cy - pad;
            if (chh > 8)
            {
                W.Card(pad, cy, w - 2 * pad, chh);
                int lines = (chh - 24) / 18; if (lines < 1) lines = 1;
                Wrap(pad + 12, cy + 12, w - 2 * pad - 24, lines, log, C.Text);
            }
        }

        public override void OnClick(int mx, int my)
        {
            if (U.In(mx, my, BoxX(), BoxY(), BoxW(), BoxH())) { editMode = 1; return; }
            if (U.In(mx, my, RunX(), BoxY(), RunW(), BoxH())) { if (t.text.Length > 0) DoRun(); else editMode = 1; return; }
        }

        public override void OnKey(int ch)
        {
            if (editMode == 1)
            {
                if (ch == 27) { editMode = 0; return; }                                      // Esc
                if (ch == 10 || ch == 13) { if (t.text.Length > 0) DoRun(); else editMode = 0; return; } // Enter
                t.Key(ch);
            }
        }
    }
}
