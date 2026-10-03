// =====================================================================
//  NetTools.cs  -  network diagnostics console for NexOS
// ---------------------------------------------------------------------
//  Honest, real diagnostics: NIC presence (Host.NicPresent), a live
//  `net info` panel (auto-refreshed ~1.2 s), and a ping tool.  There is
//  no synthetic throughput graph -- what you see is exactly what the
//  network stack reports.  MiniCLR rules (see Forms.cs): no static
//  initialisers, no floats, no generics / interfaces / try-catch;
//  durable state lives in fields set by the ctor.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class NetToolsApp : App
    {
        TBox   host;       // ping target editor (caret + selection + undo)
        string status;     // last `net info` / `netstat` capture
        string pingOut;    // last ping result
        int    editMode;   // 0 idle, 1 typing the ping target
        int    autoOn;     // 1 = auto-refresh the status panel
        int    lastRef;    // TickMs() of the last auto refresh

        public NetToolsApp()
        {
            host = new TBox();
            editMode = 1;          // focus the target box immediately
            autoOn = 1;
            pingOut = "Enter a host above (e.g. 10.0.2.2) and press Ping.";
            status = RefreshRaw();
            lastRef = 0;
        }

        public override string GetTitle() { return "Net Tools"; }

        // ---- layout helpers (Paint / Click kept in sync) ----------------
        int  Pad()   { return 16; }
        int  BoxY()  { return 70; }
        int  BoxH()  { return 32; }
        int  BoxW()  { return Gfx.Width() - 2 * Pad() - 118 - 8; }
        int  BoxX()  { return Pad(); }
        int  PingW() { return 118; }
        int  PingX() { return BoxX() + BoxW() + 8; }
        int  BtnY()  { return BoxY() + BoxH() + 12; }
        int  BtnH()  { return 30; }
        int  RefrW() { return 140; }
        int  AutoW() { return 150; }
        int  StartW(){ return 150; }
        int  RefrX() { return Pad(); }
        int  AutoX() { return RefrX() + RefrW() + 10; }
        int  StartX(){ return AutoX() + AutoW() + 10; }
        int  CardY() { return BtnY() + BtnH() + 16; }

        // Capture the network status text (or a friendly notice if the NIC
        // is not up).  Capped so we never exhaust the 512 KB bump heap.
        static string RefreshRaw()
        {
            if (Host.NicPresent() == 0)
                return "Network not initialized.\nRun 'netstart' (button below) or type it at a terminal.";
            return SafeCap(Host.Exec("net info"));
        }

        // A `net info` / `ping` transcript can be a few KB; cap what we keep.
        static string SafeCap(string res)
        {
            if (res == null) return "";
            if (res.Length <= 480) return res;
            int cut = 480;
            while (cut > 0 && ((int)res[cut] & 0xC0) == 0x80) cut--;  // UTF-8 safe
            return U.Cat(U.Sub(res, 0, cut), "...");
        }

        // Wrap `s` into <=maxLines lines of <=cols chars, drawn from y.
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

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height(), pad = Pad();

            W.Header(pad, pad, "Net Tools");

            // NIC status chip.
            int nic = Host.NicPresent();
            uint nicCol = nic != 0 ? 0x1FB85Au : 0xE5534Bu;
            Gfx.FillRound(pad, pad + 26, 150, 22, 6, 0x16202Bu);
            Gfx.DrawRound(pad, pad + 26, 150, 22, 6, nicCol);
            Gfx.Text(pad + 12, pad + 31, nic != 0 ? "LINK UP" : "NO LINK", nicCol);

            // Auto-refresh the status panel (~1.2 s cadence) when enabled.
            if (autoOn != 0)
            {
                int now = Host.TickMs();
                if (now - lastRef >= 1200 || lastRef == 0) { status = RefreshRaw(); lastRef = now; }
            }

            // Ping target box.
            Gfx.FillRound(BoxX(), BoxY(), BoxW(), BoxH(), 6, 0xFFFFFFFF);
            Gfx.DrawRound(BoxX(), BoxY(), BoxW(), BoxH(), 6, C.Accent);
            string shown = host.text;
            if (shown.Length == 0) shown = "ping target, e.g. 10.0.2.2";
            Gfx.Text(BoxX() + 8, BoxY() + 8, shown, editMode == 1 ? C.Text : C.TextSub);
            if (editMode == 1 && (Host.Ticks() / 30) % 2 == 0)
            {
                string before = "";
                for (int i = 0; i < host.cursor; i++) before = U.Cat(before, Host.CharStr((int)host.text[i]));
                int cx = BoxX() + 8 + Gfx.Measure(before);
                Gfx.FillRect(cx, BoxY() + 8, 2, 16, C.Text);
            }
            W.Primary(PingX(), BoxY(), PingW(), BoxH(), "Ping");
            W.Voice("ping", PingX(), BoxY(), PingW(), BoxH());

            // Button row: Refresh / Auto toggle / Net Start.
            W.Button(RefrX(), BtnY(), RefrW(), BtnH(), "Refresh");
            W.Voice("刷新 refresh", RefrX(), BtnY(), RefrW(), BtnH());
            W.Button(AutoX(), BtnY(), AutoW(), BtnH(), autoOn != 0 ? "Auto: ON" : "Auto: OFF");
            W.Voice("自动 auto", AutoX(), BtnY(), AutoW(), BtnH());
            W.Button(StartX(), BtnY(), StartW(), BtnH(), "Net Start");
            W.Voice("启动网络 netstart", StartX(), BtnY(), StartW(), BtnH());

            // Output card: network status (top) + ping result (bottom).
            int cy = CardY();
            int chh = h - cy - pad;
            if (chh > 8)
            {
                W.Card(pad, cy, w - 2 * pad, chh);
                W.Header(pad + 12, cy + 12, "Network status");
                int statusLines = (chh - 96) / 18; if (statusLines < 1) statusLines = 1;
                Wrap(pad + 12, cy + 36, w - 2 * pad - 24, statusLines, status, C.Text);
                int div = cy + 36 + statusLines * 18 + 6;
                Gfx.FillRect(pad + 12, div, w - 2 * pad - 24, 1, C.Border);
                W.Header(pad + 12, div + 10, "Ping");
                int pingLines = (chh - (div + 10 - cy) - 20) / 18; if (pingLines < 1) pingLines = 1;
                Wrap(pad + 12, div + 32, w - 2 * pad - 24, pingLines, pingOut, C.Text);
            }
        }

        void DoPing()
        {
            editMode = 0;
            if (host.text.Length == 0) { pingOut = "Enter a host first."; return; }
            pingOut = SafeCap(Host.Exec(U.Cat("ping ", host.text)));
        }

        public override void OnClick(int mx, int my)
        {
            if (U.In(mx, my, BoxX(), BoxY(), BoxW(), BoxH()))      { editMode = 1; return; }
            if (U.In(mx, my, PingX(), BoxY(), PingW(), BoxH()))    { DoPing(); return; }
            if (U.In(mx, my, RefrX(), BtnY(), RefrW(), BtnH()))   { status = RefreshRaw(); lastRef = Host.TickMs(); return; }
            if (U.In(mx, my, AutoX(), BtnY(), AutoW(), BtnH()))   { autoOn = autoOn != 0 ? 0 : 1; if (autoOn != 0) { status = RefreshRaw(); lastRef = Host.TickMs(); } return; }
            if (U.In(mx, my, StartX(), BtnY(), StartW(), BtnH())) { status = SafeCap(Host.Exec("netstart")); lastRef = Host.TickMs(); return; }
        }

        public override void OnKey(int ch)
        {
            if (editMode == 1)
            {
                if (ch == 27) { editMode = 0; return; }                                  // Esc
                if (ch == 10 || ch == 13) { if (host.text.Length > 0) DoPing(); else editMode = 0; return; } // Enter
                host.Key(ch);
            }
        }
    }
}
