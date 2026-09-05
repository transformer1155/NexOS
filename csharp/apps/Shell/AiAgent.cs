// =====================================================================
//  AiAgent.cs  -  desktop front-end for the NexOS multi-agent pipeline
// ---------------------------------------------------------------------
//  Thin GUI over the kernel `agent run <goal>` command.  The shell already
//  offers `ai init` / `agent init` / `agent run` at the prompt (see the
//  AiSetup wizard); this app lets a mouse-only user type a goal and watch
//  the Planner -> Actor -> Critic pipeline run, without touching a terminal.
//
//  MiniCLR rules (see Forms.cs): no static initialisers, no floats,
//  no generics / interfaces / try-catch; durable state lives in fields
//  set by the ctor; strings only offer Length, [i] and Concat (U.Cat).
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class AiAgentApp : App
    {
        TBox t;            // goal-box editor (caret + selection + undo)
        string output;     // last pipeline result (may be multi-line)
        int    editMode;   // 0 = idle, 1 = typing in the goal box
        string modelMsg;   // model status line
        string envMsg;     // environment (VM / bare metal) line

        public AiAgentApp()
        {
            t = new TBox();
            output = "Type a goal above, then press Run (or Enter).";
            editMode = 1;   // focus the goal box immediately so typing works
            modelMsg = "Default model: Qwen-1.7B (Q4_K_M, GGUF).";
            envMsg = Host.Exec("model env");
        }

        public override string GetTitle() { return "AI Agent"; }

        // ---- layout helpers (Paint / Click kept in sync) --------------
        int  BoxX() { return 16; }
        int  BoxY() { return 86; }
        int  BoxH() { return 34; }
        int  RunW() { return 120; }
        int  RunH() { return 34; }
        int  BoxW() { return Gfx.Width() - 32 - RunW() - 8; }
        int  RunX() { return BoxX() + BoxW() + 8; }
        int  LoadX() { return BoxX(); }
        int  LoadY() { return BoxY() + BoxH() + 12; }
        int  LoadW() { return 200; }
        int  LoadH() { return 30; }

        // Wrap `s` into <=maxLines lines of <=cols chars, drawn from y.
        static void Wrap(int x, int y, int maxw, int maxLines, string s, uint c)
        {
            int cols = maxw / 8 - 2;
            if (cols < 8) cols = 8;
            int i = 0, line = 0, cur = 0;
            string buf = "";
            while (i < s.Length && line < maxLines)
            {
                int ch = (int)s[i];
                if (ch == '\n' || ch == '\r')
                {
                    Gfx.Text(x, y + line * 18, buf, c);
                    buf = ""; cur = 0; line++; i++;
                    continue;
                }
                if (cur >= cols)
                {
                    Gfx.Text(x, y + line * 18, buf, c);
                    buf = ""; cur = 0; line++;
                    if (line >= maxLines) break;
                }
                buf = U.Cat(buf, Host.CharStr(ch));
                cur++; i++;
            }
            if (cur > 0 && line < maxLines) Gfx.Text(x, y + line * 18, buf, c);
        }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 16;

            W.Header(pad, pad, "AI Agent");
            Gfx.Text(pad, pad + 26, "Goal for the Planner / Actor / Critic pipeline:", C.TextSub);
            Gfx.Text(pad, pad + 44, modelMsg, C.TextSub);
            Wrap(pad, pad + 62, Gfx.Width() - 2 * pad, 2, envMsg, C.TextSub);

            // Goal box.
            Gfx.FillRound(BoxX(), BoxY(), BoxW(), BoxH(), 6, 0xFFFFFFFF);
            Gfx.DrawRound(BoxX(), BoxY(), BoxW(), BoxH(), 6, C.Accent);
            string shown = t.text;
            if (shown.Length == 0) shown = "e.g. create a report and email it";
            Gfx.Text(BoxX() + 8, BoxY() + 9, shown, editMode == 1 ? C.Text : C.TextSub);
            if (editMode == 1 && (Host.Ticks() / 30) % 2 == 0) {
                string before = "";
                for (int i = 0; i < t.cursor; i++) before = U.Cat(before, Host.CharStr((int)t.text[i]));
                int cx = BoxX() + 8 + Gfx.Measure(before);
                Gfx.FillRect(cx, BoxY() + 9, 2, 16, C.Text);
            }

            // Run button (right of the goal box).
            W.Primary(RunX(), BoxY(), RunW(), RunH(), "Run Agent");
            W.Voice("运行 run", RunX(), BoxY(), RunW(), RunH());

            // Model: default Qwen-1.7B + Load button.
            W.Primary(LoadX(), LoadY(), LoadW(), LoadH(), "Load Qwen-1.7B");
            W.Voice("加载模型 load model", LoadX(), LoadY(), LoadW(), LoadH());
            Gfx.Text(LoadX() + LoadW() + 10, LoadY() + 9,
                     "selects the default model", C.TextSub);

            // Output panel.
            int py = LoadY() + LoadH() + 14;
            int ph = h - py - pad;
            if (ph > 8)
            {
                Gfx.FillRound(pad, py, w - 2 * pad, ph, 8, C.Card);
                Gfx.DrawRound(pad, py, w - 2 * pad, ph, 8, C.Border);
                int lines = (ph - 24) / 18;
                if (lines < 1) lines = 1;
                Wrap(pad + 12, py + 12, w - 2 * pad - 24, lines, output, C.Text);
            }
        }

        // Run the pipeline: make sure engine + framework are live, then ask
        // the Planner to decompose the goal.
        void DoRun()
        {
            editMode = 0;
            Host.Exec("agent init");                 // idempotent at AI level
            output = SafeCap(Host.Exec(U.Cat("agent run ", t.text)));
        }

        // Select the default open-source model (Qwen-1.7B) for the engine.
        void DoLoadModel()
        {
            output = SafeCap(Host.Exec("model run qwen1.7b"));
            modelMsg = "Active model: Qwen-1.7B (selected).";
            envMsg = SafeCap(Host.Exec("model env"));
        }

        // A `agent run` transcript can be several KB; the MiniCLR bump heap
        // is only 512 KB with no GC, so an uncapped string exhausts it and
        // takes the whole managed shell down (the AI desktop hit the same
        // trap).  Cap every exec result we display.
        static string SafeCap(string res)
        {
            if (res == null) return "";
            if (res.Length <= 480) return res;
            int cut = 480;
            while (cut > 0 && ((int)res[cut] & 0xC0) == 0x80) cut--;  // UTF-8 safe
            return U.Cat(U.Sub(res, 0, cut), "...");
        }

        public override void OnClick(int mx, int my)
        {
            // Goal box -> focus typing.
            if (U.In(mx, my, BoxX(), BoxY(), BoxW(), BoxH()))
            {
                editMode = 1;
                return;
            }
            // Run button.
            if (U.In(mx, my, RunX(), BoxY(), RunW(), RunH()))
            {
                if (t.text.Length > 0) DoRun();
                else { output = "Enter a goal first."; editMode = 1; }
                return;
            }
            // Load Qwen-1.7B button.
            if (U.In(mx, my, LoadX(), LoadY(), LoadW(), LoadH()))
            {
                DoLoadModel();
                return;
            }
        }

        public override void OnKey(int ch)
        {
            if (editMode == 1)
            {
                if (ch == 27) { editMode = 0; return; }                                // Esc
                if (ch == 10 || ch == 13) { if (t.text.Length > 0) DoRun(); else editMode = 0; return; } // Enter
                t.Key(ch);
            }
        }
    }
}
