// =====================================================================
//  Toast.cs - lightweight transient notifications (WinUI3-style).
//
//  A small stack of up to N slots pinned to the bottom-right, above the
//  taskbar.  Each toast slides in from the right (fade + 40px translate),
//  rests for its lifetime, then slides back out and frees its slot.  All
//  motion is driven through Anim so it rides the same frame loop as the
//  rest of the shell and never snaps.
//
//  No floats, no stdlib, and (on purpose) NO string==null comparisons:
//  this image runs on a minimal CLR whose BCL surface is tiny, so every
//  slot is tracked with plain int flags instead of null checks.
// =====================================================================
using System.Runtime.CompilerServices;

namespace NexOS.Forms
{
    public static class Toast
    {
        const int CAP = 3;
        const int TW = 280;       // toast width
        const int TH = 64;        // toast height
        const int GAP = 8;
        const int PAD = 14;       // slide-in distance (px)

        const int TOAST_KEY = 0x70A57EAF;   // base Anim key (slot i -> +i)

        static string[] title = new string[CAP];
        static string[] body  = new string[CAP];
        static int[]    t0    = new int[CAP];   // show start ms
        static int[]    life  = new int[CAP];   // total lifetime ms (>1 => occupied)
        static int[]    hasBody = new int[CAP]; // 1 => body text present
        static int      inited = 0;

        public static void Init()
        {
            if (inited == 1) return;
            for (int i = 0; i < CAP; i++) { title[i] = ""; body[i] = ""; life[i] = 1; hasBody[i] = 0; }
            inited = 1;
        }

        // Queue a notification.  `ms` is how long it stays before sliding out.
        public static void Show(string t, string b, int ms)
        {
            Init();
            int slot = -1;
            for (int i = 0; i < CAP; i++) { if (life[i] <= 1) { slot = i; break; } }
            if (slot < 0) { slot = 0; }                 // overwrite oldest
            title[slot] = t;
            if (b != null) { body[slot] = b; hasBody[slot] = 1; }
            else { body[slot] = ""; hasBody[slot] = 0; }
            t0[slot] = Host.TickMs();
            life[slot] = (ms < 600) ? 2600 : ms;
            Anim.Set(TOAST_KEY + slot, 1000, Anim.DUR_SLIDE, 1);  // slide in
        }

        public static void Show(string t, string b) { Show(t, b, 2600); }

        public static void Paint(int w, int h)
        {
            Init();
            int any = 0;
            int baseY = h - Desktop.TaskH - 16;
            for (int i = 0; i < CAP; i++)
            {
                if (life[i] <= 1) continue;             // empty / freed slot
                int now = Host.TickMs();
                int age = now - t0[i];
                // start the slide-out once the lifetime has elapsed
                if (age > life[i])
                    Anim.Set(TOAST_KEY + i, 0, Anim.DUR_SLIDE, 1);
                int d = (int)Anim.Get(TOAST_KEY + i);   // 0..1000 display amount
                if (d <= 0)
                {
                    if (age > life[i]) { life[i] = 1; title[i] = ""; body[i] = ""; hasBody[i] = 0; }
                    continue;
                }
                any = 1;

                int x = w - TW - 16 + ((1000 - d) * PAD) / 1000;  // slide from right
                int y = baseY - i * (TH + GAP);
                uint bg = U.Fade(Theme.Dark != 0 ? 0xE527272Bu : 0x00F7F9FCu, d);
                uint ed = U.Fade(0xFF000000u | (uint)(Theme.Dark != 0 ? 0x3A3A3Au : 0x00D5DDE8u), d);
                Gfx.FillRound(x, y, TW, TH, 8, bg);
                Gfx.DrawRound(x, y, TW, TH, 8, ed);
                Gfx.FillRound(x + 12, y + 14, 4, TH - 28, 2, Theme.Accent); // accent stripe
                if (d > 400)
                {
                    Gfx.Text(x + 24, y + 12, title[i], Theme.Dark != 0 ? 0xEAEAEAu : 0x1B1B1Bu);
                    if (hasBody[i] != 0)
                        Gfx.Text(x + 24, y + 32, body[i], Theme.Dark != 0 ? 0x9AA0A6u : 0x606060u);
                }
            }
            if (any != 0) Host.SetAnim(1);   // keep repainting while visible
        }
    }
}
