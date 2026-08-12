// =====================================================================
//  Popup.cs  -  a software-rendered context menu, shared by the kernel
// ---------------------------------------------------------------------
//  The WinHost build draws its right-click menus with WinForms
//  ContextMenuStrip; that API does not exist inside the MiniCLR VM, so
//  the kernel needs a menu it can paint itself with the Gfx primitives.
//
//  This is that menu.  It is deliberately delegate-free (MiniCLR has no
//  closures / function pointers): each item carries an integer action
//  code and the owner dispatches on it with a plain switch.  Arrays are
//  pre-sized and copied in Open() so the rewinding heap can never leave
//  a dangling reference between frames.
//
//  Interpreter rules obeyed: no static initialisers, no floats, no
//  generics, no delegates.  Everything is plain int/string/arrays.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public static class Popup
    {
        const int    CAP   = 24;        // max items per menu
        const int    ItemH = 34;        // row height
        const int    PadY  = 6;         // top/bottom inner padding
        const uint   BG    = 0xF7F9FC;  // menu surface (light)
        const uint   Edge  = 0xD5DDE8;  // hairline border
        const uint   Ink   = 0x1B1B1B;  // label ink
        const uint   Sub   = 0x606060;  // secondary / prefix
        const uint   Hover = 0xE7EEF8;  // row hover fill
        const uint   Danger= 0xC42B1C;  // destructive action ink

        // Action code 0x40000000 marks a destructive (red) row.
        public const int DangerBit = 0x40000000;

        static string[] s_lab;
        static int[]    s_act;
        static int      n;
        static int      x, y, w;        // current rect
        static bool     open;
        static int      owner;          // who opened it (Desktop.* codes)

        public static void Init()
        {
            s_lab = new string[CAP];
            s_act = new int[CAP];
            n = 0;
            open = false;
            owner = 0;
        }

        public static bool IsOpen() { return open; }
        public static void Close()  { open = false; }
        public static int  Owner()  { return owner; }

        // Build a menu from parallel label / action arrays.  `owner` lets
        // the click handler know which surface spawned it.  Position is
        // clamped to the screen so the menu never runs off-edge.
        public static void Open(int o, int px, int py, string[] labs, int[] codes, int count)
        {
            if (count > CAP) count = CAP;
            int mw = 0;
            for (int i = 0; i < count; i++)
            {
                s_lab[i] = labs[i];
                s_act[i] = codes[i];
                if (codes[i] != -1)            // -1 == separator
                {
                    int m = Gfx.Measure(labs[i]);
                    if (m > mw) mw = m;
                }
            }
            n = count;
            owner = o;
            w = mw + 40;
            if (w < 150) w = 150;
            int h = n * ItemH + PadY * 2;
            // Clamp against the SCREEN, not the current client context:
            // Gfx.Width/Height report the window's client size while a
            // popup opens from a right-click, which would squash the menu
            // into (often negative) window coordinates.
            int W = Gfx.ScreenW(), H = Gfx.ScreenH();
            x = px; if (x < 4) x = 4; if (x + w > W - 4) x = W - 4 - w;
            y = py; if (y < 4) y = 4; if (y + h > H - 4) y = H - 4 - h;
            open = true;
        }

        // Which item is under the cursor.  Returns the action code, or
        // -1 if the click fell outside the menu box (caller dismisses),
        // or -2 if it landed inside but on a separator / gap (no-op).
        public static int Hit(int mx, int my, int W, int H)
        {
            if (!open) return -1;
            int mh = n * ItemH + PadY * 2;
            if (mx < x || mx >= x + w || my < y || my >= y + mh) return -1;
            int idx = (my - y - PadY) / ItemH;
            if (idx < 0 || idx >= n) return -2;
            if (s_act[idx] == -1) return -2;     // separator
            return s_act[idx];
        }

        public static void Paint(int W, int H)
        {
            if (!open) return;
            int mh = n * ItemH + PadY * 2;
            Gfx.FillRound(x, y, w, mh, 8, BG);
            if (Gfx.HasImage(Tex.Menu) != 0)
                Gfx.Image(Tex.Menu, x + 2, y + 2, w - 4, mh - 4);
            Gfx.DrawRound(x, y, w, mh, 8, Edge);

            int mx = Gfx.MouseX(), my = Gfx.MouseY();
            int hy = -1;
            if (mx >= x && mx < x + w && my >= y && my < y + mh)
                hy = (my - y - PadY) / ItemH;

            for (int i = 0; i < n; i++)
            {
                int iy = y + PadY + i * ItemH;
                if (s_act[i] == -1)             // separator
                {
                    Gfx.DrawLine(x + 10, iy + ItemH / 2, x + w - 10, iy + ItemH / 2, Edge);
                    continue;
                }
                if (i == hy)
                    Gfx.FillRound(x + 4, iy + 1, w - 8, ItemH - 2, 4, Hover);
                bool danger = (s_act[i] & DangerBit) != 0;
                uint col = danger ? Danger : Ink;
                Gfx.Text(x + 16, iy + (ItemH - 16) / 2, s_lab[i], col);
            }
        }
    }
}
