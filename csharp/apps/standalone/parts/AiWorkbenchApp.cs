// =====================================================================
//  AiWorkbenchApp.cs  -  AI 工作台 (AI Workbench), standalone .mex
// ---------------------------------------------------------------------
//  Port of the "NexOS AI Desktop" Python pywebview app (E:\cache\NexOSAI)
//  onto the NexOS standalone-application contract: one .mex image, its
//  own MiniCLR slot, no Python / Ollama / SQLite.
//
//  What the original offered, and how it maps here:
//    * multi-session chat       -> 4 sessions in MKFS aiws1..4.txt
//    * workspace file tools     -> ws_* files on MKFS (write/read/list)
//    * local document search    -> [SEARCH] scans the workspace + transcript
//    * web search               -> best-effort plain-HTTP fetch (no TLS)
//    * Markdown/PPTX documents  -> Markdown docs only (no PPTX/PDF writer)
//    * agent tool loop          -> text directives + 3 auto rounds
//
//  The model itself is the kernel's native GGUF engine, driven exactly as
//  AiChat does: Host.AiStart() arms a cooperative decode job and returns,
//  OnPaint() -> Pump() streams the text back.  There is no model selector
//  and no Ollama because NexOS loads a single embedded model.
//
//  MiniCLR rules (see Forms.cs): NO System.* BCL, no generics/collections,
//  no try/catch, no static field initialisers, no float, no `string +`.
//  Strings are UTF-8 byte strings: Length/indexer/Concat(U.Cat) only.
//  Char literals with an escape (e.g. '\n') are mangled by the compiler,
//  so newline/CR/tab are written as numeric codes.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class AiWorkbenchApp : App
    {
        // ---- live state (explicit ctor: no static initialisers) ------
        TBox    t;          // prompt editor
        string  history;    // visible transcript of the active session
        string  histBase;   // transcript prefix up to the streaming line
        string  reply;      // text streamed for the current answer
        string  status;     // one-line status / tool summary
        string  lastQ;      // last user question (tool follow-up context)
        int     busy;       // 1 = a decode job is running
        int     editMode;   // 1 = the prompt box has focus
        int     session;    // active session index 0..3
        int     searchOn;   // 1 = try the web for [WEB]
        int     autoTools;  // 1 = execute model tool directives
        int     round;      // tool rounds used for the current question

        // per-session cache (mirrors MKFS, survives a session switch)
        string[] sTitle;
        string[] sBody;
        int[]    sUsed;     // 1 = user has written in this session

        // directive parser scratch (no out/ref params under MiniCLR)
        int     dKind;      // 0 none / 1 shell / 2 write / 3 read / 4 doc
                            // 5 ls / 6 search / 7 web
        string  dArg;

        const int NSESS = 4;
        const int HIST_MAX = 3200;   // bytes kept in RAM (bump heap is 512 KB)
        const int DRAW_COLS = 8;     // px per glyph used by the wrap helper

        public AiWorkbenchApp()
        {
            t         = new TBox();
            sTitle    = new string[NSESS];
            sBody     = new string[NSESS];
            sUsed     = new int[NSESS];
            history   = "";
            histBase  = "";
            reply     = "";
            status    = "就绪";
            lastQ     = "";
            busy      = 0;
            editMode  = 1;
            session   = 0;
            searchOn  = 0;
            autoTools = 1;
            round     = 0;
            dKind     = 0;
            dArg      = "";

            LoadAll();
            history = sBody[0];
            if (history.Length == 0) history = Welcome();
        }

        public override string GetTitle() { return "AI 工作台"; }

        // -------------------------------------------------------------
        //  Layout helpers -- Paint and Click must agree on every rect.
        // -------------------------------------------------------------
        int Pad()     { return 14; }
        int SideW()   { return 158; }
        int MainX()   { return Pad() + SideW() + 12; }
        int MainW()   { int w = Gfx.Width() - MainX() - Pad(); if (w < 220) w = 220; return w; }
        int InH()     { return 34; }
        int InY()     { return Gfx.Height() - Pad() - InH(); }
        int InW()     { int w = MainW() - 84 - 8; if (w < 80) w = 80; return w; }
        int BodyY()   { return Pad() + 78; }
        int BodyH()   { int h = InY() - BodyY() - 10; if (h < 0) h = 0; return h; }
        int SessY(int i) { return Pad() + 38 + i * 32; }
        int NewY()    { return Gfx.Height() - Pad() - 72; }
        int ClrY()    { return Gfx.Height() - Pad() - 38; }
        int TogY()    { return Pad() + 50; }
        int Tog2X()   { return MainX() + 104; }
        int HelpX()   { return MainX() + MainW() - 64; }

        // -------------------------------------------------------------
        //  Small string helpers.  Every one is UTF-8 byte aware: a cut
        //  is only made on a lead byte, never inside a multi-byte glyph.
        // -------------------------------------------------------------
        static bool IsSpace(int c)
        {
            return c == 32 || c == 9 || c == 10 || c == 13;
        }

        static string Trim(string s)
        {
            if (s == null) return "";
            int a = 0, b = s.Length;
            while (a < b && IsSpace((int)s[a])) a++;
            while (b > a && IsSpace((int)s[b - 1])) b--;
            return U.Sub(s, a, b - a);
        }

        // First n characters (not bytes).
        static string Clip(string s, int n)
        {
            if (s == null) return "";
            int cut = 0, seen = 0;
            while (cut < s.Length)
            {
                if (((int)s[cut] & 0xC0) != 0x80)
                {
                    if (seen >= n) break;
                    seen++;
                }
                cut++;
            }
            if (cut >= s.Length) return s;
            return U.Sub(s, 0, cut);
        }

        // Last maxChars characters.
        static string TailChars(string s, int maxChars)
        {
            if (s == null) return "";
            int total = 0;
            for (int i = 0; i < s.Length; i++)
                if (((int)s[i] & 0xC0) != 0x80) total++;
            if (total <= maxChars) return s;
            int skip = total - maxChars, cut = 0, seen = 0;
            while (cut < s.Length)
            {
                cut++;
                if (((int)s[cut - 1] & 0xC0) != 0x80) seen++;
                if (seen >= skip) break;
            }
            while (cut < s.Length && ((int)s[cut] & 0xC0) == 0x80) cut++;
            return U.Sub(s, cut, s.Length - cut);
        }

        // Case-insensitive compare against a lower-case ASCII literal.
        static bool EqNoCase(string s, string low)
        {
            if (s == null) return false;
            if (s.Length != low.Length) return false;
            for (int i = 0; i < s.Length; i++)
            {
                int c = (int)s[i];
                if (c >= 65 && c <= 90) c += 32;
                if (c != (int)low[i]) return false;
            }
            return true;
        }

        // Case-insensitive ASCII substring search (CJK bytes compare raw).
        static int FindNoCase(string s, string q)
        {
            if (s == null || q == null) return -1;
            int n = s.Length, m = q.Length;
            if (m == 0 || m > n) return -1;
            for (int i = 0; i + m <= n; i++)
            {
                int j = 0;
                while (j < m)
                {
                    int a = (int)s[i + j], b = (int)q[j];
                    if (a >= 65 && a <= 90) a += 32;
                    if (b >= 65 && b <= 90) b += 32;
                    if (a != b) break;
                    j++;
                }
                if (j == m) return i;
            }
            return -1;
        }

        // One allocation per line: prefer U.Sub over a Cat() accumulator,
        // which is O(n^2) bytes on the bump heap.
        static void Wrap(int x, int y, int maxw, int maxLines, string s, uint c)
        {
            if (s == null) return;
            int cols = maxw / DRAW_COLS - 1;
            if (cols < 8) cols = 8;
            int i = 0, line = 0;
            while (i < s.Length && line < maxLines)
            {
                int ch = (int)s[i];
                if (ch == 10 || ch == 13) { i++; continue; }
                int start = i, n = 0;
                while (i < s.Length && n < cols)
                {
                    int cc = (int)s[i];
                    if (cc == 10 || cc == 13) break;
                    i++; n++;
                }
                Gfx.Text(x, y + line * 18, U.Sub(s, start, i - start), c);
                line++;
            }
        }

        // Wrap the TAIL of a transcript: the newest lines stay visible.
        static void WrapTail(int x, int y, int maxw, int maxLines, string s, uint c)
        {
            int cols = maxw / DRAW_COLS - 1;
            if (cols < 8) cols = 8;
            Wrap(x, y, maxw, maxLines, TailChars(s, cols * (maxLines + 1)), c);
        }

        // -------------------------------------------------------------
        //  Session storage.  One MKFS file per slot:
        //      aiws1.txt .. aiws4.txt
        //      line 1 = title, the rest = transcript
        //  MKFS names are capped at 19 bytes, so "aiws4.txt" is safe.
        // -------------------------------------------------------------
        string SessFile(int i) { return U.Cat("aiws", U.I(i + 1), ".txt"); }
        string DefTitle(int i) { return U.Cat("会话 ", U.I(i + 1)); }

        // The 64-bit MKFS keeps ONE global cwd.  Any app can move it, so
        // every workspace access starts by pinning it back to the root.
        void FsRoot() { Host.EnterPath(0, ""); }

        void LoadAll()
        {
            for (int i = 0; i < NSESS; i++)
            {
                sTitle[i] = DefTitle(i);
                sBody[i]  = "";
                sUsed[i]  = 0;
            }
            FsRoot();
            for (int i = 0; i < NSESS; i++)
            {
                string f = SessFile(i);
                if (Host.FileSize(0, f) <= 0) continue;
                string body = Host.ReadText(0, f);
                if (body == null) body = "";
                int nl = 0;
                while (nl < body.Length && (int)body[nl] != 10) nl++;
                if (nl < body.Length)
                {
                    string title = Trim(U.Sub(body, 0, nl));
                    if (title.Length > 0) sTitle[i] = Clip(U.Flat(title), 9);
                    sBody[i] = U.Sub(body, nl + 1, body.Length - nl - 1);
                }
                else sBody[i] = body;
                if (sBody[i] == null) sBody[i] = "";
                sUsed[i] = sBody[i].Length > 0 ? 1 : 0;
            }
        }

        void SaveSession()
        {
            if (session < 0 || session >= NSESS) return;
            sBody[session] = history;
            FsRoot();
            Host.WriteText(0, SessFile(session),
                           U.Cat(sTitle[session], "\n", history));
        }

        void Switch(int i)
        {
            if (i == session) { editMode = 1; return; }
            if (busy == 1) { Host.AiCancel(); busy = 0; Host.SetAnim(0); }
            SaveSession();
            session  = i;
            history  = sBody[i];
            if (history == null) history = "";
            if (history.Length == 0) history = Welcome();
            histBase = "";
            reply    = "";
            round    = 0;
            status   = U.Cat("已切换到会话 ", U.I(i + 1));
            editMode = 1;
        }

        void NewSession()
        {
            if (busy == 1) { Host.AiCancel(); busy = 0; Host.SetAnim(0); }
            SaveSession();
            sTitle[session] = DefTitle(session);
            sUsed[session]  = 0;
            history         = Welcome();
            sBody[session]  = history;
            histBase        = "";
            reply           = "";
            round           = 0;
            status          = "新会话已就绪";
            editMode        = 1;
            FsRoot();
            Host.FileDelete(0, SessFile(session));
        }

        void ClearBody()
        {
            if (busy == 1) { Host.AiCancel(); busy = 0; Host.SetAnim(0); }
            history = Welcome();
            sBody[session] = history;
            histBase = "";
            reply    = "";
            round    = 0;
            status   = "已清空当前会话";
            editMode = 1;
            SaveSession();
        }

        // Append one transcript turn, keeping the buffer bounded.  Cuts on
        // a UTF-8 boundary so a CJK glyph is never split.
        void AppendTurn(string who, string text)
        {
            if (text == null) text = "";
            history = U.Cat(history, "\n", who, ": ", text);
            if (history.Length > HIST_MAX)
            {
                int cut = history.Length - HIST_MAX;
                while (cut < history.Length && ((int)history[cut] & 0xC0) == 0x80) cut++;
                history = U.Cat("...", U.Sub(history, cut, history.Length - cut));
            }
        }

        static string Welcome()
        {
            return U.Cat("AI 工作台已就绪。\n",
                         "本地 GGUF 模型推理，异步流式输出。\n",
                         "输入问题后回车或点「发送」；\n",
                         "以 / 开头的命令直接执行，输入 /help 查看全部。\n",
                         "工具：/ls /read /write /doc /search /web /sh");
        }

        static string HelpText()
        {
            return U.Cat("工具命令：\n",
                         "/ls                    列出工作区文件\n",
                         "/read ws_a.txt         读取工作区文件\n",
                         "/write ws_a.txt|内容   写入工作区文件\n",
                         "/doc 标题|正文         生成 Markdown 文档\n",
                         "/search 关键词         检索工作区与对话\n",
                         U.Cat("/web 关键词            联网检索（尽力而为）\n",
                               "/sh ls                 只读 shell 命令\n",
                               "/new 新会话   /clear 清空当前\n\n",
                               "模型回答末尾可单独一行输出\n",
                               "[SHELL]/[READ]/[WRITE]/[DOC]/[LS]/\n",
                               "[SEARCH]/[WEB] 指令，工作台会自动执行\n",
                               "并把结果回喂给模型（最多 3 轮）。"));
        }

        // -------------------------------------------------------------
        //  Native model call.  Build a short prompt: the engine caps the
        //  prompt at 4 KB and this app's decode budget is 96 tokens.
        // -------------------------------------------------------------
        string BuildPrompt(string q)
        {
            string p = U.Cat("你是 NexOS AI 工作台的本地助手，用简体中文简短回答。\n",
                             "可在最后单独一行调用一个工具：\n",
                             "[SHELL] ls  [LS]  [READ] ws_a.txt  [WRITE] ws_a.txt|内容\n",
                             "[DOC] 标题|正文  [SEARCH] 词  [WEB] 词\n",
                             "工作区：", WsSummary(), "\n");
            if (history.Length > 0)
                p = U.Cat(p, "最近对话：\n", TailChars(history, 360), "\n");
            p = U.Cat(p, "用户：", Clip(q, 600));
            if (p.Length > 1800) p = U.Sub(p, p.Length - 1800, 1800);
            return p;
        }

        void Send()
        {
            string q = Trim(t.text);
            if (q.Length == 0) return;
            t.text = "";
            t.cursor = 0;
            editMode = 1;

            if ((int)q[0] == 47) { RunSlash(q); return; }   // '/'
            if (busy == 1) return;

            lastQ = q;
            round = 0;
            if (sUsed[session] == 0)
            {
                sTitle[session] = Clip(U.Flat(q), 9);
                sUsed[session]  = 1;
            }
            AppendTurn("你", q);
            histBase = U.Cat(history, "\nAI: ");
            reply    = "";

            int rc = Host.AiStart(BuildPrompt(q));
            if (rc != 0)
            {
                history = U.Cat(histBase, "(推理引擎未就绪：模型未加载，详见串口日志)");
                status  = "引擎未就绪";
                SaveSession();
                return;
            }
            busy   = 1;
            status = "推理中…（可点停止）";
            Host.SetAnim(1);
        }

        // Called once per paint: pull the delta text and rebuild the live line.
        void Pump()
        {
            if (busy != 1) return;
            string chunk = Host.AiPoll();
            if (chunk != null && chunk.Length > 0)
            {
                reply = U.Cat(reply, chunk);
                if (reply.Length > 1600)
                {
                    int cut = reply.Length - 1600;
                    while (cut < reply.Length && ((int)reply[cut] & 0xC0) == 0x80) cut++;
                    reply = U.Sub(reply, cut, reply.Length - cut);
                }
                history = U.Cat(histBase, reply);
            }
            int st = Host.AiStatus();
            if (st == 2 || st == 3 || st == 0)
            {
                if (reply.Length == 0)
                    history = U.Cat(histBase, st == 3 ? "(推理出错，详见串口日志)"
                                                     : "(引擎无输出：模型未加载或回答为空)");
                busy = 0;
                Host.SetAnim(0);
                status = "就绪";
                SaveSession();
                if (st == 2 && reply.Length > 0) MaybeRunTool();
            }
            else Host.SetAnim(1);
        }

        // -------------------------------------------------------------
        //  Tool directives.  The model emits ONE line like "[READ] ws_x",
        //  we execute it, append the result and feed it back once per
        //  round (max 3) so the model can summarise.
        // -------------------------------------------------------------
        static bool TagIs(string t, string up)
        {
            if (t == null) return false;
            if (t.Length != up.Length) return false;
            for (int i = 0; i < t.Length; i++)
            {
                int c = (int)t[i];
                if (c >= 97 && c <= 122) c -= 32;
                if (c != (int)up[i]) return false;
            }
            return true;
        }

        static int TagKind(string t)
        {
            if (TagIs(t, "SHELL"))  return 1;
            if (TagIs(t, "WRITE"))  return 2;
            if (TagIs(t, "READ"))   return 3;
            if (TagIs(t, "DOC"))    return 4;
            if (TagIs(t, "LS"))     return 5;
            if (TagIs(t, "SEARCH")) return 6;
            if (TagIs(t, "WEB"))    return 7;
            return 0;
        }

        static string TagName(int k)
        {
            if (k == 1) return "SHELL";
            if (k == 2) return "WRITE";
            if (k == 3) return "READ";
            if (k == 4) return "DOC";
            if (k == 5) return "LS";
            if (k == 6) return "SEARCH";
            if (k == 7) return "WEB";
            return "?";
        }

        // Scans every line, keeping the LAST valid directive.
        void ScanDirective(string s)
        {
            dKind = 0;
            dArg  = "";
            if (s == null) return;
            int n = s.Length, i = 0;
            while (i < n)
            {
                int ls = i;
                while (i < n && (int)s[i] != 10) i++;
                int le = i;
                if (i < n) i++;
                if (le <= ls || (int)s[ls] != 91) continue;   // '['
                int rb = ls;
                while (rb < le && (int)s[rb] != 93) rb++;     // ']'
                if (rb >= le) continue;
                string tag = U.Sub(s, ls + 1, rb - ls - 1);
                string arg = Trim(U.Sub(s, rb + 1, le - rb - 1));
                int k = TagKind(tag);
                if (k != 0) { dKind = k; dArg = arg; }
            }
        }

        string ExecTool()
        {
            if (dKind == 1) return SafeCmd(dArg);
            if (dKind == 2) return WsWrite(dArg);
            if (dKind == 3) return WsRead(dArg);
            if (dKind == 4) return WsDoc(dArg);
            if (dKind == 5) return ListWs();
            if (dKind == 6) return LocalSearch(dArg);
            if (dKind == 7) return WebSearch(dArg);
            return "";
        }

        void MaybeRunTool()
        {
            if (autoTools == 0) return;
            if (round >= 3) { status = "工具轮次已达上限"; return; }
            ScanDirective(reply);
            if (dKind == 0) return;

            string res = ExecTool();
            round++;
            AppendTurn("工具", U.Cat(TagName(dKind), " ", dArg, "\n", res));
            status = U.Cat("已执行工具 ", TagName(dKind));
            SaveSession();
            if (round >= 3) return;

            // Feed the result back for a final, tool-free answer.
            histBase = U.Cat(history, "\nAI: ");
            reply    = "";
            string p = U.Cat("用户问题：", Clip(lastQ, 400),
                             "\n工具结果：\n", TailChars(res, 600),
                             "\n请根据工具结果用中文简短回答，不要再输出工具标记。");
            int rc = Host.AiStart(p);
            if (rc != 0) { status = "工具结果已附上（引擎未就绪）"; return; }
            busy   = 1;
            status = "根据工具结果继续推理…";
            Host.SetAnim(1);
        }

        // -------------------------------------------------------------
        //  Workspace files (MKFS root, "ws_" prefix, <= 19 bytes).
        // -------------------------------------------------------------
        static bool IsWsName(string n)
        {
            if (n == null) return false;
            if (n.Length < 4 || n.Length > 19) return false;
            if ((int)n[0] != 119) return false;   // w
            if ((int)n[1] != 115) return false;   // s
            if ((int)n[2] != 95)  return false;   // _
            for (int i = 3; i < n.Length; i++)
            {
                int c = (int)n[i];
                if (c == 47 || c == 92 || c == 124 || c == 58) return false;
            }
            return true;
        }

        static int BarAt(string a)
        {
            if (a == null) return -1;
            for (int i = 0; i < a.Length; i++) if ((int)a[i] == 124) return i;
            return -1;
        }

        string WsWrite(string a)
        {
            int bar = BarAt(a);
            if (bar < 0) return "写入失败：格式应为 文件名|内容";
            string n = Trim(U.Sub(a, 0, bar));
            string b = U.Sub(a, bar + 1, a.Length - bar - 1);
            if (!IsWsName(n)) return "写入失败：文件名必须以 ws_ 开头且不超过 19 字节";
            FsRoot();
            int rc = Host.WriteText(0, n, b);
            if (rc < 0) return "写入失败（介质只读或空间不足）";
            return U.Cat("已写入 ", n, "（", U.I(b.Length), " 字节）");
        }

        string WsRead(string a)
        {
            string n = Trim(U.Flat(a));
            if (!IsWsName(n)) return "读取失败：只允许读取 ws_ 开头的文件";
            FsRoot();
            if (Host.FileSize(0, n) <= 0) return U.Cat("文件不存在：", n);
            string b = Host.ReadText(0, n);
            if (b == null) b = "";
            return U.Cat("— ", n, " —\n", b);
        }

        string WsDoc(string a)
        {
            int bar = BarAt(a);
            string title = bar < 0 ? Trim(U.Flat(a)) : Trim(U.Sub(a, 0, bar));
            string body  = bar < 0 ? "" : U.Sub(a, bar + 1, a.Length - bar - 1);
            if (title.Length == 0) title = "未命名文档";
            string name = FreeDocName();
            if (name.Length == 0) return "文档创建失败：工作区文件太多";
            FsRoot();
            string text = U.Cat("# ", title, "\n\n", body,
                                "\n\n---\n> 由 NexOS AI 工作台生成\n");
            int rc = Host.WriteText(0, name, text);
            if (rc < 0) return "文档创建失败（介质只读或空间不足）";
            return U.Cat("已生成文档 ", name, "（Markdown）\n标题：", Clip(title, 40));
        }

        string FreeDocName()
        {
            FsRoot();
            for (int i = 1; i <= 9; i++)
            {
                string n = U.Cat("ws_doc", U.I(i), ".md");
                if (Host.FileSize(0, n) <= 0) return n;
            }
            return "";
        }

        string ListWs()
        {
            FsRoot();
            int n = Host.FileCount(0);
            string r = "— 工作区 —\n";
            int c = 0;
            for (int i = 0; i < n && c < 20; i++)
            {
                string nm = Host.FileName(0, i);
                if (nm == null || !IsWsName(nm)) continue;
                int sz = Host.FileSize(0, nm);
                r = U.Cat(r, nm, "  ", U.I(sz), "B\n");
                c++;
            }
            if (c == 0) r = U.Cat(r, "(空) 用 /write ws_a.txt|内容 新建\n");
            return r;
        }

        string WsSummary()
        {
            FsRoot();
            int n = Host.FileCount(0);
            string r = "";
            int c = 0;
            for (int i = 0; i < n && c < 6; i++)
            {
                string nm = Host.FileName(0, i);
                if (nm == null || !IsWsName(nm)) continue;
                if (r.Length > 0) r = U.Cat(r, ", ");
                r = U.Cat(r, nm);
                c++;
            }
            if (r.Length == 0) r = "(空)";
            return r;
        }

        // -------------------------------------------------------------
        //  Local search: workspace files + the active transcript.
        // -------------------------------------------------------------
        static string LineAt(string s, int pos)
        {
            if (s == null) return "";
            int a = pos;
            while (a > 0 && (int)s[a - 1] != 10) a--;
            int b = pos;
            while (b < s.Length && (int)s[b] != 10) b++;
            return Clip(Trim(U.Sub(s, a, b - a)), 60);
        }

        string LocalSearch(string q)
        {
            q = Trim(q);
            if (q.Length == 0) return "用法：/search 关键词";
            string r = U.Cat("— 检索「", Clip(q, 30), "」—\n");
            int hits = 0;
            FsRoot();
            int n = Host.FileCount(0);
            for (int i = 0; i < n && hits < 6; i++)
            {
                string nm = Host.FileName(0, i);
                if (nm == null || !IsWsName(nm)) continue;
                string body = Host.ReadText(0, nm);
                if (body == null) continue;
                int p = FindNoCase(body, q);
                if (p < 0) continue;
                r = U.Cat(r, nm, ": ", LineAt(body, p), "\n");
                hits++;
            }
            int tp = FindNoCase(history, q);
            if (tp >= 0 && hits < 6)
            {
                r = U.Cat(r, "会话: ", LineAt(history, tp), "\n");
                hits++;
            }
            if (hits == 0) r = U.Cat(r, "未找到匹配内容。\n");
            return r;
        }

        // -------------------------------------------------------------
        //  Best-effort web search.  NexOS has no TLS, so only plain
        //  HTTP endpoints can answer; every failure is reported honestly
        //  instead of inventing results.
        // -------------------------------------------------------------
        static string Hex2(int v)
        {
            const string H = "0123456789ABCDEF";
            return U.Cat(U.Sub(H, (v >> 4) & 15, 1), U.Sub(H, v & 15, 1));
        }

        static string UrlArg(string s)
        {
            if (s == null) return "";
            string r = "";
            for (int i = 0; i < s.Length; i++)
            {
                int c = (int)s[i];
                if ((c >= 97 && c <= 122) || (c >= 65 && c <= 90) ||
                    (c >= 48 && c <= 57) || c == 46 || c == 45 || c == 95)
                    r = U.Cat(r, Host.CharStr(c));
                else if (c == 32) r = U.Cat(r, "+");
                else r = U.Cat(r, "%", Hex2(c));
            }
            return r;
        }

        // Strip HTML into a short plain-text excerpt.  One U.Sub per text
        // run instead of per character: a 16 KB page would otherwise churn
        // hundreds of KB through the bump heap.
        static string StripHtml(string s)
        {
            string r = "";
            int i = 0, n = s.Length, runs = 0;
            int skip = 0;
            while (i < n && r.Length < 900 && runs < 24)
            {
                if ((int)s[i] == 60)   // '<'
                {
                    int ts = i + 1;
                    while (i < n && (int)s[i] != 62) i++;
                    int te = i;
                    if (i < n) i++;
                    string tag = U.Sub(s, ts, te - ts);
                    if (TagIs(tag, "script") || TagIs(tag, "style") ||
                        TagIs(tag, "/script") || TagIs(tag, "/style"))
                        skip = 1;
                    else if (skip == 1) skip = 0;
                    continue;
                }
                int start = i;
                while (i < n && (int)s[i] != 60) i++;
                if (skip == 1) continue;
                string part = Trim(U.Sub(s, start, i - start));
                if (part.Length == 0) continue;
                r = U.Cat(r, part, " ");
                runs++;
            }
            if (r.Length > 900) r = U.Sub(r, 0, 900);
            return Trim(r);
        }

        string WebSearch(string q)
        {
            q = Trim(q);
            if (q.Length == 0) return "用法：/web 关键词";
            if (searchOn == 0) return "联网开关关闭：请先打开标题栏下的「联网」。";
            string u = U.Cat("http://html.duckduckgo.com/html/?q=", UrlArg(q));
            string raw = Host.HttpGet(u);
            if (raw == null) raw = "";
            if (raw.Length < 64)
                return "联网检索不可用（NexOS 无 TLS，网络可能离线）。";
            string text = StripHtml(raw);
            if (text.Length == 0)
                return "联网检索不可用（页面无可读文本）。";
            return U.Cat("— 网页摘要 —\n", Clip(text, 700));
        }

        // -------------------------------------------------------------
        //  Read-only shell.  The Python app exposed a full shell to the
        //  model; that is unsafe, so only inspection verbs pass.
        // -------------------------------------------------------------
        static bool CmdAllowed(string verb)
        {
            return EqNoCase(verb, "ls")      || EqNoCase(verb, "dir")    ||
                   EqNoCase(verb, "pwd")     || EqNoCase(verb, "echo")   ||
                   EqNoCase(verb, "help")    || EqNoCase(verb, "about")  ||
                   EqNoCase(verb, "meminfo") || EqNoCase(verb, "ps")     ||
                   EqNoCase(verb, "netinfo") || EqNoCase(verb, "netstat")||
                   EqNoCase(verb, "ver")     || EqNoCase(verb, "date")   ||
                   EqNoCase(verb, "time")    || EqNoCase(verb, "whoami") ||
                   EqNoCase(verb, "users")   || EqNoCase(verb, "history");
        }

        string SafeCmd(string cmd)
        {
            cmd = Trim(U.Flat(cmd));
            if (cmd.Length == 0) return "用法：/sh <只读命令>";
            int i = 0;
            while (i < cmd.Length && (int)cmd[i] != 32) i++;
            string verb = U.Sub(cmd, 0, i);
            string arg  = Trim(U.Sub(cmd, i, cmd.Length - i));

            // cat/type only ever reach the workspace, never SFS internals.
            if (EqNoCase(verb, "cat") || EqNoCase(verb, "type"))
                return WsRead(arg);
            if (!CmdAllowed(verb)) return "该命令不在只读白名单内。";
            string r = Host.Exec(cmd);
            if (r == null) r = "";
            return U.Cat("$ ", cmd, "\n", r);
        }

        // -------------------------------------------------------------
        //  Slash commands bypass the model: they are deterministic and
        //  work even when no GGUF model is loaded.
        // -------------------------------------------------------------
        void RunSlash(string q)
        {
            int i = 1;
            while (i < q.Length && (int)q[i] != 32) i++;
            string cmd = U.Sub(q, 1, i - 1);
            while (i < q.Length && (int)q[i] == 32) i++;
            string arg = U.Sub(q, i, q.Length - i);

            if (EqNoCase(cmd, "help"))   { AppendTurn("工具", HelpText());     status = "帮助"; return; }
            if (EqNoCase(cmd, "clear"))  { ClearBody(); return; }
            if (EqNoCase(cmd, "new"))    { NewSession(); return; }
            if (EqNoCase(cmd, "ls"))     { AppendTurn("工具", ListWs());       status = "工作区"; return; }
            if (EqNoCase(cmd, "read"))   { AppendTurn("工具", WsRead(arg));    status = "读取"; return; }
            if (EqNoCase(cmd, "write"))  { AppendTurn("工具", WsWrite(arg));   status = "写入"; SaveSession(); return; }
            if (EqNoCase(cmd, "doc"))    { AppendTurn("工具", WsDoc(arg));     status = "生成文档"; SaveSession(); return; }
            if (EqNoCase(cmd, "search")) { AppendTurn("工具", LocalSearch(arg)); status = "检索"; return; }
            if (EqNoCase(cmd, "web"))    { AppendTurn("工具", WebSearch(arg)); status = "联网检索"; return; }
            if (EqNoCase(cmd, "sh"))     { AppendTurn("工具", SafeCmd(arg));   status = "shell"; return; }
            if (EqNoCase(cmd, "stop"))   { if (busy == 1) { Host.AiCancel(); busy = 0; Host.SetAnim(0); } status = "已停止"; return; }

            AppendTurn("工具", U.Cat("未知命令：/", cmd, "。输入 /help 查看。"));
            status = "未知命令";
        }

        // -------------------------------------------------------------
        //  Paint
        // -------------------------------------------------------------
        public override void OnPaint()
        {
            Pump();
            W.Clear();

            int pad = Pad(), h = Gfx.Height();
            int sw = SideW(), sx = pad;

            // ---- session sidebar ------------------------------------
            W.Card(sx, pad, sw, h - 2 * pad);
            Gfx.Text(sx + 12, pad + 12, "会话", C.TextSub);
            Gfx.Text(sx + sw - 34, pad + 12, "4 槽", C.TextFaint);

            for (int i = 0; i < NSESS; i++)
            {
                int y = SessY(i);
                W.Row(sx + 8, y, sw - 16, Clip(sTitle[i], 9), i == session);
            }

            Gfx.Text(sx + 12, SessY(NSESS) + 2, "自动保存到 MKFS", C.TextFaint);

            W.Button(sx + 8, NewY(), sw - 16, 28, "新建会话");
            W.Button(sx + 8, ClrY(), sw - 16, 28, "清空当前");

            // ---- header + toggles -----------------------------------
            int mx = MainX(), mw = MainW();
            W.Header(mx, pad, "AI 工作台");
            Gfx.Text(mx, pad + 24,
                     U.Cat("本地 GGUF · 会话 ", U.I(session + 1), "/", U.I(NSESS),
                           " · ", status),
                     C.TextSub);

            int ty = TogY();
            W.Toggle(mx, ty, 40, 18, searchOn, 101);
            Gfx.Text(mx + 48, ty + 1, "联网", C.Text);
            W.Toggle(Tog2X(), ty, 40, 18, autoTools, 102);
            Gfx.Text(Tog2X() + 48, ty + 1, "自动工具", C.Text);
            W.Button(HelpX(), ty - 3, 64, 24, "帮助");

            // ---- transcript -----------------------------------------
            int by = BodyY(), bh = BodyH();
            if (bh > 20)
            {
                W.Card(mx, by, mw, bh);
                int lines = (bh - 16) / 18;
                if (lines < 1) lines = 1;
                WrapTail(mx + 12, by + 8, mw - 24, lines, history, C.Text);
            }

            // ---- prompt box + send ----------------------------------
            int iy = InY(), ih = InH(), iw = InW();
            Gfx.FillRound(mx, iy, iw, ih, 8, C.White);
            Gfx.DrawRound(mx, iy, iw, ih, 8, busy == 1 ? C.Warn : C.Accent);

            string shown = t.text;
            if (shown.Length == 0) shown = "输入问题，Enter 发送；/help 查看命令";
            Gfx.Text(mx + 10, iy + 9, shown, editMode == 1 ? C.Text : C.TextSub);

            if (editMode == 1 && (Host.Ticks() / 30) % 2 == 0)
            {
                string before = "";
                for (int i = 0; i < t.cursor && i < t.text.Length; i++)
                    before = U.Cat(before, Host.CharStr((int)t.text[i]));
                int cx = mx + 10 + Gfx.Measure(before);
                Gfx.FillRect(cx, iy + 9, 2, 18, C.Text);
            }

            if (busy == 1) W.Primary(mx + iw + 8, iy, 84, ih, "停止");
            else           W.Primary(mx + iw + 8, iy, 84, ih, "发送");
        }

        // -------------------------------------------------------------
        //  Input
        // -------------------------------------------------------------
        public override void OnClick(int mx, int my)
        {
            int pad = Pad(), sw = SideW();

            for (int i = 0; i < NSESS; i++)
                if (U.In(mx, my, pad + 8, SessY(i), sw - 16, 30)) { Switch(i); return; }
            if (U.In(mx, my, pad + 8, NewY(), sw - 16, 28)) { NewSession(); return; }
            if (U.In(mx, my, pad + 8, ClrY(), sw - 16, 28)) { ClearBody(); return; }

            int x0 = MainX(), mw = MainW(), ty = TogY();
            if (U.In(mx, my, x0, ty - 4, 84, 26))
            {
                searchOn = 1 - searchOn;
                status = searchOn == 1 ? "联网已开启（尽力而为）" : "联网已关闭";
                return;
            }
            if (U.In(mx, my, Tog2X(), ty - 4, 140, 26))
            {
                autoTools = 1 - autoTools;
                status = autoTools == 1 ? "自动工具已开启" : "自动工具已关闭";
                return;
            }
            if (U.In(mx, my, HelpX(), ty - 3, 64, 24))
            {
                AppendTurn("工具", HelpText());
                status = "帮助";
                return;
            }

            int iy = InY(), ih = InH(), iw = InW();
            if (U.In(mx, my, x0, iy, iw, ih)) { editMode = 1; return; }
            if (U.In(mx, my, x0 + iw + 8, iy, 84, ih))
            {
                if (busy == 1) { Host.AiCancel(); busy = 0; Host.SetAnim(0); status = "已停止"; return; }
                Send();
                return;
            }
            // Clicking the transcript keeps the editor focused (like AiChat).
            editMode = 1;
        }

        public override void OnKey(int ch)
        {
            if (editMode != 1)
            {
                if (ch < 32) return;
                editMode = 1;
            }
            if (ch == 27) { editMode = 0; return; }                 // Esc
            if (ch == 10 || ch == 13) { Send(); return; }           // Enter
            t.Key(ch);
        }
    }
}
