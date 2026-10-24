// =====================================================================
//  WebView.cs - Linux Guest framebuffer viewer (VNC backend).
// ---------------------------------------------------------------------
//  NexOS.Forms.App that hosts the LinuxGuestFB shared framebuffer from
//  the VNC-connected Linux guest.  Mouse/keyboard input is forwarded to
//  the VNC input buffer via Host.LinuxInput.
//
//  MiniCLR limits obeyed:
//    * no try/catch, no generics/List<T>/interfaces/closures/floats
//    * no string.Substring (NexOS.Sys.StrSub only)
//    * NO "new" in OnPaint (heap rewinds each frame - allocations in Paint
//      exhaust the bump allocator instantly)
//    * static fields for persistent state
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public class WebViewApp : App
    {
        // ---- persistent state (static, survives across Paint/Paint) ----------
        int    state;       // 0 = loading, 1 = ready, 2 = error
        int    fbW, fbH;    // cached framebuffer dims (set by OnPaint)
        int    scrollX, scrollY; // scroll offset (for FB larger than window)
        int    lastDirtyX, lastDirtyY, lastDirtyW, lastDirtyH;

        // ---- layout ---------------------------------------------------------
        int HeaderH = 34;

        public WebViewApp()
        {
            state = 0;
            fbW = 0; fbH = 0;
            scrollX = 0; scrollY = 0;
            lastDirtyX = -1;
            // kick the linux_manager lifecycle — no-op if already running
            Host.LinuxStart("http://localhost");
        }

        public override string GetTitle() { return "Linux"; }

        public override void OnPaint()
        {
            int w = Gfx.Width(), h = Gfx.Height();
            // Window background (dark chrome)
            Gfx.FillRect(0, 0, w, h, Theme.Dark != 0 ? 0xFF18181B : 0xFFF7F9FC);

            int cx = 8, cy = HeaderH + 4, cw = w - 16, ch = h - HeaderH - 8;
            if (cw < 8 || ch < 8) return;

            if (state == 0)
            {
                // ---- loading placeholder -----------------------------------
                Gfx.FillRound(cx, cy, cw, ch, 6, 0xFF000000);
                Gfx.DrawRound(cx, cy, cw, ch, 6, 0xFF00CCCC);
                Gfx.Text(cx + 16, cy + ch / 2 - 8, "Connecting to Linux guest...", 0xFF00CCCC);
                Gfx.Text(cx + 16, cy + ch / 2 + 10, "linux_manager.start sent", 0xFF008888);
                // poll active state each frame
                if (Host.LinuxActive() != 0)
                {
                    // first frame ready — read dimensions
                    int ptr = Host.LinuxFb();
                    if (ptr != 0)
                    {
                        fbW = Host.LinuxFbW();
                        fbH = Host.LinuxFbH();
                        state = 1;
                    }
                }
                Host.SetAnim(1);  // keep repainting until state == 1
                return;
            }

            if (state == 1)
            {
                // ---- framebuffer ready --------------------------------------
                // re-poll fb ptr each frame (vnc may reconnect)
                int ptr = Host.LinuxFb();
                if (ptr == 0) { state = 0; Host.SetAnim(1); return; }

                // clamp scroll
                if (fbW > 0) {
                    if (scrollX < 0) scrollX = 0;
                    if (scrollY < 0) scrollY = 0;
                    int maxSX = (fbW > cw) ? (fbW - cw) : 0;
                    int maxSY = (fbH > ch) ? (fbH - ch) : 0;
                    if (scrollX > maxSX) scrollX = maxSX;
                    if (scrollY > maxSY) scrollY = maxSY;
                }

                // Host.LinuxBlit(dstX, dstY, dstW, dstH, srcX, srcY)
                int bw = cw, bh = ch;
                if (scrollX + bw > fbW) bw = fbW - scrollX;
                if (scrollY + bh > fbH) bh = fbH - scrollY;
                if (bw > 0 && bh > 0)
                {
                    Host.LinuxBlit(cx, cy, bw, bh, scrollX, scrollY);
                }

                // scroll hint (tiny corner badge) when content is larger
                if (fbW > cw || fbH > ch)
                {
                    Gfx.FillRound(cx + cw - 84, cy + ch - 18, 80, 14, 4, 0x80000000);
                    string hx = Host.Ticks() % 1000 < 500 ? "< " : "  <";
                    string hy = fbH > ch ? ((Host.Ticks() / 500) % 2 == 0 ? "^ " : " ^") : "";
                    string tip = U.Cat(U.Cat(hy, hx), "scroll");
                    Gfx.Text(cx + cw - 80, cy + ch - 15, tip, 0xFFCCCCCC);
                }
                return;
            }

            // ---- error state fallback ----------------------------------------
            Gfx.FillRect(cx, cy, cw, ch, 0xFF220000);
            Gfx.Text(cx + 12, cy + 12, "Linux guest unavailable", 0xFFFF8888);
            Gfx.Text(cx + 12, cy + 32, "Start QEMU#1 and vnc_client plugin", 0xFFFFAAAA);
        }

        // ---- mouse forwarding ------------------------------------------------
        public override void OnMouseDown(int btn, int mx, int my)
        {
            if (state != 1) return;
            int tx = mx - 8 + scrollX;
            int ty = my - HeaderH - 4 + scrollY;
            Host.LinuxInput(tx, ty, btn, 0);
        }
        public override void OnMouseUp(int btn, int mx, int my)
        {
            if (state != 1) return;
            int tx = mx - 8 + scrollX;
            int ty = my - HeaderH - 4 + scrollY;
            Host.LinuxInput(tx, ty, 0, 0);
        }
        public override void OnMouseMove(int mx, int my)
        {
            if (state != 1) return;
            int tx = mx - 8 + scrollX;
            int ty = my - HeaderH - 4 + scrollY;
            Host.LinuxInput(tx, ty, 0xFFFF, 0);   // buttons=-1 = move event
        }

        // ---- scroll wheel (navigate the framebuffer) ------------------------
        public override void OnWheel(int dy)
        {
            if (state != 1) return;
            scrollY += dy > 0 ? -20 : 20;
            Host.SetAnim(1);
        }

        // ---- keyboard passthrough -------------------------------------------
        public override void OnKey(int ch)
        {
            if (state != 1) return;
            // key codes: MiniCLR passes raw VK / printable char.
            // 27=ESC, 9=Tab, 8=Backspace, \n=Enter, arrows = -1..-4
            Host.LinuxInput(0, 0, 0, ch);
        }
    }
}
