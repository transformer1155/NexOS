// =====================================================================
//  Demo.cs  -  "演示" window.
// ---------------------------------------------------------------------
//  A single button that, when clicked, shrinks to half its size in a
//  quick press animation, holds there for ~0.5 s (reply bubble "啊"),
//  then springs back to full size.  Pure managed code - no kernel
//  changes needed for the behaviour itself.
//
//  All phases are driven by Host.TickMs() (wall-clock milliseconds,
//  identical on the VM and the WinHost harness), so the animation
//  duration is frame-rate independent:
//    0 idle
//    1 press     100 -> 50 in PRESS_MS   (~0.1 s)
//    2 hold      stay at 50 for HOLD_MS  (~0.5 s), bubble shown
//    3 restore   50 -> 100 in RESTORE_MS (~0.15 s)
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class DemoApp : App
    {
        const int BTN_W = 200;     // button full width (client px)
        const int BTN_H = 60;      // button full height
        const int PRESS_MS   = 100;
        const int HOLD_MS    = 500;
        const int RESTORE_MS = 150;

        int btnX = 0, btnY = 0;    // last-drawn button origin (client coords)
        int scale = 100;           // button size %, 100 = full, 50 = half
        int animating = 0;         // 0 idle, 1 press, 2 hold, 3 restore
        int animStart = 0;         // ms anchor (Host.TickMs) for the current phase
        int reply = 0;             // 1 while held at half -> show "啊" bubble

        public override string GetTitle() { return "演示 Demo"; }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 18;

            Gfx.Text(pad, pad, "点击按钮演示", C.Text);
            Gfx.Text(pad, pad + 24, "点击缩到一半, 0.5秒后复原", C.TextSub);

            // Current (possibly scaled) button geometry, centred.
            int bw = BTN_W * scale / 100;
            int bh = BTN_H * scale / 100;
            btnX = (w - bw) / 2;
            btnY = pad + 80;

            uint fill = (animating != 0) ? 0x4A6FA5u : Theme.Accent;
            Gfx.FillRound(btnX, btnY, bw, bh, 10, fill);
            Gfx.DrawRound(btnX, btnY, bw, bh, 10, C.BorderMid);
            Gfx.TextCenter(btnX, btnY + bh / 2 - 6, bw, "点我", 0xFFFFFFFF);

            // Reply bubble, shown only while the button is held at half.
            if (reply != 0)
            {
                int bx = (w - 132) / 2;
                int by = btnY + bh + 22;
                Gfx.FillRound(bx, by, 132, 52, 14, 0xE3F2FD);
                Gfx.DrawRound(bx, by, 132, 52, 14, Theme.Accent);
                Gfx.TextCenter(bx, by + 20, 132, "啊", 0x0B5FA5);
                // little pointer under the bubble
                Gfx.FillRound(bx + 24, by + 50, 22, 6, 3, 0xE3F2FD);
            }

            // Advance the press animation (millisecond-driven, so the
            // durations hold on any frame rate - QEMU tcg included).
            if (animating == 1) {                    // press: 100 -> 50
                int e = Host.TickMs() - animStart;
                scale = 100 - 50 * e / PRESS_MS;
                if (scale <= 50) { scale = 50; animating = 2; animStart = Host.TickMs(); reply = 1; }
                Host.SetAnim(1);                     // keep repainting during anim
            } else if (animating == 2) {             // hold: stay at 50 for HOLD_MS
                if (Host.TickMs() - animStart >= HOLD_MS) { animating = 3; animStart = Host.TickMs(); reply = 0; }
                Host.SetAnim(1);
            } else if (animating == 3) {             // restore: 50 -> 100
                int e = Host.TickMs() - animStart;
                scale = 50 + 50 * e / RESTORE_MS;
                if (scale >= 100) { scale = 100; animating = 0; }
                Host.SetAnim(1);
            }
        }

        public override void OnClick(int mx, int my)
        {
            int bw = BTN_W * scale / 100;
            int bh = BTN_H * scale / 100;
            if (mx >= btnX && mx <= btnX + bw && my >= btnY && my <= btnY + bh)
            {
                scale = 100;       // restart from full size
                animating = 1;
                animStart = Host.TickMs();
                reply = 0;
            }
        }
    }
}
