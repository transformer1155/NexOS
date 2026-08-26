// =====================================================================
//  Login.cs  -  the graphical sign-in screen (managed lock screen)
// ---------------------------------------------------------------------
//  NexOS boots straight into the desktop now, so the credentials have
//  to be collected in the GUI instead of at the text console.  This file
//  owns the whole lock screen: backdrop, clock, account picker, the two
//  input fields and the sign-in button.
//
//  It deliberately knows nothing about passwords.  Host.LoginCheck() is
//  the only way in: the kernel hashes the pair against its account
//  database and, when it matches, commits the session (uid / euid) and
//  hands back the uid.  A rejected attempt just returns -1, so no hash
//  ever crosses into managed memory.
//
//  MiniCLR constraints observed throughout: no floats, no generics, no
//  interfaces, no try/catch, and static field initialisers never run --
//  every static is primed in Init().
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public static class Login
    {
        // ---- session state -------------------------------------------
        static bool   active;      // true while the lock screen owns the screen
        static string user;        // account name being typed / picked
        static string pass;        // password buffer (never leaves this file)
        static string err;         // one-line failure message, "" when clean
        static int    focus;       // 0 = user field, 1 = password field
        static int    attempts;    // failed tries this session (shown after 2)
        static int    nuser;       // Host.UserCount() cached at Init()

        // ---- layout, computed once per frame by Layout() -------------
        // Click() needs the exact same boxes Paint() drew, so the maths
        // lives in one place and both call it.
        static int cardX, cardY, cardW, cardH;
        static int fieldX, fieldW, fieldH;
        static int userY, passY, btnY, chipY;

        // Palette.  The lock screen is always dark (like Windows) even
        // when the desktop is running the light theme.
        const uint CARD_BG   = 0x17171B;
        const uint CARD_EDGE = 0x33333A;
        const uint FIELD_BG  = 0x232329;
        const uint FIELD_HOT = 0x2C2C34;
        const uint FG        = 0xFFFFFF;
        const uint FG_DIM    = 0xA8A8B4;
        const uint ERR_FG    = 0xFF6B6B;

        // ---- lifecycle -----------------------------------------------
        public static void Init()
        {
            nuser    = Host.UserCount();
            user     = nuser > 0 ? Host.UserName(0) : "root";
            pass     = "";
            err      = "";
            focus    = 1;              // the account is pre-filled, so start on the password
            attempts = 0;
            // The kernel is the single source of truth: it reports -1 for
            // "nobody is signed in".  A text-mode boot that already ran
            // login_prompt() therefore lands straight on the desktop.
            // Arm the graphical lock screen only when there are accounts
            // to sign into.  (The 64-bit VM's machine-state callbacks are
            // not wired, so UserCount() is 0 there -> boot straight to the
            // desktop instead of a dead-end lock screen with no accounts.)
            active   = nuser > 0;
            Host.Log(active ? "[LOGIN] lock screen armed" : "[LOGIN] session already signed in");
        }

        public static int IsActive() { return active ? 1 : 0; }

        // WinHost / --shot only: skip the lock screen so the desktop is
        // visible in headless previews.  Not used by the VM (which boots
        // straight into the lock screen for security).
        public static void BypassForHost() { active = false; }

        // Re-arm the lock screen (Start menu -> Sign out, screen lock...).
        public static void Lock()
        {
            pass = ""; err = ""; focus = 1; attempts = 0;
            active = true;
        }

        // ---- geometry -------------------------------------------------
        static void Layout(int w, int h)
        {
            cardW = 400; cardH = 430;
            if (cardW > w - 40) cardW = w - 40;
            cardX = (w - cardW) / 2;
            cardY = (h - cardH) / 2 + 20;
            if (cardY < 96) cardY = 96;

            fieldW = cardW - 96;
            fieldH = 36;
            fieldX = cardX + 48;

            userY = cardY + 190;
            passY = cardY + 248;
            btnY  = cardY + 306;
            chipY = cardY + 364;
        }

        // ---- painting --------------------------------------------------
        public static void Paint(int w, int h)
        {
            Layout(w, h);

            // Backdrop: the desktop wallpaper when the texture pack is
            // present, otherwise the theme gradient darkened a couple of
            // stops so the card still reads against it.
            if (Gfx.HasImage(Tex.Wall) != 0) Gfx.Image(Tex.Wall, 0, 0, w, h);
            else Gfx.Gradient(0, 0, w, h, U.Shade(Theme.WallTop, -12), U.Shade(Theme.WallBot, -40));

            Clock(w);
            Card(w, h);
        }

        // Big centred clock across the top, Windows-lock-screen style.
        // The bitmap font is one fixed size, so "big" is faked by drawing
        // the string four times one pixel apart (a cheap bold).
        static void Clock(int w)
        {
            string t = U.Cat(Two(Host.Hour()), ":", Two(Host.Minute()));
            int tw = Gfx.Measure(t);
            int tx = (w - tw) / 2;
            Gfx.Text(tx + 1, 49, t, 0x000000);          // drop shadow
            Gfx.Text(tx,     48, t, FG);
            Gfx.Text(tx + 1, 48, t, FG);                // 1px smear == bold

            string sub = U.Cat("NexOS  ", Host.OsName());
            int sw = Gfx.Measure(sub);
            Gfx.Text((w - sw) / 2 + 1, 71, sub, 0x000000);
            Gfx.Text((w - sw) / 2,     70, sub, FG_DIM);
        }

        static void Card(int w, int h)
        {
            Gfx.FillRound(cardX, cardY, cardW, cardH, 16, CARD_BG);
            Gfx.DrawRound(cardX, cardY, cardW, cardH, 16, CARD_EDGE);

            // ---- avatar: an accent disc with the account initial -------
            int cx = cardX + cardW / 2;
            int ay = cardY + 78;
            Gfx.FillCircle(cx, ay, 46, U.Shade(Theme.Accent, -30));
            Gfx.FillCircle(cx, ay, 42, Theme.Accent);
            string ini = Initial();
            Gfx.Text(cx - Gfx.Measure(ini) / 2, ay - 8, ini, FG);

            // ---- account name -----------------------------------------
            string nm = (user == null || user.Length == 0) ? Lang.T("lock.noaccount") : user;
            Gfx.TextCenter(cardX, cardY + 138, cardW, nm, FG);
            Gfx.TextCenter(cardX, cardY + 158, cardW, Lang.T("lock.subtitle"), FG_DIM);

            // ---- fields ------------------------------------------------
            Field(fieldX, userY, Lang.T("lock.user"), user, focus == 0, false);
            Field(fieldX, passY, Lang.T("lock.pass"),  Mask(),  focus == 1, true);

            // ---- sign-in button ----------------------------------------
            Gfx.FillRound(fieldX, btnY, fieldW, fieldH, 6, Theme.Accent);
            Gfx.TextCenter(fieldX, btnY + 10, fieldW, Lang.T("lock.signin"), FG);

            // ---- error / hint ------------------------------------------
            if (err != null && err.Length > 0)
                Gfx.TextCenter(cardX, btnY + 46, cardW, err, ERR_FG);
            else
                Gfx.TextCenter(cardX, btnY + 46, cardW, Lang.T("lock.hint"), FG_DIM);

            // ---- account chips (only worth drawing for >1 account) -----
            if (nuser > 1) Chips();

            // After a couple of misses, remind the operator of the seeded
            // credentials instead of letting them lock themselves out of
            // their own VM.
            if (attempts >= 2)
                Gfx.TextCenter(0, h - 24, w, U.Cat(Lang.T("lock.defaccounts"), "  root / admin      guest / guest"), FG_DIM);
        }

        // One labelled input box.  `masked` only changes the caret maths
        // (the caller already substituted asterisks for the text).
        static void Field(int x, int y, string label, string text, bool hot, bool masked)
        {
            Gfx.Text(x, y - 18, label, FG_DIM);
            Gfx.FillRound(x, y, fieldW, fieldH, 6, hot ? FIELD_HOT : FIELD_BG);
            Gfx.DrawRound(x, y, fieldW, fieldH, 6, hot ? Theme.Accent : CARD_EDGE);

            string s = text == null ? "" : text;
            Gfx.Text(x + 12, y + 10, s, FG);

            // Blinking caret on the focused field (500 ms duty cycle).
            if (hot && ((Host.TickMs() / 500) % 2) == 0)
            {
                int caret = x + 12 + Gfx.Measure(s) + 1;
                if (caret > x + fieldW - 6) caret = x + fieldW - 6;
                Gfx.FillRect(caret, y + 8, 2, 20, FG);
            }
        }

        // Up to four account chips so the operator can switch identity
        // without retyping the name.
        static void Chips()
        {
            int n = nuser; if (n > 4) n = 4;
            int cw = (fieldW - (n - 1) * 8) / n;
            if (cw < 40) cw = 40;
            for (int i = 0; i < n; i++)
            {
                string nm = Host.UserName(i);
                int x = fieldX + i * (cw + 8);
                bool sel = Same(nm, user);
                Gfx.FillRound(x, chipY, cw, 28, 14, sel ? Theme.Accent : FIELD_BG);
                Gfx.DrawRound(x, chipY, cw, 28, 14, sel ? Theme.Accent : CARD_EDGE);
                Gfx.TextCenter(x, chipY + 6, cw, nm, sel ? FG : FG_DIM);
            }
        }

        // ---- input -----------------------------------------------------
        // Returns 1 when the click was consumed (always, while locked --
        // nothing behind the lock screen may see the mouse).
        public static int Click(int mx, int my)
        {
            if (!active) return 0;
            Layout(Gfx.ScreenW(), Gfx.ScreenH());

            if (U.In(mx, my, fieldX, userY, fieldW, fieldH)) { focus = 0; return 1; }
            if (U.In(mx, my, fieldX, passY, fieldW, fieldH)) { focus = 1; return 1; }
            if (U.In(mx, my, fieldX, btnY,  fieldW, fieldH)) { Submit();  return 1; }

            if (nuser > 1)
            {
                int n = nuser; if (n > 4) n = 4;
                int cw = (fieldW - (n - 1) * 8) / n;
                if (cw < 40) cw = 40;
                for (int i = 0; i < n; i++)
                {
                    int x = fieldX + i * (cw + 8);
                    if (U.In(mx, my, x, chipY, cw, 28))
                    {
                        user = Host.UserName(i);
                        pass = ""; err = ""; focus = 1;
                        return 1;
                    }
                }
            }
            return 1;   // swallow everything else
        }

        // ch is raw ASCII from gui.cpp, or one of the negative virtual
        // keys the host uses for control keys (-1 backspace, -2 enter).
        public static void Key(int ch)
        {
            if (!active) return;

            if (ch == 9)                              { focus = 1 - focus; return; }   // Tab
            if (ch == -2 || ch == 10 || ch == 13)     { Submit();          return; }   // Enter
            if (ch == -1 || ch == 8)                  { Back();            return; }   // Backspace
            if (ch == 27) return;                                                      // ESC: no escape hatch
            if (ch < 32 || ch > 126) return;                                           // ignore the rest

            if (focus == 0) { if (user.Length < 15) user = U.Cat(user, Host.CharStr(ch)); }
            else            { if (pass.Length < 32) pass = U.Cat(pass, Host.CharStr(ch)); }
            err = "";
        }

        static void Back()
        {
            if (focus == 0) user = Chop(user);
            else            pass = Chop(pass);
            err = "";
        }

        // ---- the one call that can unlock the machine ------------------
        static void Submit()
        {
            if (user == null || user.Length == 0)
            {
                err = Lang.T("lock.err.user"); focus = 0; return;
            }
            int uid = Host.LoginCheck(user, pass);
            if (uid >= 0)
            {
                Host.Log(U.Cat("[LOGIN] signed in as ", user, " uid=", U.I(uid)));
                active = false;
                pass = ""; err = ""; attempts = 0;
                return;
            }
            attempts = attempts + 1;
            pass  = "";
            focus = 1;
            err   = Lang.T("lock.err.cred");
            Host.Log(U.Cat("[LOGIN] rejected ", user));
        }

        // ---- small helpers ---------------------------------------------
        // No string.Substring in the corelib, so drop the last character
        // by rebuilding the string one char at a time.
        static string Chop(string s)
        {
            if (s == null) return "";
            int n = s.Length;
            if (n == 0) return "";
            string r = "";
            for (int i = 0; i < n - 1; i++) r = U.Cat(r, Host.CharStr((int)s[i]));
            return r;
        }

        static string Mask()
        {
            string r = "";
            int n = pass == null ? 0 : pass.Length;
            for (int i = 0; i < n; i++) r = U.Cat(r, "*");
            return r;
        }

        static string Initial()
        {
            if (user == null || user.Length == 0) return "?";
            int c = (int)user[0];
            if (c >= 'a' && c <= 'z') c = c - 32;
            return Host.CharStr(c);
        }

        static string Two(int v)
        {
            if (v < 0) v = 0;
            if (v < 10) return U.Cat("0", U.I(v));
            return U.I(v);
        }

        static bool Same(string a, string b)
        {
            if (a == null || b == null) return false;
            int n = a.Length;
            if (n != b.Length) return false;
            for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
            return true;
        }
    }
}
