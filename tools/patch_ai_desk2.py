#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Finish the AI-desktop chat panel: replace the static AiDesktopClick /
AiDesktopPaint stubs (the helper methods were already inserted by
patch_ai_desk.py).  Uses a brace scan so we never have to match the
Chinese string literals inside the old paint function.

Also routes desktop keystrokes to AiDesktopKey() when the AI desktop is active.
CRLF-safe byte patch.
"""
import os, sys
ROOT = "/mnt/d/MyOS/bootloader"
os.chdir(ROOT)
DESK = "csharp/apps/Shell/Desktop.cs"
with open(DESK, "rb") as f:
    data = f.read()


def crlf(s):
    return s.replace("\n", "\r\n")


# ---- 1) Replace the one-line AiDesktopClick stub (ASCII, unique) ----
old1 = b"        static int AiDesktopClick(int mx, int my, int w, int h) { return -1; }\r\n"
if data.count(old1) != 1:
    print("FAIL [click] matches=%d" % data.count(old1)); sys.exit(1)
new1 = crlf("""        static int AiDesktopClick(int mx, int my, int w, int h) {
            AiEnsure();
            // Taskbar (bottom strip) keeps working on the AI desktop.
            if (my >= h - TaskH) {
                int t = TrayHit(mx, my, w, h);
                if (t >= 0) { OpenTrayPopup(t, mx, my); return -1; }
                int bx = GroupX(w);
                int by = h - TaskH + (TaskH - BtnSz) / 2;
                if (U.In(mx, my, bx, by, BtnSz, BtnSz)) { menuOpen = true; return -1; }
                for (int i = 0; i < tN; i++) {
                    int x = bx + (i + 1) * (BtnSz + BtnGap);
                    if (U.In(mx, my, x, by, BtnSz, BtnSz)) return tKind[i];
                }
                return -1;
            }
            if (U.In(mx, my, AiInX(w), AiInY(h), AiInW(w), AiInH())) { aiFocus = 1; return -1; }
            if (U.In(mx, my, AiBtnX(w), AiBtnY(h), AiBtnW(), AiBtnH())) { AiSend(); return -1; }
            aiFocus = 0;
            return -1;
        }
""")
data = data.replace(old1, new1.encode("utf-8"))
print("ok   [click] patched")


# ---- 2) Replace AiDesktopPaint via balanced-brace scan ----
sig = b"static void AiDesktopPaint(int w, int h) {"
i = data.find(sig)
if i < 0:
    print("FAIL [paint] signature not found"); sys.exit(1)
depth = 0
j = i
n = len(data)
end = -1
while j < n:
    c = data[j:j+1]
    if c == b"{":
        depth += 1
    elif c == b"}":
        depth -= 1
        if depth == 0:
            end = j + 1
            break
    j += 1
if end < 0:
    print("FAIL [paint] no closing brace"); sys.exit(1)
old2 = data[i:end]

new2 = crlf("""        static void AiDesktopPaint(int w, int h) {
            AiEnsure();
            Gfx.Gradient(0, 0, w, h, 0x241A52u, 0x0B0B1Eu);
            int cx = w / 2, cy = (h * 22) / 100;
            int glow = h / 3;
            Gfx.FillCircle(cx, cy, glow, 0x2E2268u);
            Gfx.FillCircle(cx, cy, (glow * 72) / 100, 0x3A2C82u);
            int pw = AiPw(w), px = AiPx(w), py = AiPy(h), ph = AiPh(h);
            Gfx.Text(18, 14, "AI \\u684C\\u9762  \\u00B7  \\u865A\\u62DF\\u684C\\u9762 2  \\u00B7  \\u804A\\u5929", 0xC3B6FFu);
            Gfx.FillRound(px, py, pw, ph, 16, 0x14122Eu);
            Gfx.DrawRound(px, py, pw, ph, 16, 0x4A3FA0u);
            Gfx.FillRound(px, py, pw, 40, 16, 0x211D44u);
            Gfx.DrawRound(px, py, pw, 40, 16, 0x4A3FA0u);
            Gfx.Text(px + 16, py + 12, "NexOS AI \\u52A9\\u624B", 0xC3B6FFu);
            Gfx.Text(px + pw - 16 - Gfx.Measure("online"), py + 12, "online", 0x6FE0A0u);
            // History: newest at the bottom, just above the input row.
            int y = AiInY(h) - 14;
            int shown = 0;
            for (int k = aiHistN - 1; k >= 0 && shown < 16; k--) {
                int lines = AiDrawMsg(px + 16, y - 18, pw - 32, aiHist[k], 0xD8D2FFu);
                y -= lines * 18 + 6;
                shown++;
                if (y < py + 48) break;
            }
            if (aiHistN == 0)
                Gfx.Text(px + 16, py + 52, "\\u70B9\\u8F93\\u5165\\u6846\\u6253\\u5B57\\uFF0C\\u56DE\\u8F66\\u53D1\\u9001\\uFF1B\\u6211\\u4F1A\\u8C03\\u7528 agent run \\u540E\\u7AEF\\u3002", 0x8A7FD0u);
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
data = data[:i] + new2.encode("utf-8") + data[end:]
print("ok   [paint] patched (replaced %d bytes)" % len(old2))


# ---- 3) Route keys to the AI desktop when active ----
old3 = b"""            if (ch == -9) { if (CurrentDesktop == 0) SwitchDesktop(1); else SwitchDesktop(0); return; }
            if (renameIdx < 0) return;
"""
new3 = b"""            if (ch == -9) { if (CurrentDesktop == 0) SwitchDesktop(1); else SwitchDesktop(0); return; }
            if (CurrentDesktop == 1) { AiDesktopKey(ch); return; }
            if (renameIdx < 0) return;
"""
if data.count(old3) != 1:
    print("FAIL [key] matches=%d" % data.count(old3)); sys.exit(1)
data = data.replace(old3, new3)
print("ok   [key] patched")

with open(DESK, "wb") as f:
    f.write(data)
print("DESKTOP CHAT PANEL DONE")
