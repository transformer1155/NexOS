// =====================================================================
//  Apps.cs  -  the built-in applications, rewritten in C#
// ---------------------------------------------------------------------
//  Every window NexOS shipped as hand-drawn C++ now lives here as a
//  managed App subclass: calculator, about, task manager, file explorer,
//  control panel, terminal, memory optimiser, notepad.  Each one draws
//  in immediate mode (OnPaint calls Gfx directly) and hit-tests clicks
//  with U.In, so nothing survives a frame except the fields it declares.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    // =================================================================
    //  Calculator
    // =================================================================
    public class CalculatorApp : App
    {
        int disp;      // number currently shown
        int acc;       // left operand held while an operator is pending
        int op;        // 0 none, 1 +, 2 -, 3 *, 4 /
        bool fresh;    // next digit starts a new number
        bool err;      // divide-by-zero latch

        public CalculatorApp() { disp = 0; acc = 0; op = 0; fresh = true; err = false; }

        public override string GetTitle() { return "Calculator"; }

        // 4 columns x 5 rows keypad below the display.
        static string Label(int r, int c)
        {
            if (r == 0) { if (c == 0) return "C"; if (c == 1) return "<"; if (c == 2) return "%"; return "/"; }
            if (r == 1) { if (c == 0) return "7"; if (c == 1) return "8"; if (c == 2) return "9"; return "*"; }
            if (r == 2) { if (c == 0) return "4"; if (c == 1) return "5"; if (c == 2) return "6"; return "-"; }
            if (r == 3) { if (c == 0) return "1"; if (c == 1) return "2"; if (c == 2) return "3"; return "+"; }
            if (c == 0) return "0"; if (c == 1) return "00"; if (c == 2) return "+-"; return "=";
        }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 14;
            int dispH = 76;

            // Display panel.
            Gfx.FillRound(pad, pad, w - 2 * pad, dispH, 8, 0x111827);
            string shown = err ? "Error" : U.I(disp);
            int tw = Gfx.Measure(shown) * 2;   // draw big-ish via right align
            Gfx.Text(w - pad - 14 - Gfx.Measure(shown), pad + dispH - 30, shown, 0xFFFFFF);
            if (op != 0 && !err)
                Gfx.Text(pad + 14, pad + 14, U.Cat(U.I(acc), OpSym(op)), 0x7DD3FC);

            // Keypad.
            int gx = pad, gy = pad + dispH + 12;
            int gw = w - 2 * pad, gh = h - gy - pad;
            int cw = (gw - 3 * 8) / 4;
            int ch = (gh - 4 * 8) / 5;
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 4; c++)
                {
                    int x = gx + c * (cw + 8);
                    int y = gy + r * (ch + 8);
                    bool accent = (c == 3) || (r == 0 && c == 0);   // ops + clear
                    W.Key(x, y, cw, ch, Label(r, c), accent);
                }
        }

        static string OpSym(int o)
        {
            if (o == 1) return " +"; if (o == 2) return " -";
            if (o == 3) return " *"; if (o == 4) return " /"; return "";
        }

        public override void OnClick(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 14, dispH = 76;
            int gx = pad, gy = pad + dispH + 12;
            int gw = w - 2 * pad, gh = h - gy - pad;
            int cw = (gw - 3 * 8) / 4;
            int ch = (gh - 4 * 8) / 5;
            for (int r = 0; r < 5; r++)
                for (int c = 0; c < 4; c++)
                {
                    int x = gx + c * (cw + 8);
                    int y = gy + r * (ch + 8);
                    if (U.In(mx, my, x, y, cw, ch)) { Press(Label(r, c)); return; }
                }
        }

        public override void OnKey(int ch)
        {
            if (ch >= '0' && ch <= '9') { Digit(ch - '0'); return; }
            if (ch == '+') { SetOp(1); return; }
            if (ch == '-') { SetOp(2); return; }
            if (ch == '*') { SetOp(3); return; }
            if (ch == '/') { SetOp(4); return; }
            if (ch == '=' || ch == -2) { Equals(); return; }   // -2 = enter
            if (ch == -1 || ch == 8) { Back(); return; }       // -1 = backspace
            if (ch == 'c' || ch == 'C' || ch == -7) { Clear(); return; }
        }

        void Press(string k)
        {
            if (k == "C") { Clear(); return; }
            if (k == "<") { Back(); return; }
            if (k == "=") { Equals(); return; }
            if (k == "+") { SetOp(1); return; }
            if (k == "-") { SetOp(2); return; }
            if (k == "*") { SetOp(3); return; }
            if (k == "/") { SetOp(4); return; }
            if (k == "%") { if (!err) disp = disp / 100; return; }
            if (k == "+-") { if (!err) disp = -disp; return; }
            if (k == "00") { Digit(0); Digit(0); return; }
            if (k == "0") { Digit(0); return; }
            if (k == "1") { Digit(1); return; } if (k == "2") { Digit(2); return; }
            if (k == "3") { Digit(3); return; } if (k == "4") { Digit(4); return; }
            if (k == "5") { Digit(5); return; } if (k == "6") { Digit(6); return; }
            if (k == "7") { Digit(7); return; } if (k == "8") { Digit(8); return; }
            if (k == "9") { Digit(9); return; }
        }

        void Digit(int d)
        {
            if (err) return;
            if (fresh) { disp = d; fresh = false; }
            else if (disp < 200000000 && disp > -200000000) disp = disp * 10 + (disp < 0 ? -d : d);
        }

        void SetOp(int o)
        {
            if (err) return;
            if (op != 0 && !fresh) Equals();
            acc = disp; op = o; fresh = true;
        }

        void Equals()
        {
            if (err || op == 0) return;
            if (op == 1) disp = acc + disp;
            else if (op == 2) disp = acc - disp;
            else if (op == 3) disp = acc * disp;
            else if (op == 4) { if (disp == 0) { err = true; } else disp = acc / disp; }
            op = 0; fresh = true;
        }

        void Back() { if (!err) disp = disp / 10; }
        void Clear() { disp = 0; acc = 0; op = 0; fresh = true; err = false; }
    }

    // =================================================================
    //  About
    // =================================================================
    public class AboutApp : App
    {
        public override string GetTitle() { return "About NexOS"; }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width();
            int pad = 18;

            // Hero card.
            Gfx.Gradient(pad, pad, w - 2 * pad, 92, 0x0B5FA5, 0x0078D4);
            Gfx.FillRound(pad + 18, pad + 22, 48, 48, 8, 0xFFFFFF);
            Gfx.TextCenter(pad + 18, pad + 38, 48, "OS", 0x0078D4);
            Gfx.Text(pad + 80, pad + 26, Host.OsName(), 0xFFFFFF);
            Gfx.Text(pad + 80, pad + 50, "A .NET-powered operating system", 0xCDE7FB);

            // Specs card.
            int cy = pad + 104;
            W.Card(pad, cy, w - 2 * pad, 196);
            int lx = pad + 20, rx = w - pad - 20;
            int y = cy + 18;
            Kv(lx, rx, y, "Edition", Host.Is64Bit() != 0 ? "64-bit" : "32-bit"); y += 28;
            Kv(lx, rx, y, "Processor", Host.CpuVendor()); y += 28;
            Kv(lx, rx, y, "Memory", U.Mb(Host.MemTotalKb())); y += 28;
            Kv(lx, rx, y, "Disk", U.Cat(U.I(Host.DiskSizeMb()), " MB")); y += 28;
            Kv(lx, rx, y, "PCI devices", U.I(Host.PciCount())); y += 28;
            Kv(lx, rx, y, "Network", Host.NicPresent() != 0 ? "Connected" : "None");
        }

        static void Kv(int lx, int rx, int y, string k, string v)
        {
            Gfx.Text(lx, y, k, C.TextSub);
            Gfx.Text(rx - Gfx.Measure(v), y, v, C.Text);
        }
    }

    // =================================================================
    //  Task Manager (performance view)
    // =================================================================
    public class TaskManagerApp : App
    {
        public override string GetTitle() { return "Task Manager"; }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width();
            int pad = 16;

            W.Header(pad, pad, "Performance");

            int used = Host.PagesUsed(), total = Host.PagesTotal();
            int memPct = total > 0 ? used * 100 / total : 0;
            int heapPct = Host.HeapAlloc() * 100 / (512 * 1024);
            if (heapPct > 100) heapPct = 100;

            int cy = pad + 34;
            W.Card(pad, cy, w - 2 * pad, 150);
            int mx = pad + 20, mw = w - 2 * pad - 40;
            W.Meter(mx, cy + 18, mw, "Physical memory", memPct, C.Accent);
            W.Meter(mx, cy + 58, mw, "Managed heap (CLR)", heapPct, C.Good);
            Gfx.Text(mx, cy + 100, U.Cat("Pages ", U.I(used), " / ", U.I(total)), C.TextSub);
            Gfx.Text(mx, cy + 122, U.Cat("Heap objects: alloc ", U.I(Host.HeapAllocCnt()),
                                         "  free ", U.I(Host.HeapFreeCnt())), C.TextSub);

            // Process list -- the resident subsystems.
            int py = cy + 166;
            W.Header(pad, py, "Processes");
            py += 30;
            Proc(pad, py, w, "System (kernel)", "Running", 0); py += W.RowH;
            Proc(pad, py, w, "NexOS.Forms Shell", "Running", 1); py += W.RowH;
            Proc(pad, py, w, "MiniCLR runtime", "Running", 0); py += W.RowH;
            Proc(pad, py, w, "Desktop compositor", "Running", 1);
        }

        static void Proc(int x, int y, int w, string name, string state, int alt)
        {
            if (alt != 0) Gfx.FillRound(x, y, w - 2 * x, W.RowH - 2, 6, C.CardAlt);
            Gfx.Text(x + 14, y + 8, name, C.Text);
            Gfx.Text(x + w - x - 90, y + 8, state, C.Good);
        }
    }

    // =================================================================
    //  File Explorer
    // =================================================================
    public class FileExplorerApp : App
    {
        int fs;        // 0 = MKFS, 1 = SFS
        int sel;       // selected index, -1 none
        int scroll;    // first visible row
        int editMode;  // 0 none, 1 rename, 2 new folder
        string editBuf;// current text in the inline editor
        string editOld;// original name (rename source)
        int dblT;      // TickMs() of the last row click (double-click detect)
        int dblIdx;    // row index of that click
        static string clip;   // last "copied" path

        public FileExplorerApp() { fs = 0; sel = -1; scroll = 0; editMode = 0; editBuf = ""; editOld = ""; dblT = -100000; dblIdx = -1; }

        public override string GetTitle() { return "File Explorer"; }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int navW = 150, pad = 12;

            // Left navigation.
            Gfx.FillRect(0, 0, navW, h, 0xFAFAFA);
            Gfx.DrawLine(navW, 0, navW, h, C.Border);
            Nav(10, 14, navW - 20, "This PC", false);
            Nav(10, 14 + 40, navW - 20, "Local Disk (MKFS)", fs == 0);
            Nav(10, 14 + 80, navW - 20, "System (SFS)", fs == 1);

            // File area header.
            int ax = navW + pad, ay = pad;
            int aw = w - ax - pad;
            int n = Host.FileCount(fs);
            Gfx.Text(ax, ay, fs == 0 ? "Local Disk (MKFS)" : "System (SFS)", C.Text);
            Gfx.Text(ax, ay + 20, U.Cat(U.I(n), " items"), C.TextSub);

            int ly = ay + 48;
            int rows = (h - ly - pad) / W.RowH;
            for (int i = 0; i < rows; i++)
            {
                int idx = scroll + i;
                if (idx >= n) break;
                int y = ly + i * W.RowH;
                bool isDir = Host.FileIsDir(fs, idx) != 0;
                string nm = Host.FileName(fs, idx);
                if (idx == sel) Gfx.FillRound(ax, y, aw, W.RowH - 2, 6, C.Sel);
                else if (W.Hot(ax, y, aw, W.RowH - 2)) Gfx.FillRound(ax, y, aw, W.RowH - 2, 6, C.Hover);
                Gfx.Icon(ax + 8, y + 4, 22, isDir ? 0xF7C948u : 0x60A5FAu, isDir ? 'D' : 'F', 0xFFFFFF);
                Gfx.Text(ax + 40, y + 8, nm, C.Text);
            }

            // Inline rename / new-folder editor.
            if (editMode != 0)
            {
                int inputY = ly;
                if (editMode == 1 && sel >= 0)
                {
                    int ri = sel - scroll;
                    if (ri >= 0 && ri < rows) inputY = ly + ri * W.RowH;
                }
                int bx = ax + 4, bw = aw - 8, bh = W.RowH - 2;
                Gfx.FillRound(bx, inputY, bw, bh, 4, 0xFFFFFFFF);
                Gfx.DrawRound(bx, inputY, bw, bh, 4, C.Accent);
                string shown = editBuf;
                if ((Host.Ticks() / 30) % 2 == 0) shown = U.Cat(shown, "|");
                Gfx.Text(bx + 6, inputY + 6, shown, C.Text);
            }
        }

        static void Nav(int x, int y, int w, string label, bool active)
        {
            if (active) Gfx.FillRound(x, y, w, 30, 6, C.Sel);
            else if (W.Hot(x, y, w, 30)) Gfx.FillRound(x, y, w, 30, 6, C.Hover);
            Gfx.Text(x + 12, y + 7, label, active ? C.Accent : C.Text);
        }

        void SelectAt(int mx, int my)
        {
            int navW = 150, pad = 12;
            int ax = navW + pad, ay = pad, aw = Gfx.Width() - ax - pad;
            int ly = ay + 48;
            int rows = (Gfx.Height() - ly - pad) / W.RowH;
            int n = Host.FileCount(fs);
            for (int i = 0; i < rows; i++)
            {
                int idx = scroll + i;
                if (idx >= n) break;
                int y = ly + i * W.RowH;
                if (U.In(mx, my, ax, y, aw, W.RowH - 2)) { sel = idx; return; }
            }
        }

        public override void OnClick(int mx, int my)
        {
            // While editing, a click outside the box commits the change.
            if (editMode != 0)
            {
                int enavW = 150, epad = 12;
                int eax = enavW + epad, eay = epad, eaw = Gfx.Width() - eax - epad;
                int ely = eay + 48;
                int erows = (Gfx.Height() - ely - epad) / W.RowH;
                int inputY = ely;
                if (editMode == 1 && sel >= 0)
                {
                    int ri = sel - scroll;
                    if (ri >= 0 && ri < erows) inputY = ely + ri * W.RowH;
                }
                if (!U.In(mx, my, eax + 4, inputY, eaw - 8, W.RowH - 2)) CommitEdit();
                return;
            }
            int navW2 = 150, pad2 = 12;
            if (U.In(mx, my, 10, 14 + 40, navW2 - 20, 30)) { fs = 0; sel = -1; scroll = 0; Host.FileRefresh(); return; }
            if (U.In(mx, my, 10, 14 + 80, navW2 - 20, 30)) { fs = 1; sel = -1; scroll = 0; Host.FileRefresh(); return; }

            int ax = navW2 + pad2, ay = pad2, aw = Gfx.Width() - ax - pad2;
            int ly = ay + 48;
            int rows = (Gfx.Height() - ly - pad2) / W.RowH;
            int n = Host.FileCount(fs);
            for (int i = 0; i < rows; i++)
            {
                int idx = scroll + i;
                if (idx >= n) break;
                int y = ly + i * W.RowH;
                if (U.In(mx, my, ax, y, aw, W.RowH - 2))
                {
                    // Single click selects; a second click on the same row
                    // within 500 ms opens it with the default handler
                    // (Notepad for files), like Windows Explorer.
                    int now = Host.TickMs();
                    bool dbl = (now - dblT) < 500 && dblIdx == idx;
                    dblT = now; dblIdx = idx;
                    sel = idx;
                    if (dbl) OpenSelected();
                    return;
                }
            }
        }

        public override void OnKey(int ch)
        {
            if (editMode != 0)
            {
                if (ch == 27) { CancelEdit(); return; }                 // Esc
                if (ch == 10 || ch == 13) { CommitEdit(); return; }     // Enter
                if (ch == 8) {                                           // Backspace
                    int m = editBuf.Length;
                    if (m > 0) { string r = ""; for (int i = 0; i < m - 1; i++) r = U.Cat(r, Host.CharStr((int)editBuf[i])); editBuf = r; }
                    return;
                }
                if ((ch >= 0x20 && ch < 0x7F) || (ch >= 0x80 && ch <= 0xFFFF)) { editBuf = U.Cat(editBuf, Host.CharStr(ch)); return; }
                return;
            }
            int n = Host.FileCount(fs);
            if (ch == -4) { if (sel < n - 1) sel++; }        // down
            else if (ch == -3) { if (sel > 0) sel--; }       // up
        }

        public override void OnRightClick(int mx, int my, int ox, int oy)
        {
            dblT = -100000; dblIdx = -1;   // a right-click never starts a double-click
            int navW = 150, pad = 12;
            int ax = navW + pad, ly = pad + 48;
            int rows = (Gfx.Height() - ly - pad) / W.RowH;
            // Inside the file-list client area -> the file action menu.
            if (mx >= ax && my >= ly && my < ly + rows * W.RowH)
            {
                SelectAt(mx, my);             // right-click also selects
                if (sel >= 0 && sel < Host.FileCount(fs))
                    Desktop.OpenFileMenu(id, fs, sel, ox + mx, oy + my);
                return;
            }
            // Nav buttons / address bar / empty space -> generic menu.
            base.OnRightClick(mx, my, ox, oy);
        }

        // ---- file-action dispatch (from the context menu) -----------
        public override void DoFileAction(int code)
        {
            if (code == Desktop.A_F_OPEN)        OpenSelected();   // .exe -> run, else Notepad
            else if (code == Desktop.A_F_EDIT)   OpenInNotepad();  // "Edit" is always the text viewer
            else if (code == Desktop.A_F_NOTEPAD) OpenInNotepad(); // "Open with... > Notepad"
            else if (code == Desktop.A_F_TERM)   Host.OpenApp(Kind.Terminal);
            else if (code == Desktop.A_F_COPY)   CopySelected();
            else if (code == Desktop.A_F_DEL)    DeleteSelected();
            else if (code == Desktop.A_F_RENAME) BeginRename();
            else if (code == Desktop.A_F_PROPS)  ShowProps();
            else if (code == Desktop.A_F_MKDIR)  BeginNewFolder();
        }

        // Default action for a double-click / "Open".  Windows Explorer
        // semantics: a .exe is a PROGRAM, so it is EXECUTED by the kernel's
        // PE loader (the same path as `winapp foo.exe`); anything else is a
        // document and opens in Notepad.  A .exe never lands in Notepad
        // unless the user explicitly picks "Open with... > Notepad".
        void OpenSelected()
        {
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            if (Host.FileIsDir(fs, sel) != 0) return;     // dirs: no viewer
            string nm = Host.FileName(fs, sel);
            if (U.IsExe(nm)) { RunExe(nm); return; }
            Shell.OpenNotepad(nm);
        }

        // Execute a .exe through the PE loader.  On failure we report the
        // loader's error code in a message box rather than silently falling
        // back to Notepad -- an executable is not text.
        void RunExe(string nm)
        {
            Host.Log(U.Cat("[FILES] running PE image ", nm));
            int rc = Host.RunExe(nm);
            if (rc >= 0) return;
            string why = "The image could not be loaded.";
            if (rc == -1) why = "File not found.";
            else if (rc == -2) why = "Not a PE executable (no MZ/PE header).";
            else if (rc == -3) why = "Unsupported PE: 64-bit needs 'switch64'; .NET not runnable.";
            else if (rc == -4) why = "Out of memory while mapping the image.";
            else if (rc == -5) why = "Unresolved imports - API not implemented.";
            else if (rc == -6) why = "Image too large for the loader (192 KiB limit).";
            string[] labs = new string[3];
            int[]    acts = new int[3];
            labs[0] = U.Cat("Cannot run ", nm);  acts[0] = Desktop.A_F_PROPS;
            labs[1] = why;                       acts[1] = Desktop.A_F_PROPS;
            labs[2] = "Close";                   acts[2] = Desktop.A_F_PROPS;
            Popup.Open(Desktop.OWNER_FILE, Gfx.Width() / 2 - 130, Gfx.Height() / 2 - 60, labs, acts, 3);
        }

        // "Open with... > Notepad": force the text viewer even for a .exe.
        void OpenInNotepad()
        {
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            if (Host.FileIsDir(fs, sel) != 0) return;
            Shell.OpenNotepad(Host.FileName(fs, sel));
        }
        void CopySelected()
        {
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            clip = Host.FileName(fs, sel);
        }
        void DeleteSelected()
        {
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            Host.FileDelete(fs, Host.FileName(fs, sel));
            Host.FileRefresh();
            sel = -1;
        }
        void BeginRename()
        {
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            editOld = Host.FileName(fs, sel);
            editBuf = editOld;
            editMode = 1;
        }
        void BeginNewFolder() { editBuf = ""; editMode = 2; }
        void CommitEdit()
        {
            if (editMode == 1 && editBuf.Length > 0 && editBuf != editOld)
                Host.FileRename(fs, editOld, editBuf);
            else if (editMode == 2 && editBuf.Length > 0)
                Host.FileMkDir(fs, editBuf);
            if (editMode != 0) Host.FileRefresh();
            editMode = 0; editBuf = "";
        }
        void CancelEdit() { editMode = 0; editBuf = ""; }

        void ShowProps()
        {
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            string nm = Host.FileName(fs, sel);
            string ty = Host.FileIsDir(fs, sel) != 0 ? "Folder" : "File";
            string loc = fs == 0 ? "Local Disk (MKFS)" : "System (SFS)";
            string[] labs = new string[4];
            int[]    acts = new int[4];
            labs[0] = U.Cat("Name:    ", nm); acts[0] = Desktop.A_F_PROPS;
            labs[1] = U.Cat("Type:    ", ty); acts[1] = Desktop.A_F_PROPS;
            labs[2] = U.Cat("Location:", loc); acts[2] = Desktop.A_F_PROPS;
            labs[3] = "Close";                  acts[3] = Desktop.A_F_PROPS;
            // Screen centre so the dialog is easy to find.
            Popup.Open(Desktop.OWNER_FILE, Gfx.Width() / 2 - 90, Gfx.Height() / 2 - 70, labs, acts, 4);
        }

        // Context-menu hooks: what is currently selected in this browser.
        public override string SelectedFile()
        { return sel >= 0 && sel < Host.FileCount(fs) ? Host.FileName(fs, sel) : ""; }
        public override int SelectedFs()   { return fs; }
        public override int SelectedIsDir()
        { return sel >= 0 && sel < Host.FileCount(fs) ? Host.FileIsDir(fs, sel) : 0; }
    }

    // =================================================================
    //  Control Panel
    // =================================================================
    public class ControlPanelApp : App
    {
        int page;   // -1 tiles, 0 System, 1 Power, 2 Display, 3 Network,
                    // 4 Storage, 5 Devices, 6 Personalize, 7 Taskbar
        const int SW = 92, SGap = 12, SCols = 6;   // swatch grid

        public ControlPanelApp()
        {
            int p = Shell.TakeSettingsPage();
            page = p < 0 ? -1 : p;
        }

        int SwX(int baseX, int i) { return baseX + (i % SCols) * (SW + SGap); }
        int SwY(int baseY, int i) { return baseY + (i / SCols) * 56; }
        bool Back(int mx, int my) { return U.In(mx, my, 16, 16, 80, 20); }

        public override string GetTitle() { return "Control Panel"; }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width();
            int pad = 16;
            if (page == 0) { System(pad, w); return; }
            if (page == 1) { Power(pad, w); return; }
            if (page == 2) { Display(pad, w); return; }
            if (page == 3) { Network(pad, w); return; }
            if (page == 4) { Storage(pad, w); return; }
            if (page == 5) { Devices(pad, w); return; }
            if (page == 6) { Personalize(pad, w); return; }
            if (page == 7) { TaskbarPage(pad, w); return; }

            W.Header(pad, pad, "All Control Panel Items");
            int gy = pad + 36, gx = pad;
            int cols = 3;
            int cw = (w - 2 * pad - (cols - 1) * 12) / cols;
            int chh = 84;
            for (int i = 0; i < 6; i++)
            {
                int r = i / cols, c = i % cols;
                int x = gx + c * (cw + 12);
                int y = gy + r * (chh + 12);
                Tile(x, y, cw, chh, TileLetter(i), TileName(i), TileColor(i));
            }
        }

        static string TileName(int i)
        {
            if (i == 0) return "System"; if (i == 1) return "Display";
            if (i == 2) return "Network"; if (i == 3) return "Storage";
            if (i == 4) return "Devices"; return "Power";
        }
        static int TileLetter(int i)
        {
            if (i == 0) return 'S'; if (i == 1) return 'D'; if (i == 2) return 'N';
            if (i == 3) return 'H'; if (i == 4) return 'V'; return 'P';
        }
        static uint TileColor(int i)
        {
            if (i == 0) return 0x0078D4; if (i == 1) return 0x8B5CF6; if (i == 2) return 0x10B981;
            if (i == 3) return 0xF59E0B; if (i == 4) return 0x06B6D4; return 0xEF4444;
        }

        static void Tile(int x, int y, int w, int h, int letter, string name, uint col)
        {
            uint bg = W.Hot(x, y, w, h) ? C.Hover : C.Card;
            Gfx.FillRound(x, y, w, h, 8, bg);
            Gfx.DrawRound(x, y, w, h, 8, C.Border);
            Gfx.Icon(x + 14, y + 16, 34, col, letter, 0xFFFFFF);
            Gfx.Text(x + 58, y + 30, name, C.Text);
        }

        void System(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "System");
            int cy = pad + 60;
            W.Card(pad, cy, w - 2 * pad, 150);
            int lx = pad + 20, rx = w - pad - 20, y = cy + 18;
            Kv(lx, rx, y, "OS name", Host.OsName()); y += 28;
            Kv(lx, rx, y, "Architecture", Host.Is64Bit() != 0 ? "x64" : "x86"); y += 28;
            Kv(lx, rx, y, "CPU", Host.CpuVendor()); y += 28;
            Kv(lx, rx, y, "Installed memory", U.Mb(Host.MemTotalKb())); y += 28;
            Kv(lx, rx, y, "Disk", Host.DiskModel());
        }

        void Power(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Power");
            W.Primary(pad, pad + 70, 160, 40, "Shut down");
            W.Button(pad + 176, pad + 70, 160, 40, "Restart");
        }

        void ApplyTheme()
        {
            if (Theme.Dark != 0) { Theme.WallTop = 0x0B0B12u; Theme.WallBot = 0x1B1B2Au; }
            else { Theme.WallTop = 0x05162Cu; Theme.WallBot = 0x0B4A83u; }
        }

        // ---- Display ---------------------------------------------------
        void Display(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Display");
            int cy = pad + 60;
            W.Card(pad, cy, w - 2 * pad, 120);
            int lx = pad + 20, rx = w - pad - 20, y = cy + 18;
            Kv(lx, rx, y, "Resolution", "1280 x 720"); y += 26;
            Kv(lx, rx, y, "Scaling", "100%"); y += 26;
            Kv(lx, rx, y, "Theme", Theme.Dark != 0 ? "Dark" : "Light"); y += 26;
            Kv(lx, rx, y, "Refresh rate", "60 Hz");

            int by = cy + 140;
            W.Button(pad, by, 200, 38, Theme.Dark != 0 ? "Dark mode: On" : "Dark mode: Off");
            W.Button(pad + 216, by, 220, 38, "Apply wallpaper preset");

            int sy = by + 60;
            Gfx.Text(pad, sy, "Accent colour", C.Text);
            uint[] acc = Theme.Accents();
            for (int i = 0; i < 6; i++)
            {
                int x = SwX(pad, i), y2 = sy + 22;
                uint bg = W.Hot(x, y2, SW, 44) ? C.Hover : C.Card;
                Gfx.FillRound(x, y2, SW, 44, 8, bg);
                Gfx.DrawRound(x, y2, SW, 44, 8, C.Border);
                Gfx.FillRound(x + SW / 2 - 14, y2 + 8, 28, 28, 14, acc[i]);
                if (Theme.Accent == acc[i])
                    Gfx.DrawRound(x + 4, y2 + 4, SW - 8, 36, 6, C.Accent);
            }
        }

        // ---- Network & Internet ---------------------------------------
        void Network(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Network & Internet");
            int cy = pad + 60;
            NetCard(pad, cy, w - 2 * pad, "Ethernet", Theme.ActiveNet == 0);
            NetCard(pad, cy + 86, w - 2 * pad, "Wi-Fi", Theme.ActiveNet == 1);
        }

        void NetCard(int x, int y, int w, string name, bool active)
        {
            uint bg = W.Hot(x, y, w, 72) ? C.Hover : C.Card;
            Gfx.FillRound(x, y, w, 72, 8, bg);
            Gfx.DrawRound(x, y, w, 72, 8, C.Border);
            Gfx.Icon(x + 16, y + 20, 32, 0x0EA5E9, 'N', 0xFFFFFF);
            Gfx.Text(x + 60, y + 16, name, C.Text);
            Gfx.Text(x + 60, y + 40, active ? "Connected" : "Not connected",
                     active ? C.Good : C.TextSub);
            if (active) Gfx.FillRound(x + w - 92, y + 22, 72, 28, 14, C.Accent);
            else        Gfx.FillRound(x + w - 92, y + 22, 72, 28, 14, C.Border);
            Gfx.TextCenter(x + w - 92, y + 28, 72, active ? "On" : "Off", C.Text);
        }

        // ---- Storage ---------------------------------------------------
        void Storage(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Storage");
            int cy = pad + 60;
            int mk = Host.FileCount(0), sf = Host.FileCount(1);
            int total = Host.DiskSizeMb();
            W.Card(pad, cy, w - 2 * pad, 140);
            int lx = pad + 20, rx = w - pad - 20, y = cy + 18;
            Kv(lx, rx, y, "Local Disk (MKFS)", U.Cat(U.I(mk), " items")); y += 26;
            Kv(lx, rx, y, "System (SFS)", U.Cat(U.I(sf), " items")); y += 26;
            Kv(lx, rx, y, "Capacity", U.Cat(U.I(total), " MB")); y += 26;
            int usedP = (mk + sf) * 4; if (usedP > 100) usedP = 100;
            W.Meter(lx, y + 4, w - 2 * pad - 40, "Used", usedP, C.Accent);
        }

        // ---- Devices ---------------------------------------------------
        void Devices(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Devices");
            int cy = pad + 60;
            int y = cy;
            y = Dev(pad, y, w, "Processor", Host.CpuVendor());
            y = Dev(pad, y, w, "Memory", U.Mb(Host.MemTotalKb()));
            y = Dev(pad, y, w, "Disk", Host.DiskModel());
            y = Dev(pad, y, w, "Display", "Generic PnP Monitor");
            y = Dev(pad, y, w, "Audio", "NexOS HD Audio");
            y = Dev(pad, y, w, "Network", Host.NicPresent() != 0 ? "Ethernet Controller" : "None");
        }

        int Dev(int pad, int y, int w, string k, string v)
        {
            if (((y - (pad + 60)) / 40) % 2 == 1)
                Gfx.FillRound(pad, y, w - 2 * pad, 36, 6, C.CardAlt);
            Gfx.Text(pad + 16, y + 9, k, C.Text);
            Gfx.Text(w - pad - 16 - Gfx.Measure(v), y + 9, v, C.TextSub);
            return y + 40;
        }

        // ---- Personalization ------------------------------------------
        static uint[] MakeWt()
        {
            uint[] a = new uint[6];
            a[0]=0x05162C; a[1]=0x1B1035; a[2]=0x06283D; a[3]=0x052E16; a[4]=0x3A0A1E; a[5]=0x2E1A05;
            return a;
        }
        static uint[] MakeWb()
        {
            uint[] a = new uint[6];
            a[0]=0x0B4A83; a[1]=0x6D28D9; a[2]=0x0EA5E9; a[3]=0x107C10; a[4]=0xE11D8A; a[5]=0xF59E0B;
            return a;
        }

        void Personalize(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Personalization");
            int cy = pad + 60;
            Gfx.Text(pad, cy, "Background", C.Text);
            uint[] wt = MakeWt(), wb = MakeWb();
            for (int i = 0; i < 6; i++)
            {
                int x = SwX(pad, i), y2 = cy + 22;
                uint bg = W.Hot(x, y2, SW, 52) ? C.Hover : C.Card;
                Gfx.FillRound(x, y2, SW, 52, 8, bg);
                Gfx.Gradient(x + 4, y2 + 4, SW - 8, 44, wt[i], wb[i]);
                Gfx.DrawRound(x + 4, y2 + 4, SW - 8, 44, 6, C.Border);
            }
            int ay = cy + 96;
            Gfx.Text(pad, ay, "Accent colour", C.Text);
            uint[] acc = Theme.Accents();
            for (int i = 0; i < 6; i++)
            {
                int x = SwX(pad, i), y2 = ay + 22;
                uint bg = W.Hot(x, y2, SW, 44) ? C.Hover : C.Card;
                Gfx.FillRound(x, y2, SW, 44, 8, bg);
                Gfx.DrawRound(x, y2, SW, 44, 8, C.Border);
                Gfx.FillRound(x + SW / 2 - 14, y2 + 8, 28, 28, 14, acc[i]);
                if (Theme.Accent == acc[i])
                    Gfx.DrawRound(x + 4, y2 + 4, SW - 8, 36, 6, C.Accent);
            }
        }

        // ---- Taskbar ---------------------------------------------------
        void TaskbarPage(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Taskbar");
            int cy = pad + 60;
            Gfx.Text(pad, cy, "Taskbar alignment", C.Text);
            W.Button(pad, cy + 26, 170, 36, Theme.TaskbarLeft != 0 ? "Left" : "Centered");
            Gfx.Text(pad + 190, cy + 36, Theme.TaskbarLeft != 0 ? "Icons align left" : "Icons centred", C.TextSub);

            int sy = cy + 90;
            Gfx.Text(pad, sy, "Show labels", C.Text);
            W.Button(pad, sy + 26, 170, 36, Theme.ShowLabels != 0 ? "On" : "Off");
        }

        static void Kv(int lx, int rx, int y, string k, string v)
        {
            Gfx.Text(lx, y, k, C.TextSub);
            Gfx.Text(rx - Gfx.Measure(v), y, v, C.Text);
        }

        public override void OnClick(int mx, int my)
        {
            int w = Gfx.Width();
            int pad = 16;
            if (page != -1)
            {
                if (Back(mx, my)) { page = -1; return; }

                if (page == 1)        // Power
                {
                    if (U.In(mx, my, pad, pad + 70, 160, 40)) { Host.Shutdown(); return; }
                    if (U.In(mx, my, pad + 176, pad + 70, 160, 40)) { Host.Reboot(); return; }
                }
                else if (page == 2)   // Display
                {
                    int cy = pad + 60, by = cy + 140;
                    if (U.In(mx, my, pad, by, 200, 38))
                    { Theme.Dark = Theme.Dark != 0 ? 0 : 1; ApplyTheme(); return; }
                    if (U.In(mx, my, pad + 216, by, 220, 38)) { ApplyTheme(); return; }
                    uint[] acc = Theme.Accents();
                    for (int i = 0; i < 6; i++)
                    {
                        int x = SwX(pad, i), y2 = by + 60 + 22;
                        if (U.In(mx, my, x, y2, SW, 44)) { Theme.Accent = acc[i]; return; }
                    }
                    return;
                }
                else if (page == 3)   // Network
                {
                    int cy = pad + 60;
                    if (U.In(mx, my, pad, cy, w - 2 * pad, 72)) { Theme.ActiveNet = 0; return; }
                    if (U.In(mx, my, pad, cy + 86, w - 2 * pad, 72)) { Theme.ActiveNet = 1; return; }
                    return;
                }
                else if (page == 6)   // Personalize
                {
                    int cy = pad + 60;
                    uint[] wt = MakeWt(), wb = MakeWb();
                    for (int i = 0; i < 6; i++)
                    {
                        int x = SwX(pad, i), y2 = cy + 22;
                        if (U.In(mx, my, x, y2, SW, 52)) { Theme.WallTop = wt[i]; Theme.WallBot = wb[i]; return; }
                    }
                    int ay = cy + 96;
                    uint[] acc = Theme.Accents();
                    for (int i = 0; i < 6; i++)
                    {
                        int x = SwX(pad, i), y2 = ay + 22;
                        if (U.In(mx, my, x, y2, SW, 44)) { Theme.Accent = acc[i]; return; }
                    }
                    return;
                }
                else if (page == 7)   // Taskbar
                {
                    int cy = pad + 60;
                    if (U.In(mx, my, pad, cy + 26, 170, 36)) { Theme.TaskbarLeft = Theme.TaskbarLeft != 0 ? 0 : 1; return; }
                    int sy = cy + 90;
                    if (U.In(mx, my, pad, sy + 26, 170, 36)) { Theme.ShowLabels = Theme.ShowLabels != 0 ? 0 : 1; return; }
                    return;
                }
                return;   // System / Storage / Devices are display-only
            }

            // Tile grid.
            int gy = pad + 36, gx = pad, cols = 3;
            int cw = (w - 2 * pad - (cols - 1) * 12) / cols;
            int chh = 84;
            for (int i = 0; i < 6; i++)
            {
                int r = i / cols, c = i % cols;
                int x = gx + c * (cw + 12), y = gy + r * (chh + 12);
                if (U.In(mx, my, x, y, cw, chh))
                {
                    if (i == 0) page = 0;
                    else if (i == 1) page = 2;
                    else if (i == 2) page = 3;
                    else if (i == 3) page = 4;
                    else if (i == 4) page = 5;
                    else if (i == 5) page = 1;
                    return;
                }
            }
        }
    }

    // =================================================================
    //  Terminal
    // =================================================================
    public class TerminalApp : App
    {
        TBox t;           // command line editor (caret + selection + undo)
        string lastCmd;   // last command echoed
        string output;    // last command's output (replaced, never grown)

        public TerminalApp()
        {
            t = new TBox();
            lastCmd = "";
            output = "NexOS terminal. Type a command and press Enter.\nTry: help, ver, mem, ls";
        }

        public override string GetTitle() { return "Terminal"; }

        public override void OnPaint()
        {
            int w = Gfx.Width(), h = Gfx.Height();
            Gfx.FillRect(0, 0, w, h, 0x0C0C0C);
            int pad = 12, y = pad;

            if (lastCmd != "")
            {
                Gfx.Text(pad, y, U.Cat("C:\\> ", lastCmd), 0x4EC9B0);
                y += 22;
            }
            // Output, wrapped crudely per source newline by the host font.
            DrawLines(pad, y, output, 0xD4D4D4, h - 40);

            // Prompt line pinned to the bottom.
            int py = h - 26;
            Gfx.FillRect(0, py - 4, w, 30, 0x161616);
            string prompt = "C:\\> ";
            string before = Slice(t.text, 0, t.cursor);
            Gfx.Text(pad, py, U.Cat(prompt, t.text), 0xFFFFFF);
            if ((Host.Ticks() / 30) % 2 == 0) {
                int cx = pad + Gfx.Measure(U.Cat(prompt, before));
                Gfx.FillRect(cx, py, 2, 18, 0xFFFFFF);
            }
        }

        // Draw a string, breaking at '\n', clipped to maxY.
        static void DrawLines(int x, int y, string s, uint col, int maxY)
        {
            int n = s.Length;
            int start = 0;
            for (int i = 0; i <= n; i++)
            {
                if (i == n || s[i] == '\n')
                {
                    string line = Slice(s, start, i);
                    if (y < maxY) Gfx.Text(x, y, line, col);
                    y += 20;
                    start = i + 1;
                }
            }
        }

        // Substring is missing from the mini BCL; build one glyph by glyph.
        static string Slice(string s, int a, int b)
        {
            string r = "";
            for (int i = a; i < b; i++) r = U.Cat(r, Host.CharStr(s[i]));
            return r;
        }

        public override void OnKey(int ch)
        {
            if (ch == -2) { Run(); return; }   // enter
            t.Key(ch);                          // backspace / ctrl combos / typing
        }

        static string Chop(string s)
        {
            int n = s.Length;
            if (n <= 0) return "";
            string r = "";
            for (int i = 0; i < n - 1; i++) r = U.Cat(r, Host.CharStr(s[i]));
            return r;
        }

        void Run()
        {
            lastCmd = t.text;
            string cmd = t.text;
            t.text = ""; t.cursor = 0; t.selA = t.selB = 0;
            if (cmd == "") { return; }
            if (cmd == "cls" || cmd == "clear") { output = ""; return; }
            output = Host.Exec(cmd);
        }

        public override void OnClick(int mx, int my) { }
    }

    // =================================================================
    //  Memory Optimizer
    // =================================================================
    public class MemOptimizerApp : App
    {
        int before;    // KB free before, 0 = not run
        int after;

        public MemOptimizerApp() { before = 0; after = 0; }

        public override string GetTitle() { return "Memory Optimizer"; }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width();
            int pad = 18;

            W.Header(pad, pad, "Memory Optimizer");

            int usedP = Host.PagesUsed(), totP = Host.PagesTotal();
            int pct = totP > 0 ? usedP * 100 / totP : 0;

            int cy = pad + 40;
            W.Card(pad, cy, w - 2 * pad, 120);
            int mx = pad + 20, mw = w - 2 * pad - 40;
            Gfx.Text(mx, cy + 18, "Current memory usage", C.TextSub);
            Gfx.Progress(mx, cy + 44, mw, 14, pct, pct > 80 ? C.Danger : C.Accent);
            Gfx.Text(mx, cy + 70, U.Cat("Free: ", U.I(Host.PagesFree() * 4), " KB   Used: ",
                                        U.I(usedP * 4), " KB"), C.Text);

            W.Primary(pad, cy + 140, 200, 42, "Optimize now");

            if (before != 0)
            {
                int ry = cy + 200;
                W.Card(pad, ry, w - 2 * pad, 76);
                Gfx.Text(pad + 20, ry + 16, "Last optimization", C.TextSub);
                int freed = after - before;
                Gfx.Text(pad + 20, ry + 42,
                    U.Cat("Reclaimed ", U.I(freed > 0 ? freed : 0), " KB  (",
                          U.I(before), " -> ", U.I(after), " KB free)"),
                    freed > 0 ? C.Good : C.Text);
            }
        }

        public override void OnClick(int mx, int my)
        {
            int pad = 18, cy = pad + 40;
            if (U.In(mx, my, pad, cy + 140, 200, 42))
            {
                before = Host.PagesFree() * 4;
                Host.Optimize();
                after = Host.PagesFree() * 4;
            }
        }
    }

    // =================================================================
    //  Notepad (text viewer / scratch editor)
    // =================================================================
    public class NotepadApp : App
    {
        TBox t;        // editor model (caret + selection + undo)
        string name;   // currently loaded file
        bool picking;   // showing the open list

        public NotepadApp()
        {
            t = new TBox();
            string pending = Shell.TakeNotepadFile();
            if (pending != null && pending != "")
            {
                name = pending;
                t.text = Host.ReadText(1, pending);
                if (t.text == "") t.text = Host.ReadText(0, pending);
                if (t.text == "") t.text = "Untitled - type below, or click Open to read a file.";
            }
            else
            {
                t.text = "Untitled - type below, or click Open to read a file.";
                name = "Untitled";
            }
            picking = false;
        }

        public override string GetTitle() { return U.Cat("Notepad - ", name); }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 10;

            // Toolbar.
            Gfx.FillRect(0, 0, w, 40, 0xFAFAFA);
            Gfx.DrawLine(0, 40, w, 40, C.Border);
            W.Button(pad, 6, 70, 28, "Open");
            W.Button(pad + 80, 6, 70, 28, "New");
            Gfx.Text(pad + 170, 12, name, C.TextSub);

            if (picking) { DrawPicker(w, h); return; }

            // Text body.
            Gfx.FillRect(pad, 48, w - 2 * pad, h - 48 - pad, C.White);
            // selection highlight (behind text) - visible for select-all
            if (t.selA == 0 && t.selB == t.text.Length && t.text.Length > 0)
                Gfx.FillRect(pad + 8, 54, w - 2 * pad - 16, h - 48 - pad, 0xCDE6FF);
            DrawLines(pad + 8, 54, t.text, C.Text, h - pad, false);
            // caret (on top)
            int cl = 0, cc = 0;
            for (int i = 0; i < t.cursor; i++) { if (t.text[i] == '\n') { cl++; cc = 0; } else cc++; }
            if ((Host.Ticks() / 30) % 2 == 0) {
                int cxp = pad + 8 + cc * 8;
                int cyp = 54 + cl * 20;
                if (cyp < h - pad) Gfx.FillRect(cxp, cyp, 2, 18, 0x1A1A1A);
            }
        }

        void DrawPicker(int w, int h)
        {
            int pad = 10, y = 48;
            Gfx.Text(pad + 8, y, "Open from System (SFS):", C.TextSub);
            y += 26;
            int n = Host.FileCount(1);
            for (int i = 0; i < n && i < 12; i++)
            {
                int ry = y + i * W.RowH;
                bool hot = W.Hot(pad, ry, w - 2 * pad, W.RowH - 2);
                if (hot) Gfx.FillRound(pad, ry, w - 2 * pad, W.RowH - 2, 6, C.Hover);
                Gfx.Icon(pad + 8, ry + 4, 22, 0x60A5FA, 'F', 0xFFFFFF);
                Gfx.Text(pad + 40, ry + 8, Host.FileName(1, i), C.Text);
            }
        }

        static void DrawLines(int x, int y, string s, uint col, int maxY, bool caret)
        {
            int n = s.Length, start = 0;
            for (int i = 0; i <= n; i++)
            {
                if (i == n || s[i] == '\n')
                {
                    string line = "";
                    for (int j = start; j < i; j++) line = U.Cat(line, Host.CharStr(s[j]));
                    if (i == n && caret && (Host.Ticks() / 30) % 2 == 0)
                        line = U.Cat(line, "|");
                    if (y < maxY) Gfx.Text(x, y, line, col);
                    y += 20; start = i + 1;
                }
            }
        }

        public override void OnClick(int mx, int my)
        {
            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 10;
            if (U.In(mx, my, pad, 6, 70, 28)) { picking = !picking; return; }
            if (U.In(mx, my, pad + 80, 6, 70, 28))
            { t.text = ""; name = "Untitled"; picking = false; return; }

            if (picking)
            {
                int y = 48 + 26;
                int n = Host.FileCount(1);
                for (int i = 0; i < n && i < 12; i++)
                {
                    int ry = y + i * W.RowH;
                    if (U.In(mx, my, pad, ry, w - 2 * pad, W.RowH - 2))
                    {
                        name = Host.FileName(1, i);
                        t.text = Host.ReadText(1, name);
                        picking = false;
                        return;
                    }
                }
            }
        }

        public override void OnKey(int ch)
        {
            if (picking) { return; }
            if (ch == -2) { t.Insert("\n"); return; }
            t.Key(ch);
        }
    }
}
