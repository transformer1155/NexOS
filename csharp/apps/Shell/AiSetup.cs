// =====================================================================
//  AiSetup.cs  -  one-tap AI enablement wizard for non-technical users
// ---------------------------------------------------------------------
//  Requirement #3: a desktop shortcut that lets a user who knows nothing
//  about the terminal turn on NexOS AI in seconds.  No flags, no prompts
//  - open it from the desktop and tap a button.  Each button simply runs
//  the same kernel commands the shell offers (`ai init`, `agent init`)
//  through Host.Exec and shows the result, so the wizard is a thin,
//  safe GUI over functionality that already exists and is tested.
//
//  MiniCLR rules (see Forms.cs): no static initialisers, no floats,
//  no generics / interfaces / try-catch; durable state lives in fields
//  set by the ctor; strings only offer Length, [i] and Concat (U.Cat).
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class AiSetupApp : App
    {
        string status;     // last action result (may be multi-line)
        int    aiOn;       // 0 = off, 1 = on
        int    agentOn;    // 0 = off, 1 = on

        public AiSetupApp()
        {
            status  = "Tap a button to turn on NexOS AI. No terminal needed.";
            aiOn    = 0;
            agentOn = 0;
        }

        public override string GetTitle() { return "AI Setup"; }

        // Draw a possibly multi-line status string, wrapping at ~maxw/8 chars.
        static void Wrap(int x, int y, int maxw, string s, uint c)
        {
            int cols = maxw / 8 - 2;
            if (cols < 8) cols = 8;
            int i = 0, line = 0, cur = 0;
            string buf = "";
            while (i < s.Length)
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
                }
                buf = U.Cat(buf, Host.CharStr(ch));
                cur++; i++;
            }
            if (cur > 0) Gfx.Text(x, y + line * 18, buf, c);
        }

        // Trim trailing newlines / spaces so command output displays clean.
        static string TrimTail(string s)
        {
            int n = s.Length;
            while (n > 0)
            {
                int ch = (int)s[n - 1];
                if (ch == '\n' || ch == '\r' || ch == ' ') n--;
                else break;
            }
            string r = "";
            for (int i = 0; i < n; i++) r = U.Cat(r, Host.CharStr((int)s[i]));
            return r;
        }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 16;

            W.Header(pad, pad, "AI Setup Wizard");
            Gfx.Text(pad, pad + 26, "Turn on NexOS AI in one tap.", C.TextSub);

            int bw = w - 2 * pad;

            // Big primary action: enable everything in a single tap.
            W.Primary(pad, 64, bw, 46, "Enable AI & Agent");
            // Two secondary, individually scoped actions.
            int half = (bw - 8) / 2;
            W.Button(pad, 120, half, 40, "Enable AI only");
            W.Button(pad + half + 8, 120, half, 40, "Enable Agent only");

            // Status / result panel.
            int py = 176, ph = h - py - pad;
            if (ph > 8)
            {
                Gfx.FillRound(pad, py, bw, ph, 8, C.Card);
                Gfx.DrawRound(pad, py, bw, ph, 8, C.Border);
                string head = aiOn != 0 ? "AI engine: ON" : "AI engine: off";
                head = U.Cat(head, agentOn != 0 ? "    Agent: ON" : "    Agent: off");
                Gfx.Text(pad + 12, py + 12, head, C.Text);
                Wrap(pad + 12, py + 36, bw - 24, status, C.Text);
            }
        }

        public override void OnClick(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 16, bw = w - 2 * pad;
            int half = (bw - 8) / 2;

            if (U.In(mx, my, pad, 64, bw, 46))
            {
                string r1 = Host.Exec("ai init");
                string r2 = Host.Exec("agent init");
                aiOn = 1; agentOn = 1;
                status = U.Cat(TrimTail(r1), "\n", TrimTail(r2));
                return;
            }
            if (U.In(mx, my, pad, 120, half, 40))
            {
                status = TrimTail(Host.Exec("ai init"));
                aiOn = 1;
                return;
            }
            if (U.In(mx, my, pad + half + 8, 120, half, 40))
            {
                status = TrimTail(Host.Exec("agent init"));
                agentOn = 1;
                return;
            }
        }
    }
}
