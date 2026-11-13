// =====================================================================
//  AiChat.cs  -  local GGUF model chat window
// ---------------------------------------------------------------------
//  A conversation surface for the native GGUF engine.  Send() now ARMS an
//  asynchronous, frame-driven decode job (Host.AiStart) and returns at once;
//  OnPaint() calls Pump() to pull streamed text (Host.AiPoll / AiStatus).
//  The native side runs at most a few transformer layers per GUI frame, so
//  the mouse keeps moving instead of freezing on the click.
//
//  MiniCLR rules (see Forms.cs): no static initialisers, no floats, no
//  generics / interfaces / try-catch; strings only offer Length, [i]
//  and Concat (U.Cat).  All state lives in fields.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class AiChatApp : App
    {
        TBox   t;        // message editor
        string history;  // transcript ("你: ...\nAI: ...")
        int    editMode; // 0 idle, 1 typing
        int    turns;    // questions asked this session
        // Async decode state.  Send() only ARMS a native job and returns;
        // OnPaint() polls the produced text, so the click never blocks.
        int    busy;     // 1 = a decode job is running
        string histBase; // transcript prefix up to the current "AI: " line
        string reply;    // text streamed for the current answer

        public AiChatApp()
        {
            t = new TBox();
            editMode = 1;
            turns = 0;
            busy = 0;
            histBase = "";
            reply = "";
            history = "本地 AI 对话：输入问题，回车或点发送。\n边生成边显示，鼠标不会卡。";
        }

        public override string GetTitle() { return "AI 对话"; }

        // ---- layout helpers (Paint / Click kept in sync) ----------------
        int Pad()   { return 16; }
        int InY()   { return Gfx.Height() - Pad() - 36; }
        int InH()   { return 36; }
        int InW()   { return Gfx.Width() - 2 * Pad() - 118 - 8; }
        int SendX() { return Pad() + InW() + 8; }
        int SendW() { return 118; }
        int HistY() { return Pad() + 46; }
        int HistH() { return InY() - HistY() - 12; }

        // Word-wrap a transcript into the output card.  Slices with U.Sub
        // (one allocation per line): the O(n^2) byte accumulator pattern
        // exhausts the 512 KB managed heap on long transcripts, and slicing
        // also keeps every multi-byte glyph intact.
        static void Wrap(int x, int y, int maxw, int maxLines, string s, uint c)
        {
            if (s == null) return;
            int cols = maxw / 8 - 2; if (cols < 8) cols = 8;
            int i = 0, line = 0;
            while (i < s.Length && line < maxLines)
            {
                int ch = (int)s[i];
                if (ch == '\n' || ch == '\r') { i++; continue; }
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

        static string Trim(string s)
        {
            int a = 0, b = s.Length;
            while (a < b) { int c = (int)s[a]; if (c == ' ' || c == '\t' || c == '\n' || c == '\r') a++; else break; }
            while (b > a) { int c = (int)s[b - 1]; if (c == ' ' || c == '\t' || c == '\n' || c == '\r') b--; else break; }
            return U.Sub(s, a, b - a);
        }

        // Append one transcript turn, keeping the buffer bounded (the bump
        // heap is 512 KB and Gfx.Text slices per line, so ~1400 bytes is a
        // safe ceiling).  Cuts on UTF-8 character boundaries.
        void AppendTurn(string who, string text)
        {
            if (text == null) text = "";
            history = U.Cat(history, "\n", who, ": ", text);
            if (history.Length > 1400)
            {
                int cut = history.Length - 1400;
                while (cut < history.Length && ((int)history[cut] & 0xC0) == 0x80) cut++;
                history = U.Cat("...", U.Sub(history, cut, history.Length - cut));
            }
        }

        void Send()
        {
            string q = Trim(t.text);
            if (q.Length == 0) return;
            if (busy == 1) return;              // one decode at a time
            t.text = "";
            t.cursor = 0;
            turns = turns + 1;
            AppendTurn("你", q);
            histBase = U.Cat(history, "\nAI: ");
            reply = "";
            int rc = Host.AiStart(q);
            if (rc != 0)
            {
                history = U.Cat(histBase, "(推理引擎未就绪：模型未加载，详见串口日志)");
                return;
            }
            busy = 1;
            Host.SetAnim(1);                    // keep the frame loop alive
        }

        // Called once per paint: pull whatever the native worker produced in
        // the last frame(s) and rebuild only the current answer line.
        void Pump()
        {
            if (busy != 1) return;
            string chunk = Host.AiPoll();
            if (chunk != null && chunk.Length > 0)
            {
                reply = U.Cat(reply, chunk);
                if (reply.Length > 1200) reply = U.Sub(reply, reply.Length - 1200, 1200);
                history = U.Cat(histBase, reply);
            }
            int st = Host.AiStatus();
            if (st == 2 || st == 3 || st == 0)
            {
                if (reply.Length == 0)
                    history = U.Cat(histBase,
                        st == 3 ? "(推理出错，详见串口日志)" :
                                  "(引擎无输出：模型未加载或回答为空)");
                busy = 0;
                Host.SetAnim(0);
            }
            else Host.SetAnim(1);
        }

        public override void OnPaint()
        {
            Pump();
            W.Clear();
            int w = Gfx.Width(), pad = Pad();

            W.Header(pad, pad, "AI 对话");
            Gfx.Text(pad, pad + 26,
                     busy == 1 ? "本地 GGUF 模型推理中（异步，鼠标可继续操作）…"
                               : "本地 GGUF 模型推理。输入问题后回车或点发送。",
                     C.TextSub);

            // Transcript card.
            int hy = HistY(), hh = HistH();
            if (hh > 8)
            {
                W.Card(pad, hy, w - 2 * pad, hh);
                int lines = (hh - 20) / 18; if (lines < 1) lines = 1;
                Wrap(pad + 12, hy + 10, w - 2 * pad - 24, lines, history, C.Text);
            }

            // Message box + send button.
            Gfx.FillRound(pad, InY(), InW(), InH(), 8, 0xFFFFFFFFu);
            Gfx.DrawRound(pad, InY(), InW(), InH(), 8, 0x9B6BFFu);
            string shown = t.text;
            if (shown.Length == 0) shown = "输入问题，回车发送…";
            Gfx.Text(pad + 10, InY() + 10, shown, editMode == 1 ? C.Text : C.TextSub);
            if (editMode == 1 && (Host.Ticks() / 30) % 2 == 0)
            {
                string before = "";
                for (int i = 0; i < t.cursor; i++) before = U.Cat(before, Host.CharStr((int)t.text[i]));
                int cx = pad + 10 + Gfx.Measure(before);
                Gfx.FillRect(cx, InY() + 10, 2, 18, C.Text);
            }
            W.Primary(SendX(), InY(), SendW(), InH(), "发送");
            W.Voice("发送 send", SendX(), InY(), SendW(), InH());
        }

        public override void OnClick(int mx, int my)
        {
            if (U.In(mx, my, Pad(), InY(), InW(), InH())) { editMode = 1; return; }
            if (U.In(mx, my, SendX(), InY(), SendW(), InH()))
            {
                if (busy == 1) { Host.AiCancel(); busy = 0; Host.SetAnim(0); return; }
                Send(); return;
            }
        }

        public override void OnKey(int ch)
        {
            if (editMode == 1)
            {
                if (ch == 27) { editMode = 0; return; }                            // Esc
                if (ch == 10 || ch == 13) { Send(); return; }                      // Enter
                t.Key(ch);
            }
        }
    }
}
