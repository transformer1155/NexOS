#if WINHOST
using System;
using System.Drawing;
using System.Windows.Forms;
using NexOS.Forms;
using NexOS.WinHost;

namespace WinHost
{
    // Standalone single-application host.
    //
    // The managed layer models every "application" as an App subclass opened
    // through Shell.Open(kind).  This host derives from ShellForm and reuses
    // its EXACT rendering + input pipeline, but shows ONLY one application
    // window - no desktop wallpaper/icons, taskbar, Start menu or tray.  The
    // result is each app packaged as a freestanding Windows .exe
    // (see csharp/apps/<Name>/).
    public sealed class AppHost : ShellForm
    {
        int myWin = -1;

        AppHost(int kind) : base(new string[0])
        {
            Text = "NexOS App";
            StartPosition = FormStartPosition.CenterScreen;

            // Boot the same way the full shell does.
            Login.BypassForHost();
            SeedSandbox();
            LoadTextures();

            // Open exactly one window and forget about the rest of the desktop.
            OpenKind(kind);
            myWin = wins.Count > 0 ? wins[wins.Count - 1].Id : -1;
            if (myWin < 0) { Text = "Failed to open app"; return; }

            var w = FindWin(myWin);
            if (w != null) ClientSize = new Size(w.W, w.H);
            Text = Shell.Title(myWin) ?? Text;
        }

        public static void Run(int kind)
        {
            Application.EnableVisualStyles();
            Application.SetHighDpiMode(HighDpiMode.SystemAware);
            Application.SetCompatibleTextRenderingDefault(false);
            using (var f = new AppHost(kind))
                Application.Run(f);
        }

        // ---- rendering: only our window, on a flat backdrop -------------
        protected override void OnPaint(PaintEventArgs e)
        {
            int W = ClientSize.Width, H = ClientSize.Height;
            Gfx.SetScreen(W, H);
            var p = PointToClient(Cursor.Position);
            Gfx.SetMouse(p.X, p.Y);

            // Flat backdrop behind the single window.
            Gfx.SetContext(e.Graphics, 0, 0, W, H);
            if (Gfx.HasImage(NexOS.Forms.Tex.WinBg) != 0)
                Gfx.Image(NexOS.Forms.Tex.WinBg, 0, 0, W, H);
            else
                Gfx.FillRect(0, 0, W, H, 0x202020);

            if (myWin >= 0)
            {
                var w = FindWin(myWin);
                if (w == null) { Close(); return; }
                // Keep the window filling the client area.
                w.X = 0; w.Y = 0; w.W = W; w.H = H;
                DrawChrome(e.Graphics, w);
                Rectangle cr = ClientRectOf(w);
                Gfx.SetContext(e.Graphics, cr.X, cr.Y, cr.Width, cr.Height);
                Shell.Paint(w.Id, cr.Width, cr.Height);
                e.Graphics.ResetTransform();
                e.Graphics.ResetClip();
            }
        }

        // ---- input: route purely to the single window -------------------
        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (myWin < 0) return;
            var w = FindWin(myWin);
            if (w == null) return;
            Rectangle cr = ClientRectOf(w);
            if (e.Y < w.Y + TitleH)
            {
                // Title-bar close button (top-right).
                int bx = w.X + w.W - BtnW * 1;
                if (e.X >= bx && e.X < bx + BtnW) { Close(); return; }
                return;
            }
            Gfx.SetMouse(e.X, e.Y);
            Gfx.SetContext(CreateGraphics(), cr.X, cr.Y, cr.Width, cr.Height);
            int b = (e.Button == MouseButtons.Right) ? 2
                  : (e.Button == MouseButtons.Middle) ? 1 : 0;
            if (b == 2) Shell.RightClick(w.Id, e.X - cr.X, e.Y - cr.Y, cr.X, cr.Y);
            else
            {
                Shell.MouseDown(w.Id, b, e.X - cr.X, e.Y - cr.Y);
                Shell.Click(w.Id, e.X - cr.X, e.Y - cr.Y);
            }
            Invalidate();
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (myWin < 0) return;
            var w = FindWin(myWin);
            if (w == null) return;
            Rectangle cr = ClientRectOf(w);
            int b = (e.Button == MouseButtons.Right) ? 2
                  : (e.Button == MouseButtons.Middle) ? 1 : 0;
            Gfx.SetContext(CreateGraphics(), cr.X, cr.Y, cr.Width, cr.Height);
            Shell.MouseUp(w.Id, b, e.X - cr.X, e.Y - cr.Y);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (myWin < 0) return;
            var w = FindWin(myWin);
            if (w == null) return;
            if (e.Y < w.Y + TitleH) return;
            Rectangle cr = ClientRectOf(w);
            Gfx.SetContext(CreateGraphics(), cr.X, cr.Y, cr.Width, cr.Height);
            Shell.MouseMove(w.Id, e.X - cr.X, e.Y - cr.Y);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            if (myWin < 0) return;
            var w = FindWin(myWin);
            if (w == null || e.Y < w.Y + TitleH) return;
            Rectangle cr = ClientRectOf(w);
            Gfx.SetContext(CreateGraphics(), cr.X, cr.Y, cr.Width, cr.Height);
            Shell.Wheel(w.Id, e.Delta);
            Invalidate();
        }

        protected override void OnKeyPress(KeyPressEventArgs e)
        {
            if (myWin < 0) return;
            if ((int)e.KeyChar < 32) { e.Handled = true; return; }
            Shell.Key(myWin, (int)e.KeyChar);
            Invalidate();
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (myWin < 0) return;
            int k = 0;
            bool ctrl = (e.Modifiers & Keys.Control) != 0;
            bool alt = (e.Modifiers & Keys.Alt) != 0;
            bool shift = (e.Modifiers & Keys.Shift) != 0;
            switch (e.KeyCode)
            {
                case Keys.Back: k = 8; break;
                case Keys.Tab: k = 9; break;
                case Keys.Enter: k = 13; break;
                case Keys.Escape: Close(); return;
                case Keys.Left: k = 0xFFFF - 1; break;
                case Keys.Right: k = 0xFFFF - 2; break;
                case Keys.Up: k = 0xFFFF - 3; break;
                case Keys.Down: k = 0xFFFF - 4; break;
                case Keys.Home: k = 0xFFFF - 5; break;
                case Keys.End: k = 0xFFFF - 6; break;
                case Keys.PageUp: k = 0xFFFF - 7; break;
                case Keys.PageDown: k = 0xFFFF - 8; break;
                case Keys.Delete: k = 0xFFFF - 9; break;
                case Keys.F1: k = 0xFFF1; break;
                case Keys.F2: k = 0xFFF2; break;
                case Keys.F3: k = 0xFFF3; break;
                case Keys.F4: k = 0xFFF4; break;
                case Keys.F5: k = 0xFFF5; break;
                case Keys.F6: k = 0xFFF6; break;
                case Keys.F7: k = 0xFFF7; break;
                case Keys.F8: k = 0xFFF8; break;
                case Keys.F9: k = 0xFFF9; break;
                case Keys.F10: k = 0xFFFA; break;
                case Keys.F11: k = 0xFFFB; break;
                case Keys.F12: k = 0xFFFC; break;
                default:
                    if (ctrl)
                    {
                        int ch = (int)e.KeyCode & 0x7F;
                        if (ch >= (int)'A' && ch <= (int)'Z')
                        { Shell.Key(myWin, 0x2000 | (ch - (int)'A' + 1)); Invalidate(); e.Handled = true; return; }
                    }
                    e.Handled = false; return;
            }
            int mods = (ctrl ? 1 : 0) | (alt ? 2 : 0) | (shift ? 4 : 0);
            Shell.Key(myWin, 0x1000 | (mods << 16) | k);
            Invalidate();
            e.Handled = true;
        }
    }
}
#endif
