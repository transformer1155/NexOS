// =====================================================================
//  Browser.cs  -  a NexOS.Forms "WebBrowser" control, shared by the
//  kernel VM and the Windows WinForms harness.
// ---------------------------------------------------------------------
//  A real browser engine (WebKit / Gecko) cannot run inside the MiniCLR
//  VM, so the kernel-side control fetches the page with the kernel's own
//  HTTP client (net.cpp) and renders the returned body as scrollable
//  text.  On Windows the harness backs it with a real WebBrowser control
//  (see ShellForm.cs), but the shared App shell here is what both sides
//  use for the address bar, navigation and the text fallback.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class BrowserApp : App
    {
        TBox t;             // address-bar editor (caret + selection + undo)
        string body;
        int    editMode;     // 0 = view, 1 = editing the address
        int    scroll;
        int    status;       // 0 idle, 1 loading, 2 done, 3 error/offline

        const int BarH = 34, BarY = 8, BarX = 8;

        public BrowserApp()
        {
            t = new TBox();
            t.text  = "https://www.bing.com/";
            body = "";
            editMode = 0;
            scroll = 0;
            status = 0;
            // Log the default home so the test harness can confirm the
            // 64-bit browser opens on Bing without any user interaction.
            Host.Log("[browser] addr=" + t.text);
        }

        public override string GetTitle() { return "Browser"; }

        // ---- layout helpers (kept in sync between Paint / Click) ------
        int  BarW() { return Gfx.Width() - 16; }
        int  GoW()  { return 56; }
        int  GoX()  { return BarX + BarW() - GoW() - 4; }
        int  GoY()  { return BarY + 4; }
        int  GoH()  { return BarH - 8; }
        int  AddrX(){ return BarX + 8; }
        int  AddrW(){ return BarW() - GoW() - 16; }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();

            // Address bar background + border.
            Gfx.FillRound(BarX, BarY, BarW(), BarH, 6, C.Card);
            Gfx.DrawRound(BarX, BarY, BarW(), BarH, 6, C.BorderMid);

            // Address text (or the editing caret at the current position).
            if (editMode == 1)
            {
                Gfx.Text(AddrX(), BarY + 9, t.text, C.Text);
                if ((Host.Ticks() / 30) % 2 == 0) {
                    string before = "";
                    for (int i = 0; i < t.cursor; i++) before = U.Cat(before, Host.CharStr((int)t.text[i]));
                    int cx = AddrX() + Gfx.Measure(before);
                    Gfx.FillRect(cx, BarY + 9, 2, 16, C.Text);
                }
            }
            else
            {
                Gfx.Text(AddrX(), BarY + 9, t.text, C.TextSub);
            }

            // Go button (registered as a control for right-click).
            W.Primary(GoX(), GoY(), GoW(), GoH(), "Go");

            // Status line.
            int sy = BarY + BarH + 6;
            string st = "Ready";
            if (status == 1) st = "Loading...";
            else if (status == 3) st = "Error / offline";
            else if (status == 4) st = "Offline page (local)";
            else if (status == 2) st = U.Cat("Loaded ", U.I(body.Length), " chars");
            Gfx.Text(8, sy, st, C.TextFaint);

            // Content panel.
            int cy = sy + 22, ch = h - cy - 8;
            Gfx.FillRound(8, cy, w - 16, ch, 6, C.Card);
            Gfx.DrawRound(8, cy, w - 16, ch, 6, C.Border);
            RenderBody(8 + 10, cy + 8, w - 36, cy + ch - 8, scroll);
        }

        void RenderBody(int x, int y, int w, int bottom, int sc)
        {
            if (body.Length == 0) return;
            int lineH = 18;
            int i = 0, lines = 0;
            int maxLines = (bottom - y) / lineH;
            while (i < body.Length)
            {
                int start = i, j = i, curw = 0;
                while (j < body.Length)
                {
                    char c = body[j];
                    if (c == '\n') { j++; break; }
                    curw += 7;                       // ~7px/char, monospace-ish
                    if (curw > w) break;
                    j++;
                }
                if (lines >= sc && (lines - sc) < maxLines)
                {
                    string line = SubStr(body, start, j - start);
                    Gfx.Text(x, y + (lines - sc) * lineH, line, C.Text);
                }
                lines++;
                i = j;
                if (lines - sc > maxLines + 2) break;
            }
        }

        static string SubStr(string s, int a, int n)
        {
            string r = "";
            int e = a + n; if (e > s.Length) e = s.Length;
            for (int k = a; k < e; k++) r = U.Cat(r, Host.CharStr((int)s[k]));
            return r;
        }

        public override void OnClick(int mx, int my)
        {
            if (editMode == 1)
            {
                if (U.In(mx, my, GoX(), GoY(), GoW(), GoH())) { Fetch(); return; }
                editMode = 0;                        // click away commits
                return;
            }
            if (U.In(mx, my, GoX(), GoY(), GoW(), GoH())) { Fetch(); return; }
            if (U.In(mx, my, AddrX(), BarY, AddrW(), BarH)) {
                editMode = 1;
                Host.Log("[browser] addr=" + t.text);   // address bar focused
                return;
            }
            // Click in the content area scrolls one page down.
            scroll += 20;
        }

        // =====================================================================
        //  Address-bar input logic -- 1:1 port of winpe/iexplore.c :: on_char()
        // ---------------------------------------------------------------------
        //  The 32-bit IE services keystrokes ONLY in WM_CHAR (never in
        //  WM_KEYDOWN), so a single Backspace pops exactly one character and a
        //  single Enter navigates once.  We mirror that semantics verbatim:
        //    * Backspace (VK_BACK / 8)  -> single char delete (spop)
        //    * Enter     (VK_RETURN)    -> navigate (on_enter)
        //    * Escape                   -> unfocus (on_escape)
        //    * 0x20..0x7E               -> append one printable ASCII char
        //    * everything else          -> ignored (matches `ch<0x20||ch>0x7E`)
        //  The exact serial tag "[browser] addr=" is preserved so the headless
        //  test harness keeps working.
        // =====================================================================
        public override void OnKey(int ch)
        {
            if (editMode != 1) return;
            if (ch == -2 || ch == 10 || ch == 13) { Fetch(); return; }   // Enter
            if (ch == 27) { editMode = 0; return; }                     // Escape
            if (t.Key(ch)) { Host.Log("[browser] addr=" + t.text); return; }
        }

        public override void OnCtxRefresh() { Fetch(); }

        void Fetch()
        {
            editMode = 0;
            status = 1;
            Host.Log("[browser] fetching " + t.text);
            string r = Host.HttpGet(t.text);
            if (r == null || r.Length == 0) {
                // No network / DNS failure / HTTPS (no TLS in NexOS).
                // Render a graceful LOCAL start page instead of a bare error
                // string, so the browser is still useful while offline.
                Host.Log("[browser] fetch-failed " + t.text);
                body = BuildOfflinePage();
                status = 4;   // local offline page (rendered, not an error)
                Host.Log("[browser] offline-page " + t.text);
            } else {
                status = 2;
                body = r;
                // Log the size so the headless test can prove the browser
                // really fetched a complete page over the network.
                // Multi-operand '+' would need String.Concat(string,string,string,string),
                // which the MiniCLR corelib lacks -- nest U.Cat instead.
                Host.Log(U.Cat("[browser] fetched ", U.I(r.Length), " bytes from ", t.text));
            }
            scroll = 0;
        }

        // Build a friendly local "you are offline" start page.  Plain
        // http:// sites fetch fine when the network is up; https:// cannot
        // (NexOS has no TLS), so we explain that and suggest http:// URLs.
        string BuildOfflinePage()
        {
            string reason;
            if (StartsWith(t.text, "https://"))
                reason = "NexOS has no TLS, so https:// sites (Bing, Google, ...) cannot be fetched.";
            else
                reason = "Host unreachable or offline (no network / DNS failed).";
            string page = "";
            page = U.Cat(page, "NexOS Browser  -  Offline start page\n");
            page = U.Cat(page, "------------------------------------------\n\n");
            page = U.Cat(page, "Address : ", t.text, "\n");
            page = U.Cat(page, "Status  : ", reason, "\n\n");
            page = U.Cat(page, "What works here:\n");
            page = U.Cat(page, "  - Plain http:// sites load when the network is up.\n");
            page = U.Cat(page, "  - This offline page is rendered locally (no network needed).\n\n");
            page = U.Cat(page, "Try in the address bar:\n");
            page = U.Cat(page, "  http://example.com\n");
            page = U.Cat(page, "  http://neverssl.com\n\n");
            page = U.Cat(page, "Press Esc, edit the address, then Enter to retry.\n");
            return page;
        }

        // MiniCLR-safe prefix test (no string.StartsWith available).
        static bool StartsWith(string s, string p)
        {
            int n = p.Length;
            if (s == null || s.Length < n) return false;
            for (int i = 0; i < n; i++)
                if (s[i] != p[i]) return false;
            return true;
        }
    }
}
