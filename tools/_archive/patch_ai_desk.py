#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Turn the AI virtual desktop into a functional chat panel (feature B).

The desktop used to be a static stub (AiDesktopClick returned -1, AiDesktopPaint
just drew placeholder bubbles).  This patch makes it a real chat surface:

  * a persistent input line + "发送" (Send) button,
  * click the input box to focus, type, Enter sends,
  * each send runs `agent run <goal>` through the real kernel command path
    (Host.Exec -> gui_cb_exec_command -> run_command) and appends the result,
  * scrolling history of the last 16 exchanges.

MiniCLR has no static initialisers, so all chat state is lazily allocated by
AiInit() the first time any AI-desktop code runs.  CRLF-safe byte patch.
"""
import os, sys
ROOT = "/mnt/d/MyOS/bootloader"
os.chdir(ROOT)


def crlf(s):
    return s.replace("\n", "\r\n")


DESK = "csharp/apps/Shell/Desktop.cs"
with open(DESK, "rb") as f:
    data = f.read()


def replace(label, old, new):
    global data
    ob = crlf(old).encode("latin-1")
    nb = crlf(new).encode("latin-1")
    cnt = data.count(ob)
    if cnt != 1:
        print("FAIL [%s] expected 1 match, found %d" % (label, cnt))
        sys.exit(1)
    data = data.replace(ob, nb)
    print("ok   [%s] patched" % label)


# ---- 1) AI chat state fields, inserted after CurrentDesktop decl ----
replace("state-fields",
"""        static int  CurrentDesktop = 0;   // 0 = default desktop, 1 = AI virtual desktop
""",
"""        static int  CurrentDesktop = 0;   // 0 = default desktop, 1 = AI virtual desktop

        // ---- AI virtual-desktop chat (feature B) ----
        // No static initialisers in MiniCLR, so these stay null/0 until AiInit().
        static string[] aiHist = null;
        static int       aiHistN = 0;
        static string    aiBox   = null;
        static int       aiFocus = 0;
        static int       aiReady = 0;

        static void AiInit() {
            aiHist  = new string[40];
            aiHistN = 0;
            aiBox   = "";
            aiFocus = 0;
            aiReady = 0;
        }
        static void AiEnsure() { if (aiHist == null) AiInit(); }
        static void AiHistAdd(string s) {
            if (s == null) s = "";
            if (aiHistN < 40) { aiHist[aiHistN] = s; aiHistN++; }
            else { for (int i = 0; i < 39; i++) aiHist[i] = aiHist[i + 1]; aiHist[39] = s; }
        }
        static int AiPw(int w){ return (w * 64) / 100; }
        static int AiPx(int w){ return (w - AiPw(w)) / 2; }
        static int AiPy(int h){ return (h * 18) / 100; }
        static int AiPh(int h){ return (h * 64) / 100; }
        static int AiInX(int w){ return AiPx(w) + 16; }
        static int AiInY(int h){ int py = AiPy(h), ph = AiPh(h); return py + ph - 52; }
        static int AiInW(int w){ return AiPw(w) - 32 - 110; }
        static int AiInH(){ return 38; }
        static int AiBtnX(int w){ return AiPx(w) + 16 + (AiPw(w) - 32 - 110) + 8; }
        static int AiBtnY(int h){ return AiInY(h); }
        static int AiBtnW(){ return 94; }
        static int AiBtnH(){ return 38; }

        // Draw one (possibly long) message wrapped to <=maxw px from y.
        // Returns the number of 18px lines consumed (capped at 6).
        static int AiDrawMsg(int x, int y, int maxw, string s, uint c) {
            if (s == null) s = "";
            int cols = maxw / 8 - 2; if (cols < 8) cols = 8;
            int i = 0, line = 0, cur = 0; string buf = "";
            while (i < s.Length && line < 6) {
                int ch = (int)s[i];
                if (cur >= cols) { Gfx.Text(x, y + line * 18, buf, c); buf = ""; cur = 0; line++; if (line >= 6) break; }
                buf = U.Cat(buf, Host.CharStr(ch)); cur++; i++;
            }
            if (cur > 0 && line < 6) Gfx.Text(x, y + line * 18, buf, c);
            return (cur > 0 && line < 6) ? (line + 1) : line;
        }

        static void AiSend() {
            AiEnsure();
            if (aiBox == null || aiBox.Length == 0) return;
            AiHistAdd(U.Cat("You: ", aiBox));
            string goal = aiBox;
            aiBox = "";
            aiFocus = 0;
            Host.Log(U.Cat("[AIDESK] run: ", goal, "\\n"));
            if (aiReady == 0) { Host.Exec("agent init"); aiReady = 1; }
            string res = Host.Exec(U.Cat("agent run ", goal));
            if (res == null || res.Length == 0) res = "(no output)";
            if (res.Length > 700) {
                string r2 = "";
                for (int i = 0; i < 700; i++) r2 = U.Cat(r2, Host.CharStr((int)res[i]));
                res = U.Cat(r2, "...");
            }
            AiHistAdd(U.Cat("AI: ", res));
        }

        static void AiDesktopKey(int ch) {
            AiEnsure();
            if (aiFocus == 0) return;
            if (ch == 8 || ch == -1) {                       // Backspace
                if (aiBox != null && aiBox.Length > 0) {
                    string r = "";
                    for (int i = 0; i < aiBox.Length - 1; i++) r = U.Cat(r, Host.CharStr((int)aiBox[i]));
                    aiBox = r;
                }
                return;
            }
            if (ch == 10 || ch == 13 || ch == -2) { AiSend(); return; }   // Enter
            if (ch == 27) { aiFocus = 0; return; }                          // Esc
            if (ch >= 0x20 && ch < 0x7F) {                                 // printable
                string s = (aiBox == null) ? "" : aiBox;
                s = U.Cat(s, Host.CharStr(ch));
                if (s.Length < 200) aiBox = s;
                return;
            }
        }
""")

# ---- 2) Replace the static AiDesktopClick + AiDesktopPaint ----
replace("ai-desk-paint",
"""        static int AiDesktopClick(int mx, int my, int w, int h) { return -1; }
        static void AiDesktopPaint(int w, int h) {
            // Independent AI virtual desktop: deep indigo wallpaper + a centered
            // chat workspace.  Deliberately nothing like the default Portal so the
            // two desktops read as clearly separate surfaces.
            Gfx.Gradient(0, 0, w, h, 0x241A52u, 0x0B0B1Eu);
            int cx = w / 2, cy = (h * 30) / 100;
            int glow = h / 3;
            Gfx.FillCircle(cx, cy, glow, 0x2E2268u);
            Gfx.FillCircle(cx, cy, (glow * 72) / 100, 0x3A2C82u);
            Gfx.Text(18, 14, "AI \\u684C\\u9762  \\u00B7  \\u865A\\u62DF\\u684C\\u9762 2", 0xC3B6FFu);
            int pw = (w * 64) / 100, px = (w - pw) / 2;
            int py = (h * 18) / 100, ph = (h * 64) / 100;
            Gfx.FillRound(px, py, pw, ph, 16, 0x14122Eu);
            Gfx.DrawRound(px, py, pw, ph, 16, 0x4A3FA0u);
            Gfx.FillRound(px, py, pw, 40, 16, 0x211D44u);
            Gfx.DrawRound(px, py, pw, 40, 16, 0x4A3FA0u);
            Gfx.Text(px + 16, py + 12, "NexOS AI \\u52A9\\u624B", 0xC3B6FFu);
            Gfx.Text(px + pw - 16 - Gfx.Measure("online"), py + 12, "online", 0x6FE0A0u);
            int by = py + 56;
            Gfx.FillRound(px + 16, by, pw - 200, 34, 10, 0x2A2550u);
            Gfx.Text(px + 28, by + 9, "\\u4F60\\u597D\\uFF0C\\u6211\\u53EF\\u4EE5\\u5E2E\\u4F60\\u505A\\u4EC0\\u4E48\\uFF1F", 0xD8D2FFu);
            by += 46;
            Gfx.FillRound(px + 120, by, pw - 160, 34, 10, 0x3A2F72u);
            Gfx.Text(px + 132, by + 9, "\\u628A AI \\u653E\\u8FDB\\u72EC\\u7ACB\\u865A\\u62DF\\u684C\\u9762", 0xE6E2FFu);
            by += 46;
            Gfx.FillRound(px + 16, by, pw - 240, 34, 10, 0x2A2550u);
            Gfx.Text(px + 28, by + 9, "\\u5DF2\\u5207\\u6362\\u5230 AI \\u865A\\u62DF\\u684C\\u9762", 0x9FE0C0u);
            int iy = py + ph - 52;
            Gfx.FillRound(px + 16, iy, pw - 32, 38, 10, 0x0E0C22u);
            Gfx.DrawRound(px + 16, iy, pw - 32, 38, 10, 0x4A3FA0u);
            Gfx.Text(px + 28, iy + 11, "\\u8F93\\u5165\\u6307\\u4EE4\\u2026  (Ctrl+Win+\\u2191 \\u4E00\\u952E\\u5207\\u6362)", 0x7C73B0u);
            Gfx.TextCenter(0, h - 28, w, "Ctrl+Win+\\u2192 \\u8FDB\\u5165 AI \\u684C\\u9762   \\u00B7   Ctrl+Win+\\u2190 \\u8FD4\\u56DE\\u9ED8\\u8BA4\\u684C\\u9762   \\u00B7   Ctrl+Win+\\u2191 \\u4E00\\u952E\\u5207\\u6362", 0x8A7FD0u);
        }
""",
"""        static int AiDesktopClick(int mx, int my, int w, int h) {
            AiEnsure();
            if (U.In(mx, my, AiInX(w), AiInY(h), AiInW(w), AiInH())) { aiFocus = 1; return -1; }
            if (U.In(mx, my, AiBtnX(w), AiBtnY(h), AiBtnW(), AiBtnH())) { AiSend(); return -1; }
            aiFocus = 0;
            return -1;
        }
        static void AiDesktopPaint(int w, int h) {
            AiEnsure();
            Gfx.Gradient(0, 0, w, h, 0x241A52u, 0x0B0B1Eu);
            int cx = w / 2, cy = (h * 22) / 100;
            int glow = h / 3;
            Gfx.FillCircle(cx, cy, glow, 0x2E2268u);
            Gfx.FillCircle(cx, cy, (glow * 72) / 100, 0x3A2C82u);
            int pw = AiPw(w), px = AiPx(w), py = AiPy(h), ph = AiPh(h);
            Gfx.Text(18, 14, "AI \\u684C\\u9762  \\u00B7  \\u865A\\u62DF\\u684C\\u9762 2  \\u00B7  \\u804A\\u5929", 0xC3B6FFu);
            // Chat panel
            Gfx.FillRound(px, py, pw, ph, 16, 0x14122Eu);
            Gfx.DrawRound(px, py, pw, ph, 16, 0x4A3FA0u);
            Gfx.FillRound(px, py, pw, 40, 16, 0x211D44u);
            Gfx.DrawRound(px, py, pw, 40, 16, 0x4A3FA0u);
            Gfx.Text(px + 16, py + 12, "NexOS AI \\u52A9\\u624B", 0xC3B6FFu);
            Gfx.Text(px + pw - 16 - Gfx.Measure("online"), py + 12, "online", 0x6FE0A0u);
            // History: newest at the bottom, just above the input row.
            int y = AiInY(h) - 14;
            int shown = 0;
            for (int i = aiHistN - 1; i >= 0 && shown < 16; i--) {
                int lines = AiDrawMsg(px + 16, y - 18, pw - 32, aiHist[i], 0xD8D2FFu);
                y -= lines * 18 + 6;
                shown++;
                if (y < py + 48) break;
            }
            if (aiHistN == 0) {
                Gfx.Text(px + 16, py + 52, "\\u70B9\\u8F93\\u5165\\u6846\\u6253\\u5B57\\uFF0C\\u56DE\\u8F66\\u53D1\\u9001\\uFF1B\\u6211\\u4F1A\\u8C03\\u7528 agent run \\u540E\\u7AEF\\u3002", 0x8A7FD0u);
            }
            // Input box + Send button
            int iy = AiInY(h);
            Gfx.FillRound(AiInX(w), iy, AiInW(w), AiInH(), 10, aiFocus == 1 ? 0x0E0C22u : 0x1A1736u);
            Gfx.DrawRound(AiInX(w), iy, AiInW(w), AiInH(), 10, 0x4A3FA0u);
            string shown2 = (aiBox == null) ? "" : aiBox;
            if (shown2.Length == 0 && aiFocus == 0) shown2 = "\\u8F93\\u5165\\u6307\\u4EE4\\uFF0C\\u56DE\\u8F66\\u53D1\\u9001\\u2026";
            Gfx.Text(AiInX(w) + 10, iy + 11, shown2, aiFocus == 1 ? 0xE6E2FFu : 0x7C73B0u);
            if (aiFocus == 1 && (Host.Ticks() / 30) % 2 == 0) {
                int cw = Gfx.Measure(shown2);
                Gfx.FillRect(AiInX(w) + 10 + cw, iy + 9, 2, 18, 0xE6E2FFu);
            }
            int bx = AiBtnX(w), byx = AiBtnY(h);
            Gfx.FillRound(bx, byx, AiBtnW(), AiBtnH(), 10, 0x4A3FA0u);
            Gfx.DrawRound(bx, byx, AiBtnW(), AiBtnH(), 10, 0x6A5FD0u);
            Gfx.TextCenter(bx, byx + (AiBtnH() - 16) / 2, AiBtnW(), "\\u53D1\\u9001", 0xFFFFFFu);
            Gfx.TextCenter(0, h - 28, w, "Ctrl+Win+\\u2192 \\u8FDB\\u5165 AI \\u684C\\u9762   \\u00B7   Ctrl+Win+\\u2190 \\u8FD4\\u56DE   \\u00B7   \\u70B9\\u8F93\\u5165\\u6846\\u6253\\u5B57\\uFF0C\\u56DE\\u8F66\\u53D1\\u9001", 0x8A7FD0u);
        }
""")

# ---- 3) Route keys to the AI desktop when it is active ----
replace("desktop-key-route",
"""            if (ch == -7) { SwitchDesktop(0); return; }
            if (ch == -8) { SwitchDesktop(1); return; }
            if (ch == -9) { if (CurrentDesktop == 0) SwitchDesktop(1); else SwitchDesktop(0); return; }
            if (renameIdx < 0) return;
""",
"""            if (ch == -7) { SwitchDesktop(0); return; }
            if (ch == -8) { SwitchDesktop(1); return; }
            if (ch == -9) { if (CurrentDesktop == 0) SwitchDesktop(1); else SwitchDesktop(0); return; }
            if (CurrentDesktop == 1) { AiDesktopKey(ch); return; }
            if (renameIdx < 0) return;
""")

with open(DESK, "wb") as f:
    f.write(data)
print("ALL DESKTOP PATCHES APPLIED")
