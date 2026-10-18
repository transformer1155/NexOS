// =====================================================================
//  TerminalApp.cs  -  IDE 级智能终端（幽灵文本 + AST 高亮 + 可视化 Diff）
// ---------------------------------------------------------------------
//  本文件是 csharp/apps/Shell/Apps.cs 中 TerminalApp 的替代实现。
//
//  启用方式（三处都要改，见文件末尾“集成说明”）：
//    1. 删除 Apps.cs 里现有的 `public class TerminalApp : App { ... }` 整块；
//    2. 把本文件加进 Shell.csproj / mex.shared.props / WinHost.csproj 的
//       <Compile Include="..."> 列表。
//    （在本文件被加入编译列表之前，它不参与任何构建，因此不会造成重复定义。）
//
//  MiniCLR 约束（血泪版，勿违反）：
//    * 无 Task / async / 泛型 / 反射 / try-catch / 浮点 / string.Format；
//    * 堆是每帧回卷的 bump 堆 —— OnPaint 里绝不允许持续 new；所有缓存数组
//      在 ctor 里一次性分配，复用到帧尾；
//    * 字符串只有 Length / [i] / Concat —— 一律走 U.Cat / U.Sub（单次分配），
//      禁止 `for(...) r = U.Cat(r, ch)` 的 O(n^2) 拼接；
//    * 静态字段初始化器不会执行（无 .cctor），静态量必须在使用前显式赋值。
//
//  AI 网关离线的降级：所有智能特性（幽灵文本 / Diff 修复）在拿不到结果时
//  静默关闭，终端保持最基础的输入输出。
// =====================================================================
#if WINHOST
using NexOS.AI;
#endif
using NexOS.Forms;

namespace NexOS.Forms
{
    public class TerminalApp : App
    {
        // ---- 常量 ----------------------------------------------------
        const int SCROLL_MAX = 2000, HIST_MAX = 200, TAB_MAX = 8;
        const int TAB_H = 26, PAD = 8;
        const int ESC = 27;

        // 终端配色（Ubuntu 紫）
        const int BG = 0x300A24, FG = 0xE8E8E8;

        // 输入行高亮配色（与 Web 端保持同一套 token 色）
        const int TK_DEF   = 0xE8E8E8;   // 普通字符
        const int TK_CMD   = 0x4EC9B0;   // 命令 / 关键字
        const int TK_STR   = 0x6A9955;   // 字符串（绿）
        const int TK_FLAG  = 0xDCDCAA;   // -flag
        const int TK_OP    = 0xD4D4D4;   // 运算符
        const int TK_BM    = 0xFFD700;   // 配对成功的括号（金）
        const int TK_BAD   = 0xF85149;   // 未闭合 / 不匹配括号（红）
        const int TK_GHOST = 0x7A6A80;   // 幽灵文本（近似半透明：偏暗的灰紫）

        // Diff 面板配色
        const int DF_BG    = 0x1E0B14;
        const int DF_HEAD  = 0xFFB86C;
        const int DF_DEL   = 0xFF7B72;
        const int DF_ADD   = 0x7EE787;
        const int DF_BORDER= 0x5A3550;

        // 输入行词法缓存容量（一行命令足够；超出部分按普通色渲染）
        const int IN_CAP   = 512;
        const int BM_CAP   = 32;     // 括号配对栈深度

        // 关键字 / 错误特征 / 补全命令名：全部放在 ctor 里一次性赋值的普通
        // 字符串字段上（MiniCLR 无 .cctor，静态初始化器不会执行；同时避免
        // 每次按键都 new string[] —— 那会直接啃 bump 堆）。
        string KW_TABLE;      // "help|ls|cat|..."  命令位关键字
        string ERR_TABLE;     // "error|fail|..."   错误特征
        string CMD_TABLE;     // "help|ver|..."     Tab 补全候选

        // 幽灵文本节流（ms）
        const int GHOST_IDLE_MS = 350;
        const int GHOST_MAX     = 64;

        // =================================================================
        //  会话（一个 tab 一个 Term）
        // =================================================================
        public class Term
        {
            public string[] lines = new string[SCROLL_MAX];
            public int head = 0, count = 0;
            public int view = 0;
            public string input = "";
            public int caret = 0;
            public string[] hist = new string[HIST_MAX];
            public int histN = 0, histPos = 0;
            public string title = "root@nexos";

            // ---- 幽灵文本（每个 tab 各自独立）----
            public string ghost = "";        // 当前预测出的后续片段
            public string ghostFor = "";     // 该预测对应的输入（不一致则丢弃）
            public int ghostAskMs = 0;       // 上次发起请求的时刻
            public int ghostReqId = 0;       // 请求序号，丢弃过期结果
            public int ghostShownId = 0;     // 已被采纳/丢弃的序号
        }

        // =================================================================
        //  状态
        // =================================================================
        int active = 0;
        Term[] tabs;
        int tabN = 1;

        string user = "root", host = "nexos", cwd = "~";

        // 布局（每帧由 ComputeLayout 重算）
        int cols, rows, cellW, cellH;
        int contentX, contentY, contentW, contentH;

        // 输入行高亮缓存（ctor 一次性分配，之后只写不 new）
        int[] inFg;          // 每个字符的显示色
        int[] bmOpen;        // 配对栈：待匹配的左括号下标
        int   bmOpenTop;     // 栈顶

        // ---- Diff 状态（需求指定的成员名）----
        public bool isDiffMode;      // 是否正在展示修复建议
        string originalCmd;          // 出错的原命令
        string fixedCmd;             // AI 给出的修复命令
        string diffErrLine;          // 触发修复的错误行

        // 背景重跑：不在 OnPaint 里执行命令（会卡帧），推到下一帧帧首
        bool pendingRun;
        string pendingCmd;

        // AI 可用性闩锁：0 = 未知，1 = 可用，2 = 已判定不可用（静默降级）
        // 一旦判定不可用就不再尝试，避免重复发起注定失败的请求。
        int aiOff;

        // WINHOST 下的异步结果中转（后台线程写入，OnPaint 轮询读取）
#if WINHOST
        string aiResultText;         // 最近一次 AI 返回文本
        int    aiResultKind;         // 0 空闲 / 1 幽灵文本 / 2 修复命令
        int    aiResultId;           // 对应的请求序号
        int    aiResultTab;          // 请求发起时的 tab（结果只回填同一个 tab）
        string aiResultBase;         // 请求发起时的输入（输入已变则丢弃）
        int    aiBusy;               // 0 无在途请求 / 1 有
#endif

        // =================================================================
        //  构造
        // =================================================================
        public TerminalApp()
        {
            tabs = new Term[TAB_MAX];
            tabs[0] = new Term();
            tabN = 1; active = 0;

            // 所有数组在此一次性分配 —— OnPaint 只读不写新对象
            inFg    = new int[IN_CAP];
            bmOpen  = new int[BM_CAP];
            bmOpenTop = 0;

            // 字符串表（MiniCLR 无静态初始化器，必须在这里显式赋值）
            KW_TABLE  = U.Cat("open|theme|wall|ai|help|ver|cls|clear|ls|cat|echo|ps|",
                              "mem|date|time|whoami|uname|exit|run|agent|plugin|",
                              "pwd|history|mkdir|rm|cp|mv|touch|sudo|set|export");
            ERR_TABLE = U.Cat("error|Error|ERROR|fail|Fail|FAIL|exception|Exception|",
                              "not found|No such|denied|unknown|Unknown|",
                              "traceback|abort|Abort|invalid|Invalid");
            CMD_TABLE = U.Cat("help|ver|mem|date|time|ls|cat|echo|clear|pwd|",
                              "whoami|history|uname|exit");

            isDiffMode = false;
            originalCmd = ""; fixedCmd = ""; diffErrLine = "";
            pendingRun = false; pendingCmd = "";

            AppendLine(tabs[0],
                U.Cat("NexOS Terminal - IDE 智能终端。\n",
                      "语法高亮 / 括号配对已启用；输入停顿后出现灰色预测，",
                      "Tab 采纳，Y 接受 AI 修复，Esc 忽略。"));
        }

        Term T() { return tabs[active]; }

        public override string GetTitle()
        {
            return U.Cat(user, "@", host, ":", cwd, "$", "");
        }

        // 供 WinHost --termtest / Theme 使用，保持与原实现一致的公开面
        public Term ActiveTerm() { return T(); }
        public int TabCount() { return tabN; }
        public string UserHost() { return U.Cat(user, "@", host); }
        public void TestAppend(string s) { AppendLine(T(), s); }
        // 字号的离散档位：14/16/18/20/22/24。用算术推档位，不分配数组
        // （原实现每次按键 new int[6]，在 bump 堆上是无谓的消耗）。
        public static int ZoomStep(int cur, int dir)
        {
            int idx = 2;
            if (cur >= 14 && cur <= 24)
            {
                idx = (cur - 14) / 2;
                if (idx < 0) idx = 0; if (idx > 5) idx = 5;
            }
            idx += dir; if (idx < 0) idx = 0; if (idx > 5) idx = 5;
            return 14 + idx * 2;
        }
        public string LastLine()
        {
            Term t = T(); int b = t.head - 1; if (b < 0) b += SCROLL_MAX;
            return t.lines[b];
        }

        // =================================================================
        //  回卷缓冲 / 输出
        // =================================================================
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
            if (t.view > 0) t.view = 0;
        }
        void ClearScreen(Term t) { t.count = 0; t.head = 0; t.view = 0; }

        string Pad2(int v) { string s = U.I(v); if (s.Length < 2) s = U.Cat("0", s); return s; }
        string PS1()
        {
            // U.Cat 最多 7 个参数 —— 超过就要分步拼（和 Apps.cs 原实现一致）
            string ts = U.Cat("[", U.I(Host.Hour()), ":", Pad2(Host.Minute()), ":",
                              Pad2(Host.Second()), "] ");
            return U.Cat(ts, user, "@", host, ":", cwd, "$ ");
        }

        // =================================================================
        //  2) AST 级高亮 + 括号配对
        //     一次性重新计算整行：把每个字符的显示色写进 inFg[]。
        //     只在输入变化时调用，OnPaint 绝不重算，也不会分配内存。
        // =================================================================
        static bool IsIdentChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
        }
        // ---------------------------------------------------------------
        //  TableHas -- 在 "a|b|c" 形式的字段表里查找 [at,len) 是否等于
        //  某个条目。全程零分配，替代 MiniCLR 不可用的 string[] + Split。
        // ---------------------------------------------------------------
        static bool TableHas(string table, string s, int at, int len)
        {
            if (table == null) return false;
            int n = table.Length, i = 0;
            while (i <= n)
            {
                // 取一个 | 分隔的条目 [i, j)
                int j = i;
                while (j < n && table[j] != '|') j++;
                int wlen = j - i;
                if (wlen == len)
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

        // 是否是“命令位”关键字（行首或 | && ; 之后）
        bool IsKeyword(string s, int at, int len)
        {
            return TableHas(KW_TABLE, s, at, len);
        }

        void RebuildHighlight()
        {
            Term t = T();
            string s = t.input;
            int n = s.Length; if (n > IN_CAP) n = IN_CAP;
            bmOpenTop = 0;

            int i = 0;
            bool expectCmd = true;   // 行首 / 运算符之后期待命令
            while (i < n)
            {
                char c = s[i];

                // ---- 注释：到行尾全按字符串色（暗绿）----
                if (c == '#')
                {
                    while (i < n) { inFg[i] = TK_STR; i++; }
                    break;
                }

                // ---- 字符串：'...' / "..."（未闭合则剩余全按字符串色）----
                if (c == '"' || c == '\'')
                {
                    char q = c; int j = i + 1;
                    while (j < n)
                    {
                        if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
                        if (s[j] == q) { j++; break; }
                        j++;
                    }
                    while (i < j) { inFg[i] = TK_STR; i++; }
                    expectCmd = false;
                    continue;
                }

                // ---- 括号：配对即双方染金，落单染红 ----
                if (c == '(' || c == '[' || c == '{' ||
                    c == ')' || c == ']' || c == '}')
                {
                    if (c == '(' || c == '[' || c == '{')
                    {
                        if (bmOpenTop < BM_CAP) { bmOpen[bmOpenTop] = i; bmOpenTop++; }
                        inFg[i] = TK_BAD;   // 先假定未闭合，配对成功再改
                    }
                    else
                    {
                        int want = (c == ')') ? '(' : (c == ']' ? '[' : '{');
                        if (bmOpenTop > 0 && s[bmOpen[bmOpenTop - 1]] == want)
                        {
                            bmOpenTop--;
                            int a = bmOpen[bmOpenTop];
                            inFg[a] = TK_BM; inFg[i] = TK_BM;
                        }
                        else inFg[i] = TK_BAD;   // 多余的右括号
                    }
                    i++; expectCmd = false;
                    continue;
                }

                // ---- 空白 ----
                if (c == ' ' || c == '\t') { inFg[i] = TK_OP; i++; continue; }

                // ---- 运算符（| || && > < &）----
                if (c == '|' || c == '&' || c == '>' || c == '<')
                {
                    inFg[i] = TK_OP; i++;
                    // | || && 之后重新期待命令
                    if (c == '|' || c == '&') expectCmd = true;
                    continue;
                }

                // ---- -(flag) ----
                if (c == '-' && i + 1 < n && IsIdentChar(s[i + 1]))
                {
                    int j = i + 1;
                    while (j < n && IsIdentChar(s[j])) j++;
                    while (i < j) { inFg[i] = TK_FLAG; i++; }
                    expectCmd = false;
                    continue;
                }

                // ---- 标识符 / 命令 / 关键字 ----
                if (IsIdentChar(c))
                {
                    int j = i;
                    while (j < n && IsIdentChar(s[j])) j++;
                    int len = j - i;
                    int col;
                    // 命令位一律命令色；非命令位但命中关键字表（if/for/sudo/
                    // export …）也染蓝 —— 关键字在 shell 里本来就不限于行首。
                    if (expectCmd) col = TK_CMD;
                    else if (IsKeyword(s, i, len)) col = TK_CMD;
                    else col = TK_DEF;
                    while (i < j) { inFg[i] = col; i++; }
                    expectCmd = false;
                    continue;
                }

                // ---- 数字 ----
                if (c >= '0' && c <= '9')
                {
                    while (i < n && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) { inFg[i] = TK_DEF; i++; }
                    expectCmd = false;
                    continue;
                }

                // ---- 其它 ----
                inFg[i] = TK_DEF; i++; expectCmd = false;
            }

            // 未参与词法分析的尾部按普通色
            for (int k = n; k < s.Length && k < IN_CAP; k++) inFg[k] = TK_DEF;

            // 落单左括号保持红色（上面已置 TK_BAD）
            for (int b = 0; b < bmOpenTop; b++)
            {
                int a = bmOpen[b];
                if (a >= 0 && a < IN_CAP) inFg[a] = TK_BAD;
            }
            bmOpenTop = 0;   // 栈清空，下次输入重新配对
        }

        // =================================================================
        //  1) 幽灵文本：停顿 350ms 请求预测
        //     WINHOST 走异步 RequestCompletionAsync；MiniCLR 无 Task，静默降级。
        // =================================================================
        void GhostTick()
        {
            Term t = T();
            if (aiOff == 2) return;                            // 已判定 AI 不可用
            int now = Host.TickMs();

            // 输入已变 / 已无输入 → 作废旧预测
            if (t.ghostFor != t.input)
            {
                t.ghost = ""; t.ghostFor = "";
            }
            if (t.input.Length == 0) { t.ghost = ""; return; }
            if (t.ghost.Length > 0) return;                    // 已有可用预测
            if (t.ghostAskMs != 0 && now - t.ghostAskMs < GHOST_IDLE_MS) return;
#if WINHOST
            if (aiBusy != 0) return;                           // 有在途请求，别叠加
#endif
            t.ghostAskMs = now;
            t.ghostReqId++;
            GhostRequest(t.input, t.ghostReqId);
        }

        void GhostRequest(string cmd, int reqId)
        {
#if WINHOST
            aiBusy = 1;
            aiResultBase = cmd;                       // 记下请求时的输入
            int reqTab = active;
            string prompt = U.Cat(
                "你是一个终端命令预测助手。只回复用户可能接下来要输入的剩余命令片段",
                "（最多40字符，不要重复已有内容、不要解释、不要引号、不要换行）。当前已输入：\n", cmd);
            NexOS.AI.AI.RequestCompletionAsync(prompt, NexOS.AI.AIMode.Fast)
                .ContinueWith(r =>
                {
                    string s = (r.Result != null) ? r.Result.text : null;
                    aiResultText = s; aiResultKind = 1;
                    aiResultId = reqId; aiResultTab = reqTab; aiBusy = 0;
                });
#else
            // MiniCLR：没有 Task/async，无法在不阻塞 UI 的前提下请求网关。
            // 按约定静默降级 —— 幽灵文本关闭，终端其余功能不受影响。
            // 置闩锁，避免每次输入都重复做无用功。
            aiOff = 2;
            Log("ghost: ai unavailable on this target (silent degrade)");
#endif
        }

        // 采纳幽灵文本（Tab）。只有光标在行尾时才允许采纳 —— 预测是“续写”，
        // 在行中间插入会把命令拼错。
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

        // =================================================================
        //  3) 可视化 Diff：错误 → AI 修复 → 双栏面板 → Y 接受
        // =================================================================
        // 错误特征匹配：与 Web 端的 /error|fail|exception|not found|.../ 等价。
        // 用 "|" 分隔的字段表 + 子串扫描，零分配。
        bool LooksLikeError(string s)
        {
            if (s == null || s.Length == 0) return false;
            if (ERR_TABLE == null) return false;
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

        void RequestFix(string cmd, string errLine)
        {
#if WINHOST
            if (aiBusy != 0) return;
            aiBusy = 1;
            string prompt = U.Cat(
                "用户执行了 shell 命令但报错了。请只返回修复后的命令本身",
                "（不要解释、不要 markdown 代码块、不要引号）。\n命令：", cmd,
                "\n错误：", errLine);
            NexOS.AI.AI.RequestCompletionAsync(prompt, NexOS.AI.AIMode.Balanced)
                .ContinueWith(r =>
                {
                    string s = (r.Result != null) ? r.Result.text : null;
                    aiResultText = s; aiResultKind = 2; aiResultId = 0; aiBusy = 0;
                });
#else
            // MiniCLR：静默降级，不弹 Diff 面板。
            Log("diff: ai unavailable on this target");
#endif
        }

        // AI 返回文本清洗：去 ``` 围栏 / 去引号 / 取首行
        static string CleanCmd(string s)
        {
            if (s == null) return "";
            int a = 0, b = s.Length;

            // 先剥 ``` 围栏。顺序很重要：必须先认围栏再剥反引号，
            // 否则 "```sh" 会先被当成引号剥掉反引号，只剩 "sh"。
            // 跳过前导空白
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) a++;
            if (a + 2 < b && s[a] == '`' && s[a + 1] == '`' && s[a + 2] == '`')
            {
                a += 3;
                // 跳过语言标注（sh / bash / shell / console …）到行尾
                while (a < b && s[a] != '\n' && s[a] != '\r') a++;
                while (a < b && (s[a] == '\n' || s[a] == '\r')) a++;
            }

            // 跳过尾部空白
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
            // 去掉结尾的 ``` 围栏
            if (b - 2 > a && s[b - 1] == '`' && s[b - 2] == '`' && s[b - 3] == '`')
            {
                b -= 3;
                while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) b--;
            }

            // 再去掉普通引号
            while (a < b && (s[a] == '"' || s[a] == '\'' || s[a] == '`')) a++;
            while (b > a && (s[b - 1] == '"' || s[b - 1] == '\'' || s[b - 1] == '`')) b--;

            // 只取第一行（AI 偶尔会多给一行说明）
            for (int i = a; i < b; i++)
                if (s[i] == '\n' || s[i] == '\r') { b = i; break; }

            // 收尾再 trim 一次（第一行可能带尾随空格）
            while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;

            if (b <= a) return "";
            return U.Sub(s, a, b - a);
        }

        // WinHost：把后台线程的 AI 结果搬进 UI 状态（OnPaint 帧首调用，无分配）
        void PollAI()
        {
#if WINHOST
            if (aiResultKind == 0) return;
            int kind = aiResultKind; string txt = aiResultText;
            aiResultKind = 0; aiResultText = null;

            if (kind == 1)   // 幽灵文本
            {
                // 结果只回填到发起请求的那个 tab，且输入必须没变 ——
                // 否则用户可能已经切 tab 或继续打字，套用旧预测会显示错误内容。
                if (aiResultTab < 0 || aiResultTab >= tabN) return;
                Term t = tabs[aiResultTab];
                if (t.input != aiResultBase) return;
                if (txt == null || txt.Length == 0) return;
                string g = CleanCmd(txt);
                if (g.Length == 0) return;
                // 若 AI 把已输入部分也重复了一遍，则只保留增量
                if (g.Length >= t.input.Length && StartsWithStr(g, t.input))
                    g = U.Sub(g, t.input.Length, g.Length - t.input.Length);
                if (g.Length == 0 || g == t.input) return;
                if (g.Length > GHOST_MAX) g = U.Sub(g, 0, GHOST_MAX);
                t.ghost = g; t.ghostFor = t.input;
            }
            else if (kind == 2)   // 修复命令
            {
                string f = CleanCmd(txt);
                if (f.Length == 0) return;
                if (f == originalCmd) return;      // 与原文一致 = 没修好，别弹
                fixedCmd = f;
                isDiffMode = true;
            }
#endif
        }

        static bool StartsWithStr(string s, string p)
        {
            if (s == null || p == null) return false;
            if (s.Length < p.Length) return false;
            for (int i = 0; i < p.Length; i++) if (s[i] != p[i]) return false;
            return true;
        }

        // =================================================================
        //  命令执行
        // =================================================================
        void Run()
        {
            Term t = T();
            string cmd = t.input;
            t.input = ""; t.caret = 0;
            t.ghost = ""; t.ghostFor = ""; t.ghostAskMs = 0;
            t.histPos = t.histN;
            RebuildHighlight();

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

        static string FirstLine(string s)
        {
            if (s == null) return "";
            for (int i = 0; i < s.Length; i++)
                if (s[i] == '\n' || s[i] == '\r') return U.Sub(s, 0, i);
            return s;
        }

        string RunBuiltin(Term t, string cmd)
        {
            string c = Trim(cmd), low = Lower(c);
            if (low == "help")
                return U.Cat("NexOS shell - builtins: help ver mem date time ls cat echo ",
                             "clear pwd whoami history uname exit\n",
                             "IDE: Tab 采纳预测 / Y 接受修复 / Esc 忽略");
            if (low == "ver") return "NexOS 1.0 / .NET";
            if (low == "mem") return U.Cat("total ", U.I(Host.MemTotalKb() / 1024), " MB");
            if (low == "date" || low == "time")
                return U.Cat(U.I(Host.Hour()), ":", Pad2(Host.Minute()), ":", Pad2(Host.Second()));
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
            if (StartsWithStr(low, "cat "))
            {
                string f = Trim(U.Sub(c, 4, c.Length - 4));
                string b = Host.ReadText(1, f); if (b == null || b.Length == 0) b = Host.ReadText(0, f);
                if (b == null || b.Length == 0) return U.Cat("cat: ", f, ": No such file or directory");
                return b;
            }
            if (StartsWithStr(low, "echo ")) return U.Sub(c, 5, c.Length - 5);
            return null;
        }

        // ---- tabs ----------------------------------------------------
        void NewTab()
        {
            if (tabN >= TAB_MAX) return;
            tabs[tabN] = new Term(); tabN++; active = tabN - 1;
            RebuildHighlight();   // 高亮缓存是实例级的，切换后必须重算
        }
        void CloseTab()
        {
            if (tabN <= 1) { Shell.Close(this.id); return; }
            for (int i = active; i < tabN - 1; i++) tabs[i] = tabs[i + 1];
            tabN--; if (active >= tabN) active = tabN - 1;
            RebuildHighlight();
        }

        // =================================================================
        //  键盘
        // =================================================================
        public override void OnKey(int ch)
        {
            Term t = T();

            // ---- Diff 模式优先接管按键 ----
            if (isDiffMode)
            {
                if (ch == 'y' || ch == 'Y')
                {
                    // 接受修复：退出 Diff，命令排队到下一帧执行（不卡绘制）
                    pendingCmd = fixedCmd; pendingRun = true;
                    isDiffMode = false; originalCmd = ""; fixedCmd = ""; diffErrLine = "";
                    return;
                }
                if (ch == ESC)   // 忽略
                {
                    isDiffMode = false; originalCmd = ""; fixedCmd = ""; diffErrLine = "";
                    return;
                }
                return;   // Diff 面板期间吞掉其余按键
            }

            if (ch >= 32)
            {
                t.input = InsStr(t.input, t.caret, Host.CharStr((char)ch));
                t.caret++;
                t.ghost = ""; t.ghostFor = ""; t.ghostAskMs = Host.TickMs();  // 重新计时
                RebuildHighlight();
                return;
            }

            switch (ch)
            {
                case VK.Enter: Run(); return;
                case VK.Back:
                    if (t.caret > 0) { t.input = DelStr(t.input, t.caret - 1, 1); t.caret--; RebuildHighlight(); }
                    t.ghost = ""; t.ghostAskMs = Host.TickMs();
                    return;
                case VK.Delete:
                    if (t.caret < t.input.Length) { t.input = DelStr(t.input, t.caret, 1); RebuildHighlight(); }
                    t.ghost = ""; t.ghostAskMs = Host.TickMs();
                    return;
                case VK.Tab:
                    // 需求：Tab 拦截默认行为（防焦点跳出）—— 有预测则采纳，否则退回补全
                    if (AcceptGhost()) return;
                    Complete(t);
                    return;
                case VK.Esc:
                    if (t.ghost.Length > 0) { t.ghost = ""; return; }   // 先撤预测
                    return;
                case VK.Up:   HistUp(t);   return;
                case VK.Down: HistDown(t); return;
                case VK.Left:  if (t.caret > 0) t.caret--; return;
                case VK.Right: if (t.caret < t.input.Length) t.caret++; return;
                case VK.CtrlA: case VK.HomeK: t.caret = 0; return;
                case VK.CtrlE: case VK.EndK:  t.caret = t.input.Length; return;
                case VK.CtrlC:
                    Host.SetClipboard(t.input);
                    AppendLine(t, U.Cat(PS1(), "^C"));
                    t.input = ""; t.caret = 0; RebuildHighlight();
                    return;
                case VK.CtrlV: case VK.CsV:
                    {
                        string cb = Host.GetClipboard();
                        if (cb != null && cb.Length > 0)
                        {
                            t.input = InsStr(t.input, t.caret, cb);
                            t.caret += cb.Length; RebuildHighlight();
                        }
                    }
                    return;
                case VK.CtrlU:
                    t.input = U.Sub(t.input, t.caret, t.input.Length - t.caret);
                    t.caret = 0; RebuildHighlight(); return;
                case VK.CtrlK: t.input = U.Sub(t.input, 0, t.caret); RebuildHighlight(); return;
                case VK.CtrlL: ClearScreen(t); return;
                case VK.CtrlD: if (t.input.Length == 0) CloseTab(); return;
                case VK.CsT: NewTab(); return;
                case VK.CsW: CloseTab(); return;
                case VK.CtrlPlus:  Theme.TermCellH = ZoomStep(Theme.TermCellH, 1);  return;
                case VK.CtrlMinus: Theme.TermCellH = ZoomStep(Theme.TermCellH, -1); return;
                case VK.Ctrl0:     Theme.TermCellH = 18; return;
                default: return;
            }
        }

        void HistUp(Term t)
        {
            if (t.histN == 0) return;
            if (t.histPos > 0) t.histPos--;
            t.input = (t.histPos < t.histN) ? t.hist[t.histPos] : "";
            t.caret = t.input.Length; RebuildHighlight();
        }
        void HistDown(Term t)
        {
            if (t.histPos < t.histN) t.histPos++;
            t.input = (t.histPos >= t.histN) ? "" : t.hist[t.histPos];
            t.caret = t.input.Length; RebuildHighlight();
        }

        // Tab 补全（既有的命令名补全，保留）。候选表在 CMD_TABLE 里，
        // 按 "|" 分隔，零分配。
        void Complete(Term t)
        {
            string tok = t.input;
            int tn = (CMD_TABLE != null) ? CMD_TABLE.Length : 0;
            int m = 0; string first = "", all = "";
            int i = 0;
            while (i <= tn)
            {
                int j = i;
                while (j < tn && CMD_TABLE[j] != '|') j++;
                int wlen = j - i;
                if (wlen > 0 && StartsWithStr(tok, U.Sub(CMD_TABLE, i, wlen)))
                {
                    m++;
                    string w = U.Sub(CMD_TABLE, i, wlen);
                    if (m == 1) first = w;
                    all = U.Cat(all, w, " ");
                }
                i = j + 1;
            }
            if (m == 1) { t.input = first; t.caret = first.Length; RebuildHighlight(); }
            else if (m > 1 && all.Length > 0) AppendLine(t, all);
        }

        // =================================================================
        //  鼠标
        // =================================================================
        public override void OnMouseDown(int btn, int mx, int my)
        {
            if (isDiffMode) return;               // Diff 面板期间不接收点击
            if (my < TAB_H)                        // 切 tab
            {
                int tw = 140;
                for (int i = 0; i < tabN; i++)
                {
                    int tx = contentX + i * (tw + 4);
                    if (mx >= tx && mx < tx + tw)
                    {
                        if (active != i) { active = i; RebuildHighlight(); }
                        break;
                    }
                }
                return;
            }
            if (btn == 1)                          // 中键粘贴
            {
                Term t = T();
                string cb = Host.GetClipboard();
                if (cb != null && cb.Length > 0)
                {
                    t.input = InsStr(t.input, t.caret, cb);
                    t.caret += cb.Length; RebuildHighlight();
                }
            }
        }
        public override void OnWheel(int dy)
        {
            Term t = T();
            if (dy > 0) t.view++; else if (dy < 0) t.view--;
            if (t.view < 0) t.view = 0;
        }
        public override void OnClick(int mx, int my) { }

        // =================================================================
        //  绘制
        // =================================================================
        void ComputeLayout()
        {
            int W = Gfx.Width(), H = Gfx.Height();
            contentX = PAD;
            contentY = TAB_H + PAD;
            contentW = W - contentX * 2 - 8;
            contentH = H - contentY - PAD;

            int ch2 = Theme.TermCellH; if (ch2 < 12) ch2 = 12; if (ch2 > 28) ch2 = 28;
            cellH = ch2;
            int w0 = Gfx.Measure("0"); if (w0 < 4) w0 = 8; if (w0 > 20) w0 = 10;
            cellW = (w0 * ch2) / 18; if (cellW < 4) cellW = 4;

            // Diff 模式下把终端内容整体下移，给双栏面板让位。
            // 注意：contentH 必须同步扣掉这块高度，否则输入行会被挤出窗口。
            if (isDiffMode)
            {
                int dh = DiffH();
                contentY += dh + PAD;
                contentH -= dh + PAD;
            }

            // 底部提示条固定占 16px
            contentH -= 18;

            if (contentW < 40) contentW = 40;
            if (contentH < 40) contentH = 40;

            rows = contentH / cellH; cols = contentW / cellW;
            if (rows < 1) rows = 1; if (cols < 1) cols = 1;
        }
        int DiffH()
        {
            int h = Gfx.Height() / 2 - PAD;
            if (h < 80) h = 80;
            if (h > Gfx.Height() - 120) h = Gfx.Height() - 120;
            return h;
        }

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

        // 输出区：逐字符绘制。注意 contentY 已包含 Diff 面板让出的高度，
        // 所以这里必须以 contentY 为基线（不能从窗口顶部 0 开始）。
        void DrawScrollback(Term t)
        {
            int total = t.count;
            int first = total - rows - t.view;
            if (first < 0) first = 0;
            for (int vr = 0; vr < rows; vr++)
            {
                int abs = first + vr;
                if (abs < 0 || abs >= total) continue;
                int y = contentY + vr * cellH;
                // 输入行占最后一行（contentY + rows*cellH），输出区只能画到它上面
                if (y + cellH > contentY + rows * cellH) break;
                string s = RingAt(t, abs);
                if (s == null) continue;
                Gfx.Text(contentX, y, s, (uint)FG);
            }
        }

        // 输入行：按 inFg[] 分段着色 + 光标 + 幽灵文本
        void DrawInputLine(Term t)
        {
            // 输入行紧贴输出区最后一行之下；ComputeLayout 已为 Diff 面板让出高度
            int y = contentY + rows * cellH;
            int limit = Gfx.Height() - 20;              // 给底部提示条留位置
            if (y + cellH > limit) y = limit - cellH;
            if (y < contentY) y = contentY;

            string ps1 = PS1();
            Gfx.Text(contentX, y, ps1, 0x8AE234);

            int x0 = contentX + Gfx.Measure(ps1);
            string s = t.input;
            int n = s.Length;

            // ---- 分段绘制：相同颜色的连续片段合成一次 Gfx.Text ----
            int i = 0;
            while (i < n)
            {
                int col = (i < IN_CAP) ? inFg[i] : TK_DEF;
                int j = i + 1;
                while (j < n && ((j < IN_CAP ? inFg[j] : TK_DEF) == col)) j++;
                Gfx.Text(x0 + i * cellW, y, U.Sub(s, i, j - i), (uint)col);
                i = j;
            }

            // ---- 光标（块状，反色）----
            int cx = x0 + t.caret * cellW;
            if ((Host.Ticks() / 30) % 2 == 0)
            {
                Gfx.FillRect(cx, y, cellW, cellH, 0xE8E8E8);
                if (t.caret < n) Gfx.Text(cx, y, U.Sub(s, t.caret, 1), (uint)BG);
            }

            // ---- 幽灵文本：光标之后，偏暗色近似“半透明” ----
            // 显存没有 alpha 通道，用暗灰紫模拟 40% 透明度的观感。
            if (t.ghost != null && t.ghost.Length > 0 && t.caret == n)
            {
                Gfx.Text(cx + cellW, y, t.ghost, (uint)TK_GHOST);
            }
        }

        // ---- 3) Diff 双栏面板（终端上方）----
        void DrawDiff()
        {
            if (!isDiffMode) return;
            int x = contentX, y = TAB_H + PAD;
            int w = Gfx.Width() - contentX * 2 - 8;
            int h = DiffH();
            if (w < 120 || h < 60) return;

            Gfx.FillRound(x, y, w, h, 8, (uint)DF_BG);
            Gfx.DrawRound(x, y, w, h, 8, (uint)DF_BORDER);

            // 标题
            Gfx.Text(x + 12, y + 10,
                     "⚠ AI 修复建议 —— 按 Y 接受并重跑，Esc 忽略", (uint)DF_HEAD);
            Gfx.DrawLine(x + 12, y + 30, x + w - 12, y + 30, (uint)DF_BORDER);

            // 双栏
            int colW = (w - 36) / 2;
            int lx = x + 12, rx = x + 24 + colW;
            int cy = y + 40;

            Gfx.Text(lx, cy, "原命令 / 错误", (uint)0xAAAAAA);
            Gfx.Text(rx, cy, "修复后", (uint)0xAAAAAA);

            int ty = cy + 20;
            DrawWrapped(lx, ty, colW - 12, h - (ty - y) - 12, originalCmd, (uint)DF_DEL);
            DrawWrapped(rx, ty, colW - 12, h - (ty - y) - 12, fixedCmd, (uint)DF_ADD);
        }

        // 受限换行绘制：不分配数组，逐行用 U.Sub 切片。
        void DrawWrapped(int x, int y, int maxw, int maxh, string s, uint c)
        {
            if (s == null) return;
            int colsN = maxw / cellW; if (colsN < 4) colsN = 4;
            int maxLines = maxh / cellH; if (maxLines < 1) maxLines = 1;

            int i = 0, line = 0;
            while (i < s.Length && line < maxLines)
            {
                int take = s.Length - i; if (take > colsN) take = colsN;
                // 避免把 \n 画出来
                int real = 0;
                while (real < take && s[i + real] != '\n' && s[i + real] != '\r') real++;
                Gfx.Text(x, y + line * cellH, U.Sub(s, i, real), c);
                if (real < take) { i += real + 1; line++; }
                else { i += real; line++; }
            }
        }

        public override void OnPaint()
        {
            int W = Gfx.Width(), H = Gfx.Height();

            // 帧首：搬运后台 AI 结果 + 处理“接受修复”的排队重跑
            PollAI();
            GhostTick();
            if (pendingRun)
            {
                string c = pendingCmd;
                pendingRun = false; pendingCmd = "";
                AppendLine(T(), U.Cat(PS1(), c, "   (AI fix)"));
                string outp = RunBuiltin(T(), c);
                if (outp == null) outp = Host.Exec(c);
                if (outp != null && outp.Length > 0) AppendLine(T(), outp);
            }

            Term t = T();
            ComputeLayout();

            // 背景
            Gfx.FillRect(0, 0, W, H, (uint)BG);

            DrawTabBar();
            if (isDiffMode) DrawDiff();
            DrawScrollback(t);
            DrawInputLine(t);

            // 底部提示条
            Gfx.Text(contentX, H - 16,
                     "Tab 采纳预测 · Y 接受修复 · Esc 忽略 · ↑↓ 历史",
                     (uint)0x9A7AA0);
        }

        // =================================================================
        //  小工具（MiniCLR 无 BCL，全部手写）
        // =================================================================
        static string Trim(string s)
        {
            if (s == null) return "";
            int a = 0, b = s.Length;
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
            if (b < a) b = a;
            return U.Sub(s, a, b - a);
        }
        static string Lower(string s)
        {
            if (s == null) return "";
            // 单次分配：先复制到 char 数组再一次性建串不可行（无 StringBuilder），
            // 短命令用 Cat 循环可接受；这里限制在 256 字符内以防堆爆。
            int n = s.Length; if (n > 256) n = 256;
            string r = "";
            for (int i = 0; i < n; i++)
            {
                char c = s[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                r = U.Cat(r, Host.CharStr((int)c));
            }
            return r;
        }
        static string InsStr(string s, int pos, string ins)
        {
            return U.Cat(U.Sub(s, 0, pos), U.Cat(ins, U.Sub(s, pos, s.Length - pos)));
        }
        static string DelStr(string s, int pos, int len)
        {
            return U.Cat(U.Sub(s, 0, pos), U.Sub(s, pos + len, s.Length - pos - len));
        }
        static void Log(string s) { Host.Log(U.Cat("[term] ", s)); }
    }
}

// =====================================================================
//  集成说明
// ---------------------------------------------------------------------
//  1) 删除 csharp/apps/Shell/Apps.cs 中现有的
//         public class TerminalApp : App { ... }
//     整块（注意保留同文件里它前面的 Terminal 相关注释）。
//
//  2) 在三处 <ItemGroup> 里加入本文件（缺一不可，否则某个目标会找不到类型）：
//       csharp/apps/Shell/Shell.csproj
//         <Compile Include="TerminalApp.cs" />
//       csharp/apps/mex.shared.props
//         <Compile Include="$(MSBuildThisFileDirectory)../apps/Shell/TerminalApp.cs" />
//       csharp/winhost/WinHost.csproj
//         <Compile Include="../apps/Shell/TerminalApp.cs" />
//
//  3) 【重要】Shell.csproj（编译出 shell.mex 的那个目标）**没有**包含
//     NexOS.Core/AI.cs，因此该目标下不存在 NexOS.AI 类型。
//     本文件所有 AI 调用都包在 `#if WINHOST` 里，所以在 shell.mex 目标下
//     会自动降级为“无幽灵文本 / 无 Diff”（符合“网关离线静默降级”的约定）。
//     若希望内核侧也能用（同步、会阻塞一帧），需要：
//       a. 在 Shell.csproj 的 <ItemGroup> 加入
//            <Compile Include="../../NexOS.Core/AI.cs" />
//       b. 在 Shell.csproj 的 <PropertyGroup> 加入
//            <DefineConstants>$(DefineConstants);NEXOS_AI</DefineConstants>
//       c. 把本文件的 `#if WINHOST` 改成 `#if WINHOST || NEXOS_AI`，
//          并在 `#else` 分支里调用同步的
//            NexOS.AI.AI.Request(prompt, (int)NexOS.AI.AIMode.Fast)
//          （kernel 侧 ic_ai_request 已实现，见 nexos_ai.h）。
//
//  4) 关于 TBox：TerminalApp 的输入行**不使用** TBox（它有自己的
//     Term.input/Term.caret 模型，走 InsStr/DelStr）。因此幽灵文本状态
//     放在 Term 上。若要让共用 TBox 的其它输入框（Notepad / AiAgent）
//     也支持幽灵文本，在 Forms.cs 的 TBox 里补三行即可：
//         public string ghost = "";
//         public string ghostFor = "";
//         public int    ghostAskMs = 0;
//     绘制端按 tBox.ghost/ghostFor 判断是否画灰字即可，逻辑与本文件一致。
// =====================================================================
