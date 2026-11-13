// =====================================================================
//  BugReport.mex -- APMS bug-report application (standalone .mex)
// ---------------------------------------------------------------------
//  Lets the user describe a bug in their own words, capture the CURRENT
//  screen from the kernel framebuffer, and write a complete Markdown
//  report to the MKFS data disk.  The report carries:
//
//    * the user's description / reproduction steps
//    * a downscaled screenshot (apms_shot.ppm next to the report)
//    * kernel telemetry (memory, heap, last diagnostic step)
//    * the APMS event ring (faults, CLR faults, boot steps)
//    * the tail of the serial log
//
//  Run `python tools/apms_extract.py` on the host afterwards to pull
//  apms_report.md + apms_shot.png out of the data disk, then hand both
//  to the assistant.  Nothing here needs the desktop shell: it is a real
//  independent application loaded into its own MiniCLR slot.
// =====================================================================
using NexOS.Forms;

namespace App
{
    public class BugReportApp : NexOS.Forms.App
    {
        TBox box;
        string status;
        string titleBuf;
        int shotDone;
        int reportDone;
        int focus;
        int viewLine;
        int lastStatus;

        const int PAD = 16;

        public BugReportApp()
        {
            box = new TBox();
            box.text = "【出现情况】\n\n【复现步骤】\n1.\n2.\n\n【期望结果】\n\n【实际结果】\n";
            box.cursor = box.text.Length;
            box.selA = box.cursor;
            box.selB = box.cursor;
            status = "填写上面的情况，然后点「抓取当前屏幕」和「生成报告」。";
            titleBuf = "NexOS Bug 上报";
            shotDone = 0;
            reportDone = 0;
            focus = 1;
            viewLine = 0;
            lastStatus = 0;
        }

        // ---- layout ---------------------------------------------------
        int EditorY() { return 108; }
        int EditorH() { return Gfx.Height() - 108 - 150; }
        int EditorW() { return Gfx.Width() - 2 * PAD; }

        // Render one logical line of the document.  TBox already owns the
        // newline bookkeeping; we only slice, never rebuild the string.
        static string LineAt(TBox t, int i)
        {
            int a = t.LineStart(i);
            int b = (i + 1 < t.LineCount()) ? t.LineStart(i + 1) : t.text.Length;
            if (b > a && (int)t.text[b - 1] == 10) b--;
            if (b <= a) return "";
            return U.Sub(t.text, a, b - a);
        }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int bw = EditorW();

            W.Header(PAD, PAD, "Bug 上报 · APMS");
            W.Sub(PAD, PAD + 26, "描述出现情况与复现步骤，附当前屏幕截图，报告写到数据盘。");

            int ey = EditorY(), eh = EditorH();
            if (eh < 60) eh = 60;
            Gfx.FillRound(PAD, ey, bw, eh, 8, C.Card);
            Gfx.DrawRound(PAD, ey, bw, eh, 8, focus != 0 ? C.Accent : C.Border);

            int lines = box.LineCount();
            int first = viewLine;
            if (first > lines - 1) first = lines - 1;
            if (first < 0) first = 0;
            int maxLines = (eh - 12) / 16;
            for (int i = 0; i < maxLines; i++)
            {
                int li = first + i;
                if (li >= lines) break;
                string s = LineAt(box, li);
                if (s.Length > 68) s = U.Sub(s, 0, 68);
                Gfx.Text(PAD + 10, ey + 8 + i * 16, s, C.Text);
            }

            int by = ey + eh + 12;
            int third = (bw - 16) / 3;
            W.Primary(PAD, by, third, 38, "抓取当前屏幕");
            W.Button(PAD + third + 8, by, third, 38, "生成报告");
            W.Button(PAD + 2 * (third + 8), by, third, 38, "清空");

            int sy = by + 46;
            Gfx.FillRound(PAD, sy, bw, h - sy - PAD, 8, C.Card);
            Gfx.DrawRound(PAD, sy, bw, h - sy - PAD, 8, C.Border);
            Gfx.Text(PAD + 10, sy + 8, status, C.Text);

            int ev = Host.ApmsStatus();
            string head = U.Cat("APMS 事件数：", U.I(ev));
            if (shotDone != 0) head = U.Cat(head, "   · 截图已抓取");
            if (reportDone != 0) head = U.Cat(head, "   · 报告已写入数据盘");
            Gfx.Text(PAD + 10, sy + 28, head, C.TextSub);

            int show = ev; if (show > 3) show = 3;
            for (int i = 0; i < show; i++)
            {
                string e = Host.ApmsEvent(ev - 1 - i);
                if (e == null) continue;
                if (e.Length > 72) e = U.Sub(e, 0, 72);
                Gfx.Text(PAD + 10, sy + 46 + i * 16, e, C.TextSub);
            }
        }

        public override void OnClick(int mx, int my)
        {
            int bw = EditorW();
            int ey = EditorY(), eh = EditorH();
            if (eh < 60) eh = 60;
            if (U.In(mx, my, PAD, ey, bw, eh))
            {
                focus = 1;
                viewLine = (my - ey - 8) / 16;
                return;
            }

            int by = ey + eh + 12;
            int third = (bw - 16) / 3;
            if (U.In(mx, my, PAD, by, third, 38))
            {
                int n = Host.ApmsCapture(2);
                if (n > 0)
                {
                    shotDone = 1;
                    status = U.Cat("截图已抓取并写入 apms_shot.ppm（", U.I(n), " 字节）。");
                }
                else
                {
                    shotDone = 0;
                    status = U.Cat("截图失败，错误码 ", U.I(n), "，详见串口日志。");
                }
                return;
            }
            if (U.In(mx, my, PAD + third + 8, by, third, 38))
            {
                int n = Host.ApmsReport(box.text);
                if (n > 0)
                {
                    reportDone = 1;
                    status = U.Cat("报告已写入数据盘 apms_report.md（", U.I(n), " 字节）。");
                }
                else
                {
                    reportDone = 0;
                    status = U.Cat("报告写入失败，错误码 ", U.I(n), "。");
                }
                return;
            }
            if (U.In(mx, my, PAD + 2 * (third + 8), by, third, 38))
            {
                box.text = "";
                box.cursor = 0; box.selA = 0; box.selB = 0;
                focus = 1; viewLine = 0; shotDone = 0; reportDone = 0;
                status = "已清空。";
                return;
            }
        }

        public override void OnKey(int ch)
        {
            if (focus == 0) return;
            if (box.Key(ch))
            {
                int cur = box.LineOf(box.cursor);
                if (cur < viewLine) viewLine = cur;
                if (cur > viewLine + 6) viewLine = cur - 6;
            }
        }

        public override string GetTitle() { return titleBuf; }
    }

    // Standalone entry contract -- see mforms.cpp::slot_symbol().
    public static class Main
    {
        static BugReportApp app;

        public static void Init()
        {
            if (app == null) { app = new BugReportApp(); app.id = 0; app.KindId = 19; }
            NexOS.Forms.App.Current = app;
            NexOS.Forms.Anim.Init();
        }

        public static int Paint(int id, int w, int h)
        {
            Init(); app.id = id; app.OnPaint(); return 0;
        }

        public static int Click(int id, int x, int y)
        {
            Init(); app.id = id; app.OnClick(x, y); return 0;
        }

        public static int Key(int id, int ch)
        {
            Init(); app.id = id; app.OnKey(ch); return 0;
        }

        public static string Title(int id)
        {
            Init(); app.id = id; return app.GetTitle();
        }
    }
}
