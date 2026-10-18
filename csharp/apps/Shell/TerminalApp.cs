// =====================================================================
//  TerminalApp.cs  -  IDE 级智能终端（幽灵文本 + AST 高亮 + 可视化 Diff）
// ---------------------------------------------------------------------
//  本文件由 csharp/apps/Shell/Apps.cs 中的 TerminalApp 迁出并增强。
//  原先该类已从 Apps.cs 删除，改为在独立文件中维护。
//
//  在原有 GNOME 终端兼容能力（ANSI/SGR 解析、滚动回看、选区复制、
//  Ctrl+R 反向搜索、多标签、滚轮）之上，新增三项 IDE 特性：
//    1) 幽灵文本   —— 输入停顿 350ms 请求 AI 预测，灰紫浮于光标后，Tab 采纳；
//    2) AST 高亮   —— 词法分析命令/flag/字符串/注释/运算符，括号自动配对
//                     （成对金色、未闭合或错配红色），按字符切片分段绘制；
//    3) 可视化 Diff —— 输出命中错误特征时请求 AI 修复，在终端上方弹出
//                     双栏面板（左原命令+错误，右修复），Y 接受并后台重跑。
//
//  MiniCLR 约束（务必遵守）：
//    * 无反射 / 无泛型 / 无 async / 无 try-catch / 无浮点 / 无 string.Format；
//    * 堆是每帧回卷的 bump 堆 —— OnPaint 内零分配，缓存数组仅在 ctor 创建；
//    * 字符串只有 Length / [i] / Concat —— 统一走 U.Cat / U.Sub（单次分配）；
//    * 无静态初始化器 —— 字符表一律在 ctor 里显式赋值。
//
//  AI 网关不可用时：幽灵文本与 Diff 静默降级（aiOff 闩锁，只记一次日志），
//  终端保持完整的原有输入输出能力。
// =====================================================================
#if WINHOST
using NexOS.AI;
#endif
using NexOS.Forms;

namespace NexOS.Forms
{public class TerminalApp : App
    {
        const int SCROLL_MAX = 2000, HIST_MAX = 200, TAB_MAX = 8;
        const int GRIDCAP = 65536;   // cell grid cap (MiniCLR heap-safe: 256KB/array)
        const int TAB_H = 26, PAD = 8, ROWCAP = 16000;
        const int BG = 0x300A24, FG = 0xFFFFFF, SEL = 0x4A2E5A;
        const int URLC = 0x55AAFF;   // link-blue for URL hover highlight

        // ---- IDE 特性配色（与 Web 端同一套 token 色）----
        const int TK_DEF   = 0xE8E8E8;   // 普通字符
        const int TK_CMD   = 0x4EC9B0;   // 命令 / 关键字
        const int TK_STR   = 0x6A9955;   // 字符串 / 注释
        const int TK_FLAG  = 0xDCDCAA;   // -flag
        const int TK_OP    = 0xD4D4D4;   // 运算符 / 空白
        const int TK_BM    = 0xFFD700;   // 配对成功的括号
        const int TK_BAD   = 0xF85149;   // 未闭合 / 错配括号
        const int TK_GHOST = 0x7A6A80;   // 幽灵文本（暗灰紫≈半透明）
        const int DF_BG    = 0x1E0B14;
        const int DF_HEAD  = 0xFFB86C;
        const int DF_DEL   = 0xFF7B72;
        const int DF_ADD   = 0x7EE787;
        const int DF_BORDER= 0x5A3550;
        const int GHOST_IDLE_MS = 350;
        const int GHOST_MAX     = 64;
        const int ESC = 27;

        // One session == one tab.
        public class Term
        {
            public string[] lines = new string[SCROLL_MAX];
            public int head = 0, count = 0;   // ring buffer of logical lines
            public int view = 0;               // scrollback offset from bottom
            public string input = "";
            public int caret = 0;
            public bool hasSel = false;
            public int selLineA = -1, selCharA = -1, selLineB = -1, selCharB = -1;
            public string[] hist = new string[HIST_MAX];
            public int histN = 0, histPos = 0;
            public bool search = false;
            public string sbuf = "";
            public string title = "root@nexos";

            // ---- 幽灵文本（AI 命令预测，每 tab 独立）----
            public string ghost = "";        // 预测出的后续片段
            public string ghostFor = "";     // 该预测对应的输入快照
            public int ghostAskMs = 0;       // 上次发起请求的时刻
            public int ghostReqId = 0;       // 请求序号（丢弃过期结果）
            public bool dragging = false;
        }

        // Parsed line: displayed chars (ANSI stripped) + per-char fg/bg
        // (0xRRGGBB, -1 = default).  Stored as a char[] so OnPaint can
        // render without building a string per frame (the O(n^2) Cat loop
        // warned about in U.Sub would fault the MiniCLR bump heap).
        public class PR
        {
            public char[] ch;
            public int len;
            public int[] fg;
            public int[] bg;
            public int[] url;   // per-char URL flag (0/1), for hover highlight
        }

        int[] PAL;                  // 16-colour Ubuntu GNOME palette
        int active = 0;
        Term[] tabs;
        int tabN = 1;

        // Layout caches (recomputed every frame for the active tab).
        int cols, rows, cellW, cellH, contentX, contentY, contentW, contentH;
        int[] gLine, gChar; int gN;
        int[] rowLine, rowStart, rowEnd; int rowN;
        PR[] prs; int firstAbsRow, visRows, lastTotalRows;

        int hoverX = -1, hoverY = -1;
        int hoverLine = -1, hoverChar = -1;
        int lastDownMs = 0, lastDownCell = -1, clickCount = 0;
        string user = "root", host = "nexos", cwd = "~";

        // =================================================================
        //  IDE 智能终端状态（幽灵文本 / AST 高亮 / 可视化 Diff）
        // =================================================================
        // 输入行高亮缓存：ctor 一次性分配，之后只写不 new（bump 堆约束）
        int[] inFg;          // 每个输入字符的显示色
        int[] bmOpen;        // 括号配对栈（待匹配的左括号下标）
        int   bmOpenTop;     // 栈顶
        const int IN_CAP = 512, BM_CAP = 32;

        // 词法/错误特征表：'|' 分隔的普通字符串，避免 new string[]（MiniCLR）
        string KW_TABLE, ERR_TABLE;

        // ---- 可视化 Diff（需求指定的成员名）----
        public bool isDiffMode;      // 是否展示 AI 修复建议
        string originalCmd;          // 出错的原命令
        string fixedCmd;             // AI 给出的修复命令
        string diffErrLine;          // 触发修复的错误行

        // 背景重跑：命令排队到下一帧帧首执行，避免在 OnPaint 里跑命令卡帧
        bool pendingRun;
        string pendingCmd;

        // AI 可用性闩锁：0=未知 1=可用 2=已判定不可用（静默降级，不再重试）
        int aiOff;

        // WinHost 下后台线程 → UI 线程的 AI 结果中转（OnPaint 帧首轮询）
#if WINHOST
        string aiResultText;
        int    aiResultKind;    // 0 空闲 / 1 幽灵文本 / 2 修复命令
        int    aiResultTab;     // 发起请求时的 tab
        string aiResultBase;    // 发起请求时的输入快照
        int    aiBusy;          // 0 无在途 / 1 有
#endif

        public TerminalApp()
        {
            PAL = new int[] { 0x2E3436,0xCC0000,0x4E9A06,0xC4A000,0x3465A4,0x75507B,
                              0x06989A,0xD3D7CF,0x555753,0xEF2929,0x8AE234,0xFCE94F,
                              0x729FCF,0xAD7FA8,0x34E2E2,0xEEEEEC };
            tabs = new Term[TAB_MAX];
            tabs[0] = new Term();
            tabN = 1; active = 0;
            gLine = new int[GRIDCAP]; gChar = new int[GRIDCAP];

            // ---- IDE 特性的一次性分配与字符表初始化 ----
            inFg = new int[IN_CAP];
            bmOpen = new int[BM_CAP];
            bmOpenTop = 0;
            isDiffMode = false;
            originalCmd = ""; fixedCmd = ""; diffErrLine = "";
            pendingRun = false; pendingCmd = "";
            aiOff = 0;
            // MiniCLR 无静态初始化器，字符串表必须在这里显式赋值
            KW_TABLE = U.Cat("open|theme|wall|ai|help|ver|cls|clear|ls|cat|echo|ps|",
                             "mem|date|time|whoami|uname|exit|run|agent|plugin|",
                             "pwd|history|mkdir|rm|cp|mv|touch|sudo|set|export");
            ERR_TABLE = U.Cat("error|Error|ERROR|fail|Fail|FAIL|exception|Exception|",
                              "not found|No such|denied|unknown|Unknown|",
                              "traceback|abort|Abort|invalid|Invalid");
            AppendLine(tabs[0],
                U.Cat("NexOS Terminal - GNOME Terminal compatible.\n",
                      "Type `help`.  Ctrl+Shift+T new tab, Ctrl+Shift+C copy,\n",
                      "Ctrl+Shift+V paste, middle-click paste, wheel = scrollback."));
        }

        Term T() { return tabs[active]; }

        // Test / introspection helpers (used by the WinHost --termtest
        // harness).  They expose otherwise-private per-tab state without
        // changing any runtime behaviour.
        public Term ActiveTerm() { return T(); }
        public string SelText() { return GetSelText(T()); }
        public int TabCount() { return tabN; }
        public string UserHost() { return U.Cat(user, "@", host); }
        // Test-only helpers (WinHost --termtest harness): inject a logical
        // line (e.g. ANSI-coloured) and read the most recently appended one.
        public void TestAppend(string s) { AppendLine(T(), s); }
        public string LastLine()
        {
            Term t = T(); int b = t.head - 1; if (b < 0) b += SCROLL_MAX;
            return t.lines[b];
        }

        public override string GetTitle() { return U.Cat(user, "@", host, ":", cwd, "$", ""); }

        // ---- scrollback ----------------------------------------------
        string RingAt(Term t, int idx)
        {
            int b = t.head - t.count; if (b < 0) b += SCROLL_MAX;
            return t.lines[(b + idx) % SCROLL_MAX];
        }
        void AppendLine(Term t, string s)
        {
            if (s == null) return;
            int n = s.Length, start = 0;
            for (int i = 0; i <= n; i++)
            {
                if (i == n || s[i] == '\n')
                {
                    t.lines[t.head] = U.Sub(s, start, i - start);
                    t.head = (t.head + 1) % SCROLL_MAX;
                    if (t.count < SCROLL_MAX) t.count++;
                    start = i + 1;
                }
            }
            if (t.view > 0) t.view = 0;   // stick to bottom on new output
        }
        void ClearScreen(Term t) { t.count = 0; t.head = 0; t.view = 0; }

        // ---- prompt / title ------------------------------------------
        string Pad2(int v) { string s = U.I(v); if (s.Length < 2) s = U.Cat("0", s); return s; }
        string PS1()
        {
            string ts = U.Cat("[", U.I(Host.Hour()), ":", Pad2(Host.Minute()), ":",
                              Pad2(Host.Second()), "] ");
            return U.Cat(ts, user, "@", host, ":", cwd, "$ ");
        }

        // ---- ANSI / SGR colour ---------------------------------------
        int Cube(int n)
        {
            int v = n - 16, r = v / 36, g = (v / 6) % 6, b = v % 6;
            return (CubeC(r) << 16) | (CubeC(g) << 8) | CubeC(b);
        }
        int CubeC(int x) { return x == 0 ? 0 : 55 + x * 40; }
        int Gray(int n) { int v = 8 + (n - 232) * 10; return (v << 16) | (v << 8) | v; }

        PR ParseLine(string s)
        {
            PR pr = new PR();
            int n = (s == null) ? 0 : s.Length;
            char[] ch = new char[n + 1];
            int[] fg = new int[n + 1], bg = new int[n + 1];
            int[] url = new int[n + 1];
            int curFg = -1, curBg = -1, k = 0, i = 0;
            bool inUrl = false;
            while (i < n)
            {
                char c = s[i];
                if (c == (char)ESC && i + 1 < n && s[i + 1] == '[')
                {
                    int j = i + 2;
                    int[] p = new int[16]; int pn = 0, cur = 0; bool any = false;
                    while (j < n && s[j] != 'm' && s[j] != (char)ESC)
                    {
                        if (s[j] >= '0' && s[j] <= '9') { cur = cur * 10 + (s[j] - '0'); any = true; }
                        else if (s[j] == ';') { if (pn < 16) p[pn] = any ? cur : 0; pn++; cur = 0; any = false; }
                        j++;
                    }
                    if (pn < 16 && any) p[pn++] = cur;
                    else if (!any && pn == 0) { p[0] = 0; pn = 1; }
                    int kk = 0;
                    while (kk < pn)
                    {
                        int code = p[kk];
                        if (code == 0) { curFg = -1; curBg = -1; }
                        else if (code >= 30 && code <= 37) curFg = PAL[code - 30];
                        else if (code >= 90 && code <= 97) curFg = PAL[code - 90 + 8];
                        else if (code >= 40 && code <= 47) curBg = PAL[code - 40];
                        else if (code >= 100 && code <= 107) curBg = PAL[code - 100 + 8];
                        else if (code == 38 || code == 48)
                        {
                            int isFg = (code == 38) ? 1 : 0;
                            if (kk + 2 < pn && p[kk + 1] == 5)
                            {
                                int v = p[kk + 2];
                                int col = (v < 16) ? PAL[v] : (v < 232 ? Cube(v) : Gray(v));
                                if (isFg != 0) curFg = col; else curBg = col; kk += 2;
                            }
                            else if (kk + 4 < pn && p[kk + 1] == 2)
                            {
                                int r = p[kk + 2], g = p[kk + 3], b = p[kk + 4];
                                int col = (r << 16) | (g << 8) | b;
                                if (isFg != 0) curFg = col; else curBg = col; kk += 4;
                            }
                        }
                        kk++;
                    }
                    i = (j < n && s[j] == 'm') ? j + 1 : j;
                }
                else if (c == '\r' || c == '\n') i++;
                else
                {
                    bool isU = IsUrlChar(c);
                    if (inUrl)
                    {
                        if (isU) { ch[k] = c; fg[k] = curFg; bg[k] = curBg; url[k] = 1; k++; }
                        else { inUrl = false; ch[k] = c; fg[k] = curFg; bg[k] = curBg; k++; }
                    }
                    else
                    {
                        int sl = UrlSchemeLen(s, i, n);
                        if (sl > 0) { inUrl = true; ch[k] = c; fg[k] = curFg; bg[k] = curBg; url[k] = 1; k++; }
                        else { ch[k] = c; fg[k] = curFg; bg[k] = curBg; k++; }
                    }
                    i++;
                }
            }
            pr.ch = ch; pr.fg = fg; pr.bg = bg; pr.url = url; pr.len = k;
            return pr;
        }

        // ---- mini-BCL string helpers (MiniCLR has no Substring/IndexOf/etc) ----
        static bool IsWordChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                   c == '/' || c == '.' || c == ':';
        }
        // ---- URL detection (MiniCLR-safe: no Substring/IndexOf) ----------
        static bool IsUrlChar(char c)
        {
            if (c >= 'a' && c <= 'z') return true;
            if (c >= 'A' && c <= 'Z') return true;
            if (c >= '0' && c <= '9') return true;
            if (c == '.' || c == '-' || c == '_' || c == ':' || c == '/' ||
                c == '?' || c == '#' || c == '&' || c == '=' || c == '+' ||
                c == '%' || c == '@' || c == '~') return true;
            return false;
        }
        // Returns the length of a URL scheme prefix at s[i..] (http(s)://,
        // ftp://, www.), else 0.  Bounds-checked so it is safe to call with
        // any i.
        static int UrlSchemeLen(string s, int i, int n)
        {
            if (i + 6 < n &&
                s[i] == 'h' && s[i + 1] == 't' && s[i + 2] == 't' && s[i + 3] == 'p' &&
                s[i + 4] == ':' && s[i + 5] == '/' && s[i + 6] == '/') return 7;
            if (i + 7 < n &&
                s[i] == 'h' && s[i + 1] == 't' && s[i + 2] == 't' && s[i + 3] == 'p' &&
                s[i + 4] == 's' && s[i + 5] == ':' && s[i + 6] == '/' && s[i + 7] == '/') return 8;
            if (i + 5 < n &&
                s[i] == 'f' && s[i + 1] == 't' && s[i + 2] == 'p' &&
                s[i + 3] == ':' && s[i + 4] == '/' && s[i + 5] == '/') return 6;
            if (i + 3 < n &&
                s[i] == 'w' && s[i + 1] == 'w' && s[i + 2] == 'w' && s[i + 3] == '.') return 4;
            return 0;
        }
        static string Trim(string s)
        {
            int a = 0, b = s.Length;
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
            if (b < a) b = a;
            return U.Sub(s, a, b - a);
        }
        static string Lower(string s)
        {
            string r = "";
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                r = U.Cat(r, Host.CharStr((int)c));
            }
            return r;
        }
        static int IndexOf(string s, string sub)
        {
            int n = s.Length, m = sub.Length;
            if (m == 0) return 0;
            for (int i = 0; i + m <= n; i++)
            {
                bool ok = true;
                for (int j = 0; j < m; j++) { if (s[i + j] != sub[j]) { ok = false; break; } }
                if (ok) return i;
            }
            return -1;
        }
        static bool StartsWith(string s, string p)
        {
            int m = p.Length;
            if (s.Length < m) return false;
            for (int i = 0; i < m; i++) if (s[i] != p[i]) return false;
            return true;
        }
        // Insert / delete on a string (used for the input line editing).
        static string InsStr(string s, int pos, string ins)
        {
            return U.Cat(U.Sub(s, 0, pos), U.Cat(ins, U.Sub(s, pos, s.Length - pos)));
        }
        static string DelStr(string s, int pos, int len)
        {
            return U.Cat(U.Sub(s, 0, pos), U.Sub(s, pos + len, s.Length - pos - len));
        }

        // ---- layout --------------------------------------------------
        void ComputeColsRows()
        {
            int W = Gfx.Width(), H = Gfx.Height();
            contentX = PAD; contentY = TAB_H + PAD;
            contentW = W - contentX * 2 - 8;
            contentH = H - contentY - PAD;
            if (contentW < 40) contentW = 40;
            if (contentH < 40) contentH = 40;
            int ch2 = Theme.TermCellH; if (ch2 < 12) ch2 = 12; if (ch2 > 28) ch2 = 28;
            cellH = ch2;
            int w0 = Gfx.Measure("0"); if (w0 < 4) w0 = 8; if (w0 > 20) w0 = 10;
            cellW = (w0 * ch2) / 18; if (cellW < 4) cellW = 4;
            // Diff 模式下把终端内容整体下移，给双栏面板让位；
            // contentH 必须同步扣减，否则输入行会被挤出窗口。
            if (isDiffMode)
            {
                int dh = DiffH() + PAD;
                contentY += dh;
                contentH -= dh;
            }
            contentH -= 18;              // 底部提示条
            if (contentH < 20) contentH = 20;

            rows = contentH / cellH; cols = contentW / cellW;
            if (rows < 1) rows = 1; if (cols < 1) cols = 1;
        }
        void ComputeLayout(Term t)
        {
            int ndoc = t.count + 1;
            prs = new PR[ndoc];
            rowLine = new int[ROWCAP]; rowStart = new int[ROWCAP]; rowEnd = new int[ROWCAP]; rowN = 0;
            for (int idx = 0; idx < ndoc; idx++)
            {
                string s = (idx < t.count) ? RingAt(t, idx) : U.Cat(PS1(), t.input);
                PR pr = ParseLine(s); prs[idx] = pr;
                int L = pr.len, pos = 0;
                while (pos < L && rowN < ROWCAP - 1)
                {
                    int take = (L - pos < cols) ? (L - pos) : cols;
                    rowLine[rowN] = idx; rowStart[rowN] = pos; rowEnd[rowN] = pos + take; rowN++;
                    pos += take;
                }
            }
            lastTotalRows = rowN;
            visRows = rows;
            firstAbsRow = rowN - visRows - t.view;
            if (firstAbsRow < 0) firstAbsRow = 0;
            for (int vr = 0; vr < rows; vr++)
            {
                int absRow = firstAbsRow + vr, b = vr * cols;
                if (absRow < 0 || absRow >= rowN) { for (int c = 0; c < cols && b + c < GRIDCAP; c++) gLine[b + c] = -1; continue; }
                for (int c = 0; c < cols && b + c < GRIDCAP; c++)
                {
                    int ca = rowStart[absRow] + c;
                    if (ca < rowEnd[absRow]) { gLine[b + c] = rowLine[absRow]; gChar[b + c] = ca; }
                    else gLine[b + c] = -1;
                }
            }
            gN = rows * cols; if (gN > GRIDCAP) gN = GRIDCAP;
        }
        int[] CellAt(int mx, int my)
        {
            if (my < contentY || my >= contentY + contentH || mx < contentX || mx >= contentX + contentW)
                return new int[] { -1, -1 };
            int col = (mx - contentX) / cellW, vr = (my - contentY) / cellH;
            if (vr < 0 || vr >= rows) return new int[] { -1, -1 };
            int idx = vr * cols + col; if (idx >= gN) return new int[] { -1, -1 };
            int line = gLine[idx]; if (line < 0) return new int[] { -1, -1 };
            return new int[] { line, gChar[idx] };
        }
        bool CellSelected(Term t, int line, int chr)
        {
            if (!t.hasSel) return false;
            long ka = (long)line * 1000000L + (long)chr;
            long a = (long)t.selLineA * 1000000L + (long)t.selCharA;
            long b = (long)t.selLineB * 1000000L + (long)t.selCharB;
            if (a > b) { long tmp = a; a = b; b = tmp; }
            return ka >= a && ka <= b;
        }
        int LineLen(Term t, int line)
        {
            string s = (line < t.count) ? RingAt(t, line) : U.Cat(PS1(), t.input);
            return ParseLine(s).len;
        }
        int[] WordBounds(Term t, int line, int chr)
        {
            string s = (line < t.count) ? RingAt(t, line) : U.Cat(PS1(), t.input);
            PR pr = ParseLine(s);
            int start = WordStartC(pr.ch, pr.len, chr), end = WordEndC(pr.ch, pr.len, chr);
            return new int[] { start, end };
        }
        bool IsWord(char c) { return IsWordChar(c); }
        int WordStart(string s, int pos)
        {
            int i = pos; while (i > 0 && !IsWord(s[i - 1])) i--;
            while (i > 0 && IsWord(s[i - 1])) i--; return i;
        }
        int WordEnd(string s, int pos)
        {
            int i = pos, n = s.Length;
            while (i < n && !IsWord(s[i])) i++;
            while (i < n && IsWord(s[i])) i++; return i;
        }
        int WordStartC(char[] s, int n, int pos)
        {
            int i = pos; while (i > 0 && !IsWordChar(s[i - 1])) i--;
            while (i > 0 && IsWordChar(s[i - 1])) i--; return i;
        }
        int WordEndC(char[] s, int n, int pos)
        {
            int i = pos;
            while (i < n && !IsWordChar(s[i])) i++;
            while (i < n && IsWordChar(s[i])) i++; return i;
        }
        string GetSelText(Term t)
        {
            if (!t.hasSel) return "";
            if (t.selLineA > t.selLineB || (t.selLineA == t.selLineB && t.selCharA > t.selCharB))
            {
                int tl = t.selLineA; t.selLineA = t.selLineB; t.selLineB = tl;
                int tc = t.selCharA; t.selCharA = t.selCharB; t.selCharB = tc;
            }
            string r = "";
            for (int L = t.selLineA; L <= t.selLineB; L++)
            {
                PR pr = (L < t.count) ? ParseLine(RingAt(t, L)) : ParseLine(U.Cat(PS1(), t.input));
                int s0 = (L == t.selLineA) ? t.selCharA : 0;
                int e0 = (L == t.selLineB) ? t.selCharB : pr.len;
                if (e0 > s0) { for (int k2 = s0; k2 < e0; k2++) r = U.Cat(r, Host.CharStr((int)pr.ch[k2])); }
                if (L < t.selLineB) r = U.Cat(r, "\n");
            }
            return r;
        }
        int MaxView(Term t) { return (lastTotalRows > rows) ? (lastTotalRows - rows) : 0; }

        // Wallpaper hue at a given *screen* Y, blended 55% toward the Ubuntu
        // terminal purple.  Used when TermBgMode == 1 to make the terminal
        // background echo the live desktop behind it (pure-C# transparency
        // approximation -- the framebuffer has no alpha channel).
        int WallColorAt(int sy)
        {
            int H = Gfx.ScreenH(); if (H < 1) H = 1;
            int t = (sy * 100) / H; if (t < 0) t = 0; if (t > 100) t = 100;
            int a = (int)Theme.WallTop, b = (int)Theme.WallBot;
            int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
            int br = (b >> 16) & 0xFF, bg2 = (b >> 8) & 0xFF, bb = b & 0xFF;
            int r = (ar * (100 - t) + br * t) / 100;
            int g = (ag * (100 - t) + bg2 * t) / 100;
            int bl = (ab * (100 - t) + bb * t) / 100;
            int ur = (BG >> 16) & 0xFF, ug = (BG >> 8) & 0xFF, ub = BG & 0xFF;
            r = (r * 45 + ur * 55) / 100;
            g = (g * 45 + ug * 55) / 100;
            bl = (bl * 45 + ub * 55) / 100;
            return (r << 16) | (g << 8) | bl;
        }

        // Cycle the terminal cell size through a discrete set.  dir +1 = zoom
        // in, -1 = zoom out.  Pure int math (no BCL arrays-of-literals needed).
        // =================================================================
        //  IDE 特性 1/2/3 ：幽灵文本 / AST 高亮 / 可视化 Diff
        //  MiniCLR：无反射、无泛型、无 async、OnPaint 零分配。
        // =================================================================

        static bool TableHas(string table, string s, int at, int len)
        {
            if (table == null) return false;
            int n = table.Length, i = 0;
            while (i <= n)
            {
                int j = i;
                while (j < n && table[j] != '|') j++;
                if (j - i == len)
                {
                    bool ok = true;
                    for (int k = 0; k < len; k++)
                        if (table[i + k] != s[at + k]) { ok = false; break; }
                    if (ok) return true;
                }
                i = j + 1;
            }
            return false;
        }
        static bool IsIdentChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
        }

        // ---- 2) AST 级词法高亮 + 括号配对 ----
        void RebuildHighlight()
        {
            Term t = T();
            string s = t.input;
            int n = s.Length; if (n > IN_CAP) n = IN_CAP;
            bmOpenTop = 0;
            int i = 0;
            bool expectCmd = true;
            while (i < n)
            {
                char ch = s[i];
                if (ch == '#') { while (i < n) { inFg[i] = TK_STR; i++; } break; }
                if (ch == '"' || ch == '\'')
                {
                    char q = ch; int j = i + 1;
                    while (j < n)
                    {
                        if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
                        if (s[j] == q) { j++; break; }
                        j++;
                    }
                    while (i < j) { inFg[i] = TK_STR; i++; }
                    expectCmd = false; continue;
                }
                if (ch == '(' || ch == '[' || ch == '{' ||
                    ch == ')' || ch == ']' || ch == '}')
                {
                    if (ch == '(' || ch == '[' || ch == '{')
                    {
                        if (bmOpenTop < BM_CAP) { bmOpen[bmOpenTop] = i; bmOpenTop++; }
                        inFg[i] = TK_BAD;
                    }
                    else
                    {
                        int want = (ch == ')') ? '(' : (ch == ']' ? '[' : '{');
                        if (bmOpenTop > 0 && s[bmOpen[bmOpenTop - 1]] == want)
                        {
                            bmOpenTop--;
                            int a = bmOpen[bmOpenTop];
                            inFg[a] = TK_BM; inFg[i] = TK_BM;
                        }
                        else inFg[i] = TK_BAD;
                    }
                    i++; expectCmd = false; continue;
                }
                if (ch == ' ' || ch == '\t') { inFg[i] = TK_OP; i++; continue; }
                if (ch == '|' || ch == '&' || ch == '>' || ch == '<')
                {
                    inFg[i] = TK_OP; i++;
                    if (ch == '|' || ch == '&') expectCmd = true;
                    continue;
                }
                if (ch == '-' && i + 1 < n && IsIdentChar(s[i + 1]))
                {
                    int j = i + 1;
                    while (j < n && IsIdentChar(s[j])) j++;
                    while (i < j) { inFg[i] = TK_FLAG; i++; }
                    expectCmd = false; continue;
                }
                if (IsIdentChar(ch))
                {
                    int j = i;
                    while (j < n && IsIdentChar(s[j])) j++;
                    int len2 = j - i;
                    int col;
                    if (expectCmd || TableHas(KW_TABLE, s, i, len2)) col = TK_CMD;
                    else col = TK_DEF;
                    while (i < j) { inFg[i] = col; i++; }
                    expectCmd = false; continue;
                }
                if (ch >= '0' && ch <= '9')
                {
                    while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.'))
                    { inFg[i] = TK_DEF; i++; }
                    expectCmd = false; continue;
                }
                inFg[i] = TK_DEF; i++; expectCmd = false;
            }
            for (int b = 0; b < bmOpenTop; b++)
            {
                int a = bmOpen[b];
                if (a >= 0 && a < IN_CAP) inFg[a] = TK_BAD;
            }
            bmOpenTop = 0;
        }

        // ---- 1) 幽灵文本 ----
        void GhostTick()
        {
            Term t = T();
            if (aiOff == 2) return;
            int now = Host.TickMs();
            if (t.ghostFor != t.input) { t.ghost = ""; t.ghostFor = ""; }
            if (t.input.Length == 0) { t.ghost = ""; return; }
            if (t.ghost.Length > 0) return;
            if (t.ghostAskMs != 0 && now - t.ghostAskMs < GHOST_IDLE_MS) return;
#if WINHOST
            if (aiBusy != 0) return;
#endif
            t.ghostAskMs = now;
            t.ghostReqId++;
            GhostRequest(t.input, t.ghostReqId);
        }

        void GhostRequest(string cmd, int reqId)
        {
#if WINHOST
            aiBusy = 1;
            aiResultBase = cmd;
            int reqTab = active;
            string prompt = U.Cat(
                "Predict the next part of the terminal command. Reply ONLY the remaining fragment, max 40 chars, no quotes, no explanation. Current input:\n", cmd);
            NexOS.AI.AI.RequestCompletionAsync(prompt, NexOS.AI.AIMode.Fast)
                .ContinueWith(r =>
                {
                    aiResultText = (r.Result != null) ? r.Result.text : null;
                    aiResultKind = 1; aiResultTab = reqTab; aiBusy = 0;
                });
#else
            aiOff = 2;
            Host.Log("[term] ghost text: AI unavailable on this target (silent degrade)");
#endif
        }

        bool AcceptGhost()
        {
            Term t = T();
            if (t.ghost == null || t.ghost.Length == 0) return false;
            if (t.caret != t.input.Length) { t.ghost = ""; t.ghostFor = ""; return false; }
            t.input = U.Cat(t.input, t.ghost);
            t.caret = t.input.Length;
            t.ghost = ""; t.ghostFor = "";
            RebuildHighlight();
            return true;
        }

        // ---- 3) 错误识别 / 修复请求 ----
        bool LooksLikeError(string s)
        {
            if (s == null || s.Length == 0 || ERR_TABLE == null) return false;
            int tn = ERR_TABLE.Length, i = 0;
            while (i <= tn)
            {
                int j = i;
                while (j < tn && ERR_TABLE[j] != '|') j++;
                int plen = j - i;
                if (plen > 0 && plen <= s.Length)
                {
                    int last = s.Length - plen;
                    for (int k = 0; k <= last; k++)
                    {
                        bool ok = true;
                        for (int m = 0; m < plen; m++)
                            if (s[k + m] != ERR_TABLE[i + m]) { ok = false; break; }
                        if (ok) return true;
                    }
                }
                i = j + 1;
            }
            return false;
        }
        static string FirstLine(string s)
        {
            if (s == null) return "";
            for (int i = 0; i < s.Length; i++)
                if (s[i] == '\n' || s[i] == '\r') return U.Sub(s, 0, i);
            return s;
        }
        void RequestFix(string cmd, string errLine)
        {
#if WINHOST
            if (aiBusy != 0) return;
            aiBusy = 1;
            string prompt = U.Cat(
                "The shell command failed. Reply ONLY the corrected command, no explanation, no markdown fence, no quotes.\ncommand: ", cmd,
                "\nerror: ", errLine);
            NexOS.AI.AI.RequestCompletionAsync(prompt, NexOS.AI.AIMode.Balanced)
                .ContinueWith(r =>
                {
                    aiResultText = (r.Result != null) ? r.Result.text : null;
                    aiResultKind = 2; aiBusy = 0;
                });
#else
            aiOff = 2;
#endif
        }

        static string CleanCmd(string s)
        {
            if (s == null) return "";
            int a = 0, b = s.Length;
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
            if (a + 2 < b && s[a] == '`' && s[a + 1] == '`' && s[a + 2] == '`')
            {
                a += 3;
                while (a < b && s[a] != '\n' && s[a] != '\r') a++;
                while (a < b && (s[a] == '\n' || s[a] == '\r')) a++;
            }
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
            if (b - 2 > a && s[b - 1] == '`' && s[b - 2] == '`' && s[b - 3] == '`')
            {
                b -= 3;
                while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
            }
            while (a < b && (s[a] == '"' || s[a] == '\'' || s[a] == '`')) a++;
            while (b > a && (s[b - 1] == '"' || s[b - 1] == '\'' || s[b - 1] == '`')) b--;
            for (int i = a; i < b; i++)
                if (s[i] == '\n' || s[i] == '\r') { b = i; break; }
            while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
            if (b <= a) return "";
            return U.Sub(s, a, b - a);
        }
        static bool StartsWithStr(string s, string p)
        {
            if (s == null || p == null || s.Length < p.Length) return false;
            for (int i = 0; i < p.Length; i++) if (s[i] != p[i]) return false;
            return true;
        }

        void PollAI()
        {
#if WINHOST
            if (aiResultKind == 0) return;
            int kind = aiResultKind; string txt = aiResultText;
            aiResultKind = 0; aiResultText = null;
            if (kind == 1)
            {
                if (aiResultTab < 0 || aiResultTab >= tabN) return;
                Term t = tabs[aiResultTab];
                if (t.input != aiResultBase) return;
                if (txt == null || txt.Length == 0) return;
                string g = CleanCmd(txt);
                if (g.Length == 0) return;
                if (g.Length >= t.input.Length && StartsWithStr(g, t.input))
                    g = U.Sub(g, t.input.Length, g.Length - t.input.Length);
                if (g.Length == 0 || g == t.input) return;
                if (g.Length > GHOST_MAX) g = U.Sub(g, 0, GHOST_MAX);
                t.ghost = g; t.ghostFor = t.input;
            }
            else if (kind == 2)
            {
                string f = CleanCmd(txt);
                if (f.Length == 0 || f == originalCmd) return;
                fixedCmd = f;
                isDiffMode = true;
            }
#endif
        }

        int DiffH()
        {
            int hh = Gfx.Height() / 2 - PAD;
            if (hh < 80) hh = 80;
            if (hh > Gfx.Height() - 160) hh = Gfx.Height() - 160;
            return hh;
        }

        void DrawDiff()
        {
            if (!isDiffMode) return;
            int x = PAD, y = TAB_H + PAD;
            int w = Gfx.Width() - PAD * 2 - 8;
            int hh = DiffH();
            if (w < 120 || hh < 60) return;
            Gfx.FillRound(x, y, w, hh, 8, (uint)DF_BG);
            Gfx.DrawRound(x, y, w, hh, 8, (uint)DF_BORDER);
            Gfx.Text(x + 12, y + 10, "AI fix - Y accept & re-run, Esc ignore", (uint)DF_HEAD);
            Gfx.DrawLine(x + 12, y + 32, x + w - 12, y + 32, (uint)DF_BORDER);
            int colW = (w - 36) / 2;
            int lx = x + 12, rx = x + 24 + colW;
            Gfx.Text(lx, y + 40, "original / error", (uint)0xAAAAAA);
            Gfx.Text(rx, y + 40, "fixed", (uint)0xAAAAAA);
            int ty = y + 62;
            int avail = hh - (ty - y) - 10;
            DrawWrapped(lx, ty, colW - 12, avail / 2, originalCmd, (uint)DF_DEL);
            DrawWrapped(lx, ty + avail / 2, colW - 12, avail / 2, diffErrLine, (uint)0xCC8888);
            DrawWrapped(rx, ty, colW - 12, avail, fixedCmd, (uint)DF_ADD);
        }

        void DrawWrapped(int x, int y, int maxw, int maxh, string s, uint c)
        {
            if (s == null) return;
            int colsN = maxw / cellW; if (colsN < 4) colsN = 4;
            int maxLines = maxh / cellH; if (maxLines < 1) maxLines = 1;
            int i = 0, line = 0;
            while (i < s.Length && line < maxLines)
            {
                int take = s.Length - i; if (take > colsN) take = colsN;
                int real = 0;
                while (real < take && s[i + real] != '\n' && s[i + real] != '\r') real++;
                Gfx.Text(x, y + line * cellH, U.Sub(s, i, real), c);
                if (real < take) i += real + 1; else i += real;
                line++;
            }
        }
        public static int ZoomStep(int cur, int dir)
        {
            int[] steps = new int[6];
            steps[0] = 14; steps[1] = 16; steps[2] = 18; steps[3] = 20; steps[4] = 22; steps[5] = 24;
            int idx = 2;
            for (int i = 0; i < 6; i++) if (steps[i] == cur) { idx = i; break; }
            idx += dir; if (idx < 0) idx = 0; if (idx > 5) idx = 5;
            return steps[idx];
        }

        // ---- tabs ----------------------------------------------------
        void NewTab()
        {
            if (tabN >= TAB_MAX) return;
            tabs[tabN] = new Term(); tabN++; active = tabN - 1; RebuildHighlight();
        }
        void CloseTab()
        {
            if (tabN <= 1) { Shell.Close(this.id); return; }
            for (int i = active; i < tabN - 1; i++) tabs[i] = tabs[i + 1];
            tabN--; if (active >= tabN) active = tabN - 1; RebuildHighlight();
        }

        // ---- commands -------------------------------------------------
        void Run()
        {
            Term t = T();
            string cmd = t.input;
            t.input = ""; t.caret = 0; t.hasSel = false; t.search = false; t.sbuf = "";
            t.histPos = t.histN;
            AppendLine(t, U.Cat(PS1(), cmd));
            if (Trim(cmd).Length == 0) return;
            if (t.histN == 0 || t.hist[t.histN - 1] != cmd)
            {
                if (t.histN < HIST_MAX) { t.hist[t.histN] = cmd; t.histN++; }
            }
            t.histPos = t.histN;
            if (cmd == "exit" || cmd == "logout") { CloseTab(); return; }
            if (cmd == "clear" || cmd == "cls") { ClearScreen(t); return; }
            string outp = RunBuiltin(t, cmd);
            if (outp == null) outp = Host.Exec(cmd);
            if (outp != null && outp.Length > 0)
            {
                AppendLine(t, outp);
                // 错误捕获 → 请求 AI 修复（本帧只挂请求，不阻塞绘制）
                if (LooksLikeError(outp))
                {
                    originalCmd = cmd;
                    diffErrLine = FirstLine(outp);
                    RequestFix(cmd, diffErrLine);
                }
            }
        }
        string RunBuiltin(Term t, string cmd)
        {
            string c = Trim(cmd), low = Lower(c);
            if (low == "help") return "NexOS shell - builtins: help ver mem date time ls cat echo clear pwd whoami history uname exit";
            if (low == "ver") return "NexOS 1.0 / .NET";
            if (low == "mem") return U.Cat("total ", U.I(Host.MemTotalKb() / 1024), " MB");
            if (low == "date" || low == "time") return U.Cat(U.I(Host.Hour()), ":", Pad2(Host.Minute()), ":", Pad2(Host.Second()));
            if (low == "whoami") return user;
            if (low == "pwd") return U.Cat("/home/", user);
            if (low == "uname") return "NexOS";
            if (low == "history")
            {
                string r = "";
                for (int i = 0; i < t.histN; i++) r = U.Cat(r, U.I(i + 1), "  ", t.hist[i], "\n");
                return r;
            }
            if (low == "ls")
            {
                string r = "";
                for (int fs = 0; fs < 2; fs++)
                {
                    int n = Host.FileCount(fs);
                    for (int i = 0; i < n && i < 40; i++) r = U.Cat(r, Host.FileName(fs, i), " ");
                }
                return r.Length > 0 ? r : "(empty)";
            }
            if (StartsWith(low, "cat "))
            {
                string f = Trim(U.Sub(c, 4, c.Length - 4));
                string b = Host.ReadText(1, f); if (b == null || b.Length == 0) b = Host.ReadText(0, f);
                if (b == null || b.Length == 0) return U.Cat("cat: ", f, ": No such file");
                return b;
            }
            if (StartsWith(low, "echo ")) return U.Sub(c, 5, c.Length - 5);
            return null;
        }

        // ---- keyboard ------------------------------------------------
        public override void OnKey(int ch)
        {
            Term t = T();
            // ---- Diff 模式优先接管：Y 接受并后台重跑，Esc 忽略，其余吞掉 ----
            if (isDiffMode)
            {
                if (ch == 'y' || ch == 'Y')
                {
                    pendingCmd = fixedCmd; pendingRun = true;
                    isDiffMode = false; originalCmd = ""; fixedCmd = ""; diffErrLine = "";
                    return;
                }
                if (ch == VK.Esc)
                {
                    isDiffMode = false; originalCmd = ""; fixedCmd = ""; diffErrLine = "";
                    return;
                }
                return;
            }

            if (t.search) { KeySearch(t, ch); return; }
            if (ch >= 32)
            {
                t.input = InsStr(t.input, t.caret, Host.CharStr((char)ch));
                t.caret++; t.hasSel = false;
                t.ghost = ""; t.ghostFor = ""; t.ghostAskMs = Host.TickMs();
                RebuildHighlight();
                return;
            }
            switch (ch)
            {
                case VK.Enter:
                    t.ghost = ""; t.ghostFor = ""; t.ghostAskMs = 0;
                    Run(); break;
                case VK.Back: if (t.caret > 0) { t.input = DelStr(t.input, t.caret - 1, 1); t.caret--; } t.ghost = ""; t.ghostAskMs = Host.TickMs(); RebuildHighlight(); break;
                case VK.Delete: if (t.caret < t.input.Length) t.input = DelStr(t.input, t.caret, 1); t.ghost = ""; t.ghostAskMs = Host.TickMs(); RebuildHighlight(); break;
                case VK.CtrlC:
                    if (t.hasSel) Host.SetClipboard(GetSelText(t));
                    else { AppendLine(t, U.Cat(PS1(), "^C")); t.input = ""; t.caret = 0; t.hasSel = false; }
                    break;
                case VK.CtrlV:
                case VK.CsV:
                    { string cb = Host.GetClipboard(); if (cb != null && cb.Length > 0) { t.input = InsStr(t.input, t.caret, cb); t.caret += cb.Length; } }
                    break;
                case VK.CsC: if (t.hasSel) Host.SetClipboard(GetSelText(t)); break;
                case VK.CsT: NewTab(); break;          // Ctrl+Shift+T new tab
                case VK.CsW: CloseTab(); break;        // Ctrl+Shift+W close tab
                case VK.CtrlA: case VK.HomeK: t.caret = 0; break;
                case VK.CtrlE: case VK.EndK: t.caret = t.input.Length; break;
                case VK.CtrlU: t.input = U.Sub(t.input, t.caret, t.input.Length - t.caret); t.caret = 0; break;
                case VK.CtrlK: t.input = U.Sub(t.input, 0, t.caret); break;
                case VK.CtrlW: { int s = WordStart(t.input, t.caret); t.input = DelStr(t.input, s, t.caret - s); t.caret = s; } break;
                case VK.CtrlL: ClearScreen(t); break;
                case VK.CtrlD: if (t.input.Length == 0) CloseTab(); break;
                case VK.CtrlZ: AppendLine(t, "[1]+  Stopped"); break;
                case VK.CtrlR: t.search = true; t.sbuf = ""; break;
                case VK.AltF: t.caret = WordEnd(t.input, t.caret); break;
                case VK.AltB: t.caret = WordStart(t.input, t.caret); break;
                case VK.Tab:
                    // 拦截 Tab 默认行为（防焦点跳出）：有预测则采纳，否则补全
                    if (AcceptGhost()) break;
                    Complete(t); break;
                case VK.Up: HistUp(t); break;
                case VK.Down: HistDown(t); break;
                case VK.Left: if (t.caret > 0) t.caret--; break;
                case VK.Right: if (t.caret < t.input.Length) t.caret++; break;
                case VK.PageUp: t.view += rows; if (t.view > MaxView(t)) t.view = MaxView(t); break;
                case VK.PageDown: t.view -= rows; if (t.view < 0) t.view = 0; break;
                case VK.Esc:
                    if (isDiffMode) { isDiffMode = false; originalCmd = ""; fixedCmd = ""; diffErrLine = ""; break; }
                    if (t.ghost.Length > 0) { t.ghost = ""; break; }
                    t.hasSel = false; break;
                case VK.CtrlPlus:  Theme.TermCellH = ZoomStep(Theme.TermCellH, 1);  Theme.Save(); break;
                case VK.CtrlMinus: Theme.TermCellH = ZoomStep(Theme.TermCellH, -1); Theme.Save(); break;
                case VK.Ctrl0:     Theme.TermCellH = 18; Theme.Save(); break;
                default:
                    if (ch <= -40 && ch >= -51) AppendLine(t, U.Cat("F", U.I(-ch - 39), " pressed"));
                    break;
            }
        }
        void KeySearch(Term t, int ch)
        {
            if (ch == VK.Enter) { t.search = false; t.sbuf = ""; return; }
            if (ch == VK.Esc) { t.search = false; t.sbuf = ""; t.input = ""; t.caret = 0; return; }
            if (ch == VK.Back) { if (t.sbuf.Length > 0) t.sbuf = U.Sub(t.sbuf, 0, t.sbuf.Length - 1); }
            else if (ch >= 32) t.sbuf = U.Cat(t.sbuf, Host.CharStr((char)ch));
            else return;
            string low = Lower(t.sbuf);
            for (int i = t.histN - 1; i >= 0; i--)
                if (IndexOf(Lower(t.hist[i]), low) >= 0) { t.input = t.hist[i]; t.caret = t.input.Length; break; }
        }
        void HistUp(Term t)
        {
            if (t.histN == 0) return;
            if (t.histPos > 0) t.histPos--;
            t.input = (t.histPos < t.histN) ? t.hist[t.histPos] : "";
            t.caret = t.input.Length;
        }
        void HistDown(Term t)
        {
            if (t.histPos < t.histN) t.histPos++;
            t.input = (t.histPos >= t.histN) ? "" : t.hist[t.histPos];
            t.caret = t.input.Length;
        }
        void Complete(Term t)
        {
            string[] cmds = { "help","ver","mem","date","time","ls","cat","echo",
                              "clear","pwd","whoami","history","uname","exit","net" };
            string tok = t.input; int m = 0; string first = "";
            for (int i = 0; i < cmds.Length; i++)
                if (StartsWith(cmds[i], tok)) { m++; if (m == 1) first = cmds[i]; }
            if (m == 1) { t.input = first; t.caret = first.Length; }
            else if (m > 1)
            {
                string r = "";
                for (int i = 0; i < cmds.Length; i++) if (StartsWith(cmds[i], tok)) r = U.Cat(r, cmds[i], " ");
                AppendLine(t, r);
            }
        }

        // ---- mouse ---------------------------------------------------
        public override void OnMouseDown(int btn, int mx, int my)
        {
            if (my < TAB_H)   // tab bar: switch
            {
                int tw = 140;
                for (int i = 0; i < tabN; i++)
                {
                    int tx = contentX + i * (tw + 4);
                    if (mx >= tx && mx < tx + tw) { if (active != i) { active = i; RebuildHighlight(); } break; }
                }
                return;
            }
            Term t = T();
            if (btn == 1)   // middle-click paste
            {
                string cb = Host.GetClipboard();
                if (cb != null && cb.Length > 0) { t.input = InsStr(t.input, t.caret, cb); t.caret += cb.Length; }
                return;
            }
            int[] cell = CellAt(mx, my); if (cell[0] < 0) return;
            int line = cell[0], chr = cell[1];
            int now = Host.TickMs(), cellKey = line * 100000 + chr;
            if (now - lastDownMs < 400 && cellKey == lastDownCell) clickCount++; else clickCount = 1;
            if (clickCount > 3) clickCount = 3;
            lastDownMs = now; lastDownCell = cellKey;
            if (clickCount >= 3)
            {
                int L = LineLen(t, line);
                t.selLineA = line; t.selCharA = 0; t.selLineB = line; t.selCharB = L; t.hasSel = true; t.dragging = false;
            }
            else if (clickCount == 2)
            {
                int[] wb = WordBounds(t, line, chr);
                t.selLineA = line; t.selCharA = wb[0]; t.selLineB = line; t.selCharB = wb[1]; t.hasSel = true; t.dragging = false;
            }
            else
            {
                t.dragging = true;
                t.selLineA = line; t.selCharA = chr; t.selLineB = line; t.selCharB = chr; t.hasSel = false;
            }
        }
        public override void OnMouseMove(int mx, int my)
        {
            hoverX = mx; hoverY = my;
            Term t = T();
            if (my < TAB_H) { hoverLine = -1; hoverChar = -1; }
            else if (gN > 0)
            {
                int[] cell = CellAt(mx, my);
                if (cell[0] >= 0) { hoverLine = cell[0]; hoverChar = cell[1]; }
                else { hoverLine = -1; hoverChar = -1; }
            }
            if (!t.dragging) return;
            if (my < TAB_H) return;
            int[] c2 = CellAt(mx, my); if (c2[0] < 0) return;
            t.selLineB = c2[0]; t.selCharB = c2[1]; t.hasSel = true;
        }
        public override void OnMouseUp(int btn, int mx, int my)
        {
            Term t = T();
            if (t.dragging) t.dragging = false;
            // GNOME Terminal copies to the PRIMARY selection the moment a
            // selection is finalised -- by drag-release OR by double / triple
            // click.  Mirror that: any active selection lands on the clipboard.
            if (t.hasSel) Host.SetClipboard(GetSelText(t));
        }
        public override void OnWheel(int dy)
        {
            Term t = T();
            if (dy > 0) { t.view++; if (t.view > MaxView(t)) t.view = MaxView(t); }
            else if (dy < 0) { t.view--; if (t.view < 0) t.view = 0; }
        }
        public override void OnClick(int mx, int my) { }

        // ---- paint ---------------------------------------------------
        void DrawTabBar()
        {
            Gfx.FillRect(0, 0, Gfx.Width(), TAB_H, 0x1A0820);
            int tw = 140;
            for (int i = 0; i < tabN; i++)
            {
                int tx = contentX + i * (tw + 4), ty = 4, th = TAB_H - 8;
                bool act = (i == active);
                Gfx.FillRound(tx, ty, tw, th, 4, (uint)(act ? 0x3465A4 : 0x2A1030));
                Gfx.Text(tx + 10, ty + (th - 16) / 2, tabs[i].title, (uint)(act ? 0xFFFFFF : 0xC0C0C0));
                if (act) Gfx.Text(tx + tw - 16, ty + (th - 16) / 2, "x", 0xDDDDDD);
            }
        }
        // 幽灵文本：贴着输入行光标之后，用暗灰紫模拟“半透明”（显存无 alpha）
        void DrawGhostText(Term t)
        {
            if (t.ghost == null || t.ghost.Length == 0) return;
            if (t.caret != t.input.Length) return;        // 只在行尾提示
            if (rowN <= 0 || prs == null) return;

            // 输入行是文档最后一行；找到它被折到哪一视觉行、哪一列结束
            int lastLine = t.count;
            int absRow = -1;
            for (int r = 0; r < rowN; r++) if (rowLine[r] == lastLine) absRow = r;
            if (absRow < 0) return;
            int vis = absRow - firstAbsRow;
            if (vis < 0 || vis >= rows) return;            // 滚出视口就不画

            int colAt = rowEnd[absRow] - rowStart[absRow];
            int gx = contentX + colAt * cellW;
            int gy = contentY + vis * cellH;

            // 裁掉超出右边界部分，避免画出窗口
            int room = (contentX + contentW - gx) / cellW;
            if (room < 1) return;
            string g = t.ghost;
            if (g.Length > room) g = U.Sub(g, 0, room);
            Gfx.Text(gx, gy, g, (uint)TK_GHOST);
        }
        void DrawScrollbar(Term t)
        {
            int x = contentX + contentW + 2, y = contentY, w = 6, h = contentH;
            Gfx.FillRect(x, y, w, h, 0x1A0820);
            int total = lastTotalRows; if (total < 1) total = 1;
            int thumbH = h, ty = y;
            if (total > rows)
            {
                thumbH = (h * rows) / total; if (thumbH < 10) thumbH = 10;
                int maxOff = h - thumbH;
                int off = (total <= rows) ? 0 : (maxOff * (total - rows - t.view) / (total - rows));
                ty = y + off;
            }
            Gfx.FillRect(x, ty, w, thumbH, 0x555555);
        }
        public override void OnPaint()
        {
            int W = Gfx.Width(), H = Gfx.Height();
            Term t = T();
            ComputeColsRows();

            // ---- 帧首：搬运后台 AI 结果、推进幽灵文本、执行排队重跑 ----
            PollAI();
            GhostTick();
            if (pendingRun)
            {
                string pc = pendingCmd;
                pendingRun = false; pendingCmd = "";
                Term pt = T();
                AppendLine(pt, U.Cat(PS1(), pc, "   (AI fix)"));
                string po = RunBuiltin(pt, pc);
                if (po == null) po = Host.Exec(pc);
                if (po != null && po.Length > 0) AppendLine(pt, po);
            }
            ComputeLayout(t);
            DrawTabBar();
            if (isDiffMode) DrawDiff();
            uint baseBg = (Theme.TermBgMode != 0)
                ? (uint)WallColorAt(Gfx.OriginY() + contentY)
                : (uint)BG;
            Gfx.FillRect(0, 0, W, H, baseBg);
            for (int vr = 0; vr < rows; vr++)
            {
                int y = contentY + vr * cellH;
                int absRow = firstAbsRow + vr;
                if (absRow < 0 || absRow >= rowN) continue;
                PR pr = prs[rowLine[absRow]];
                int start = rowStart[absRow], end = rowEnd[absRow];
                int isInput = (rowLine[absRow] == t.count) ? 1 : 0;
                uint rowBg = (Theme.TermBgMode != 0)
                    ? (uint)WallColorAt(Gfx.OriginY() + y)
                    : (uint)BG;
                Gfx.FillRect(0, y, W, cellH, rowBg);   // band covers full width + margins
                int defBg = (int)rowBg;
                for (int i = start; i < end; i++)
                {
                    int xc = contentX + (i - start) * cellW;
                    int ch = (int)pr.ch[i];
                    int fg = pr.fg[i]; if (fg < 0) fg = FG;
                    // ---- AST 高亮：仅对最后一行（当前输入行）按 token 着色 ----
                    // 历史输出行保留原有 ANSI 颜色，两者互不干扰。
                    if (isInput != 0 && pr.fg[i] < 0 && i < IN_CAP)
                    {
                        int tk = inFg[i];
                        if (tk != TK_DEF) fg = tk;
                    }
                    int bg = pr.bg[i]; if (bg < 0) bg = defBg;
                    bool sel = CellSelected(t, rowLine[absRow], i);
                    if (sel) Gfx.FillRect(xc, y, cellW, cellH, SEL);
                    // URL hover highlight: expand from hoverChar to the URL span
                    bool hot = false;
                    if (hoverLine == rowLine[absRow] && pr.url != null && pr.url[i] != 0)
                    {
                        int s = i, e = i;
                        while (s - 1 >= start && pr.url[s - 1] != 0) s--;
                        while (e + 1 < end && pr.url[e + 1] != 0) e++;
                        if (hoverChar >= s && hoverChar <= e) hot = true;
                    }
                    if (ch != ' ' && ch != '\t')
                    {
                        string s2 = Host.CharStr((char)ch);
                        if (hot) fg = URLC;
                        if (bg == defBg)
                        {
                            if (hot) Gfx.TextBg(xc, y, s2, (uint)fg, rowBg);
                            else Gfx.Text(xc, y, s2, (uint)fg);
                        }
                        else Gfx.TextBg(xc, y, s2, (uint)fg, (uint)bg);
                    }
                    if (hot) Gfx.FillRect(xc, y + cellH - 2, cellW, 2, URLC);
                    if (isInput != 0 && i == t.caret)
                    {
                        if (t.hasSel) Gfx.FillRect(xc, y, 2, cellH, 0xFFFFFF);          // I-beam
                        else
                        {
                            Gfx.FillRect(xc, y, cellW, cellH, 0xFFFFFF);                 // block
                            if (ch != ' ' && ch != '\t') Gfx.Text(xc, y, Host.CharStr((char)ch), (uint)defBg);
                        }
                    }
                }
            }
            DrawGhostText(t);
            DrawScrollbar(t);
        }
    }
}