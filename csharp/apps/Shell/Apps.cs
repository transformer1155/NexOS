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
        bool editDirty;// true once the user has started editing the buffer
        int dblT;      // TickMs() of the last row click (double-click detect)
        int dblIdx;    // row index of that click
        static string clip;   // last "copied" path

        public FileExplorerApp() { fs = 0; sel = -1; scroll = 0; editMode = 0; editBuf = ""; editOld = ""; editDirty = false; dblT = -100000; dblIdx = -1; }

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
                // Explicit finish/cancel hint so the mode is never a mystery.
                Gfx.Text(ax + 4, inputY + W.RowH - 4,
                         editMode == 1 ? "Enter 确认 · Esc 取消" : "", C.TextSub);
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
            // While editing, a click inside the inline box is ignored (the
            // editor handles key input). A click outside commits the edit and
            // then keeps processing, so nav buttons still work while a rename
            // is pending.
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
                if (U.In(mx, my, eax + 4, inputY, eaw - 8, W.RowH - 2)) return;
                CommitEdit();
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
                    if (!editDirty) { editBuf = ""; editDirty = true; return; }
                    int m = editBuf.Length;
                    if (m > 0) { string r = ""; for (int i = 0; i < m - 1; i++) r = U.Cat(r, Host.CharStr((int)editBuf[i])); editBuf = r; }
                    return;
                }
                if ((ch >= 0x20 && ch < 0x7F) || (ch >= 0x80 && ch <= 0xFFFF)) {
                    if (!editDirty) { editBuf = Host.CharStr(ch); editDirty = true; return; }
                    editBuf = U.Cat(editBuf, Host.CharStr(ch)); return;
                }
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
            else if (code == Desktop.A_F_NEWFILE) BeginNewFile();
            // Win11 cluster: Cut copies (no clipboard-move in the shell),
            // Paste / Share are acknowledged but inert for now.
            else if (code == Desktop.A_F_CUT)   CopySelected();
            else if (code == Desktop.A_F_PASTE) Host.Log("[FILE] paste (no-op)");
            else if (code == Desktop.A_F_SHARE) Host.Log("[FILE] share (no-op)");
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
            editMode = 1; editDirty = false;
        }
        // Create a new folder IMMEDIATELY with an auto-unique name, then
        // drop into the inline rename editor so the user can rename it on
        // the spot (Enter confirms / Esc keeps the auto name).  The editor
        // follows the new folder's row instead of overlaying the first
        // file, and a hint line spells out how to finish editing.
        void BeginNewFolder()
        {
            string baseName = "New Folder";
            string name = baseName;
            int n = Host.FileCount(fs);
            int k = 1;
            while (true)
            {
                bool clash = false;
                for (int i = 0; i < n; i++)
                    if (Host.FileName(fs, i) == name) { clash = true; break; }
                if (!clash) break;
                name = U.Cat(baseName, " (", U.I(k), ")");
                k++;
                n = Host.FileCount(fs);
            }
            Host.FileMkDir(fs, name);
            Host.FileRefresh();
            // Locate the new folder so the rename editor tracks its row
            // (and scrolls it into view) instead of sitting on row 0.
            int nn = Host.FileCount(fs);
            sel = -1;
            for (int i = 0; i < nn; i++)
                if (Host.FileName(fs, i) == name) { sel = i; break; }
            if (sel >= 0)
            {
                int rows = (Gfx.Height() - 60) / W.RowH;
                if (sel >= scroll + rows) scroll = sel - rows + 1;
                if (sel < scroll) scroll = sel;
            }
            editOld = name;
            editBuf = name;
            editMode = 1; editDirty = false;   // rename mode
        }
        // Create a new EMPTY TEXT FILE immediately with an auto-unique name,
        // then drop into the same inline rename editor so the user can name
        // it on the spot (Enter confirms / Esc keeps the auto name).  Mirrors
        // BeginNewFolder() but writes a file body ("" => empty file) via the
        // writable MKFS volume (fs==0) -- SFS is read-only and never offers
        // this action.
        void BeginNewFile()
        {
            string baseName = "New File.txt";
            string name = baseName;
            int n = Host.FileCount(fs);
            int k = 1;
            while (true)
            {
                bool clash = false;
                for (int i = 0; i < n; i++)
                    if (Host.FileName(fs, i) == name) { clash = true; break; }
                if (!clash) break;
                name = U.Cat("New File (", U.I(k), ").txt");
                k++;
                n = Host.FileCount(fs);
            }
            // WriteText with an empty body creates the file on MKFS (fs==0).
            Host.WriteText(fs, name, "");
            Host.Log(U.Cat("[FILES] new file created: ", name));
            Host.FileRefresh();
            // Locate the new file so the rename editor tracks its row
            // (and scrolls it into view) instead of sitting on row 0.
            int nn = Host.FileCount(fs);
            sel = -1;
            for (int i = 0; i < nn; i++)
                if (Host.FileName(fs, i) == name) { sel = i; break; }
            if (sel >= 0)
            {
                int rows = (Gfx.Height() - 60) / W.RowH;
                if (sel >= scroll + rows) scroll = sel - rows + 1;
                if (sel < scroll) scroll = sel;
            }
            editOld = name;
            editBuf = name;
            editMode = 1; editDirty = false;   // rename mode
        }
        void CommitEdit()
        {
            if (editMode == 1 && editBuf.Length > 0 && editBuf != editOld)
            {
                Host.FileRename(fs, editOld, editBuf);
                Host.Log("[FILES] rename ok");
            }
            else if (editMode == 2 && editBuf.Length > 0)
                Host.FileMkDir(fs, editBuf);
            if (editMode != 0) Host.FileRefresh();
            editMode = 0; editBuf = ""; editDirty = false;
        }
        void CancelEdit() { editMode = 0; editBuf = ""; editDirty = false; }

        void ShowProps()
        {
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            string nm = Host.FileName(fs, sel);
            string ty = Host.FileIsDir(fs, sel) != 0 ? "Folder" : "File";
            string loc = fs == 0 ? "Local Disk (MKFS)" : "System (SFS)";
            string[] labs = new string[4];
            int[]    acts = new int[4];
            labs[0] = U.Cat("Name:    ", nm); acts[0] = -3;
            labs[1] = U.Cat("Type:    ", ty); acts[1] = -3;
            labs[2] = U.Cat("Location:", loc); acts[2] = -3;
            labs[3] = "Close";                  acts[3] = -3;   // -3 = dismiss
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
                    // 4 Storage, 5 Devices, 6 Personalize, 7 Taskbar, 8 Plugins
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
            if (page == 8) { Plugins(pad, w); return; }
            if (page == 9) { AppsPage(pad, w); return; }

            W.Header(pad, pad, "All Control Panel Items");
            int gy = pad + 36, gx = pad;
            int cols = 3;
            int cw = (w - 2 * pad - (cols - 1) * 12) / cols;
            int chh = 84;
            for (int i = 0; i < 8; i++)
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
            if (i == 4) return "Devices"; if (i == 5) return "Power";
            if (i == 6) return "Plugins"; return "Apps";
        }
        static int TileLetter(int i)
        {
            if (i == 0) return 'S'; if (i == 1) return 'D'; if (i == 2) return 'N';
            if (i == 3) return 'H'; if (i == 4) return 'V'; if (i == 5) return 'P';
            if (i == 6) return 'G'; return 'A';
        }
        static uint TileColor(int i)
        {
            if (i == 0) return 0x0078D4; if (i == 1) return 0x8B5CF6; if (i == 2) return 0x10B981;
            if (i == 3) return 0xF59E0B; if (i == 4) return 0x06B6D4; if (i == 5) return 0xEF4444;
            if (i == 6) return 0x6D28D9; return 0x0EA5E9;
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
            if (Theme.Dark != 0) { Theme.WallTop = 0x218FD9u; Theme.WallBot = 0x05216Bu; }
            else { Theme.WallTop = 0x5B86C4u; Theme.WallBot = 0xCFE3FFu; }
        }

        // ---- Display ---------------------------------------------------
        void Display(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Display");
            int cy = pad + 60;
            W.Card(pad, cy, w - 2 * pad, 120);
            int lx = pad + 20, rx = w - pad - 20, y = cy + 18;
            Kv(lx, rx, y, "Resolution", U.Cat(U.I(Gfx.Width()), " x ", U.I(Gfx.Height()))); y += 26;
            Kv(lx, rx, y, "Scaling", "100%"); y += 26;
            Kv(lx, rx, y, "Theme", Theme.Dark != 0 ? "Dark" : "Light"); y += 26;
            Kv(lx, rx, y, "Refresh rate", "60 Hz");

            int by = cy + 140;
            W.Button(pad, by, 200, 38, Theme.Dark != 0 ? "Dark mode: On" : "Dark mode: Off");
            W.Button(pad + 216, by, 220, 38, "Apply wallpaper preset");

            int ly2 = by + 48;
            W.Button(pad, ly2, 200, 38, Theme.DesktopMode == 0 ? "Layout: Clean" : "Layout: Rich");
            Gfx.Text(pad + 216, ly2 + 12, Theme.DesktopMode == 0 ? "Win11 desktop" : "Portal launcher", C.TextSub);

            int sy = by + 96;
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

            int py = sy + 96;
            Gfx.Text(pad, py, "Pixel / CRT monitor", C.Text);
            W.Toggle(pad + 200, py + 16, 56, 30, Theme.PixelMode != 0 ? 1 : 0, 0x10);
            W.Toggle(pad + 430, py + 16, 56, 30, Theme.PixelScan != 0 ? 1 : 0, 0x11);
            int qy = py + 70;
            Gfx.Text(pad, qy, "Pixel size", C.Text);
            int pct = ((Theme.PixelScale - 1) * 100) / 3;   // 1..4 -> 0..100
            W.Slider(pad + 200, qy + 14, 220, 16, pct, 0x12);
            Gfx.Text(pad + 430, qy + 14, U.Cat(U.I(Theme.PixelScale), "x"), C.TextSub);

            int ty = qy + 70;
            Gfx.Text(pad, ty, "Terminal", C.Text);
            W.Button(pad,      ty + 22, 200, 38, U.Cat("Font: ", U.I(Theme.TermCellH)));
            W.Button(pad + 236, ty + 22, 220, 38, Theme.TermBgMode != 0 ? "Background: Tint" : "Background: Solid");
        }

        // ---- Network & Internet ---------------------------------------
        void Network(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Network & Internet");
            int cy = pad + 60;
            NetCard(pad, cy, w - 2 * pad, "Ethernet", Theme.ActiveNet == 0, 0x20);
            NetCard(pad, cy + 86, w - 2 * pad, "Wi-Fi", Theme.ActiveNet == 1, 0x21);
        }

        void NetCard(int x, int y, int w, string name, bool active, int id)
        {
            uint bg = W.Hot(x, y, w, 72) ? C.Hover : C.Card;
            Gfx.FillRound(x, y, w, 72, 8, bg);
            Gfx.DrawRound(x, y, w, 72, 8, C.Border);
            Gfx.Icon(x + 16, y + 20, 32, 0x0EA5E9, 'N', 0xFFFFFF);
            Gfx.Text(x + 60, y + 16, name, C.Text);
            Gfx.Text(x + 60, y + 40, active ? "Connected" : "Not connected",
                     active ? C.Good : C.TextSub);
            W.Toggle(x + w - 92 + 8, y + 22, 56, 28, active ? 1 : 0, id);
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
            W.Toggle(pad + 190, sy + 26, 56, 30, Theme.ShowLabels != 0 ? 1 : 0, 0x13);
        }

        // ---- Plugins (page 8) -------------------------------------------
        // Reads the catalogue the kernel serialises to plugins.lst.  Each
        // line is "id|name|state|loaded".  Clicking a row toggles load via
        // the `plugin toggle <id>` shell command (which re-persists).
        void Plugins(int pad, int w)
        {
            Gfx.Text(pad, pad, "< Back", C.Accent);
            W.Header(pad, pad + 26, "Plugins");
            Gfx.Text(pad, pad + 58, "Click a row to load / unload. Green = loaded.", C.TextSub);
            int top = pad + 84, rowH = 30, gap = 4;
            string data = Host.ReadText(0, "plugins.lst");
            if (data == null || NexOS.Sys.StrLen(data) == 0) {
                Gfx.Text(pad, top, "(no plugin data - run 'plugin persist')", C.TextSub);
                return;
            }
            int n = NexOS.Sys.StrLen(data), i = 0, y = top;
            while (i < n && y < Gfx.Height() - 10)
            {
                int start = i;
                while (i < n && NexOS.Sys.StrCharAt(data, i) != '\n') i++;
                string line = NexOS.Sys.StrSub(data, start, i - start);
                i++; // consume newline
                int ln = NexOS.Sys.StrLen(line);
                int[] seps = new int[3]; int sc = 0;
                for (int k = 0; k < ln; k++) if (NexOS.Sys.StrCharAt(line, k) == '|') seps[sc++] = k;
                if (sc < 3) continue;
                string name = NexOS.Sys.StrSub(line, seps[0] + 1, seps[1] - seps[0] - 1);
                string stStr = NexOS.Sys.StrSub(line, seps[1] + 1, seps[2] - seps[1] - 1);
                string ldStr = NexOS.Sys.StrSub(line, seps[2] + 1, ln - seps[2] - 1);
                bool loaded = NexOS.Sys.StrEq(ldStr, "1");
                uint col = loaded ? 0x107C10u : 0x6B7280u;
                uint bg = U.In(Gfx.MouseX(), Gfx.MouseY(), pad, y, w - 2 * pad, rowH) ? C.Hover : C.Card;
                Gfx.FillRound(pad, y, w - 2 * pad, rowH, 6, bg);
                Gfx.Text(pad + 10, y + 8, name, C.Text);
                string status = loaded ? "Loaded" : (NexOS.Sys.StrEq(stStr, "0") ? "Planned" : "Available");
                Gfx.Text(pad + 10 + Gfx.Measure(name) + 12, y + 8, status, col);
                Gfx.Text(w - pad - 230, y + 8, NexOS.Sys.StrSub(line, 0, seps[0]), C.TextSub);
                y += rowH + gap;
            }
        }

        // Return the plugin id of catalogue row `idx` (0-based), or "".
        string PluginIdAt(int idx)
        {
            string data = Host.ReadText(0, "plugins.lst");
            if (data == null || NexOS.Sys.StrLen(data) == 0) return "";
            int n = NexOS.Sys.StrLen(data), i = 0, cur = 0;
            while (i < n)
            {
                int start = i;
                while (i < n && NexOS.Sys.StrCharAt(data, i) != '\n') i++;
                string line = NexOS.Sys.StrSub(data, start, i - start);
                i++;
                if (cur == idx)
                {
                    int ln = NexOS.Sys.StrLen(line);
                    int sep = 0;
                    while (sep < ln && NexOS.Sys.StrCharAt(line, sep) != '|') sep++;
                    return NexOS.Sys.StrSub(line, 0, sep);
                }
                cur++;
            }
            return "";
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
                    { Theme.Dark = Theme.Dark != 0 ? 0 : 1; ApplyTheme(); Theme.Save(); return; }
                    if (U.In(mx, my, pad + 216, by, 220, 38)) { ApplyTheme(); Theme.Save(); return; }
                    if (U.In(mx, my, pad, by + 48, 200, 38))
                    { Theme.DesktopMode = Theme.DesktopMode != 0 ? 0 : 1; Theme.Save(); return; }
                    uint[] acc = Theme.Accents();
                    for (int i = 0; i < 6; i++)
                    {
                        int x = SwX(pad, i), y2 = by + 96 + 22;
                        if (U.In(mx, my, x, y2, SW, 44)) { Theme.Accent = acc[i]; Theme.Save(); return; }
                    }
                    int sy = by + 96, py = sy + 96;
                    if (U.In(mx, my, pad + 200, py + 16, 56, 30))
                    { Theme.PixelMode = Theme.PixelMode != 0 ? 0 : 1; Theme.ApplyPixel(); Theme.Save(); return; }
                    if (U.In(mx, my, pad + 430, py + 16, 56, 30))
                    { Theme.PixelScan = Theme.PixelScan != 0 ? 0 : 1; Theme.ApplyPixel(); Theme.Save(); return; }
                    int qy = py + 70;
                    if (U.In(mx, my, pad + 200, qy + 14, 220, 16))
                    {
                        int r = mx - (pad + 200);
                        int p = (r * 100) / 220;
                        Theme.PixelScale = 1 + (p * 3) / 100; if (Theme.PixelScale > 4) Theme.PixelScale = 4;
                        Theme.ApplyPixel(); Theme.Save(); return;
                    }
                    int ty = qy + 70;
                    if (U.In(mx, my, pad, ty + 22, 200, 38))
                    { Theme.TermCellH = TerminalApp.ZoomStep(Theme.TermCellH, 1); Theme.Save(); return; }
                    if (U.In(mx, my, pad + 236, ty + 22, 220, 38))
                    { Theme.TermBgMode = Theme.TermBgMode != 0 ? 0 : 1; Theme.Save(); return; }
                    return;
                }
                else if (page == 3)   // Network
                {
                    int cy = pad + 60;
                    if (U.In(mx, my, pad, cy, w - 2 * pad, 72)) { Theme.ActiveNet = 0; Theme.Save(); return; }
                    if (U.In(mx, my, pad, cy + 86, w - 2 * pad, 72)) { Theme.ActiveNet = 1; Theme.Save(); return; }
                    return;
                }
                else if (page == 6)   // Personalize
                {
                    int cy = pad + 60;
                    uint[] wt = MakeWt(), wb = MakeWb();
                    for (int i = 0; i < 6; i++)
                    {
                        int x = SwX(pad, i), y2 = cy + 22;
                        if (U.In(mx, my, x, y2, SW, 52)) { Theme.WallTop = wt[i]; Theme.WallBot = wb[i]; Theme.Save(); return; }
                    }
                    int ay = cy + 96;
                    uint[] acc = Theme.Accents();
                    for (int i = 0; i < 6; i++)
                    {
                        int x = SwX(pad, i), y2 = ay + 22;
                        if (U.In(mx, my, x, y2, SW, 44)) { Theme.Accent = acc[i]; Theme.Save(); return; }
                    }
                    return;
                }
                else if (page == 7)   // Taskbar
                {
                    int cy = pad + 60;
                    if (U.In(mx, my, pad, cy + 26, 170, 36)) { Theme.TaskbarLeft = Theme.TaskbarLeft != 0 ? 0 : 1; Theme.Save(); return; }
                    int sy = cy + 90;
                    if (U.In(mx, my, pad + 190, sy + 26, 56, 30)) { Theme.ShowLabels = Theme.ShowLabels != 0 ? 0 : 1; Theme.Save(); return; }
                    return;
                }
                else if (page == 8)   // Plugins
                {
                    if (U.In(mx, my, pad, pad, 80, 20)) { page = -1; return; }
                    int top = pad + 84, rowH = 30, gap = 4;
                    if (my < top) return;
                    int idx = (my - top) / (rowH + gap);
                    string id = PluginIdAt(idx);
                    if (NexOS.Sys.StrLen(id) > 0) Host.Exec(U.Cat("plugin toggle ", id));
                    return;
                }
                return;   // System / Storage / Devices are display-only
            }

            // "Apps & features" page: install / uninstall each of the 12 apps.
            if (page == 9)
            {
                int top = pad + 60, rowH = 44, btnW = 110, btnH = 30;
                int btnX = w - pad - btnW;
                for (int i = 0; i < 12; i++)
                {
                    int ry = top + i * rowH;
                    if (U.In(mx, my, btnX, ry + (rowH - btnH) / 2, btnW, btnH))
                    {
                        int cur = Desktop.IsInstalled(i);
                        Desktop.SetInstalled(i, cur == 0 ? 1 : 0);
                        return;
                    }
                }
                return;
            }

            // Tile grid.
            int gy = pad + 36, gx = pad, cols = 3;
            int cw = (w - 2 * pad - (cols - 1) * 12) / cols;
            int chh = 84;
            for (int i = 0; i < 7; i++)
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
                    else if (i == 6) { Host.Exec("plugin persist"); page = 8; }
                    else if (i == 7) page = 9;
                    return;
                }
            }
        }

        // ---- "Apps & features" page (page 9) ---------------------------
        static void AppsPage(int pad, int w)
        {
            int top = pad + 60, rowH = 44, btnW = 110, btnH = 30;
            int btnX = w - pad - btnW;
            int listW = w - 2 * pad;
            Gfx.Text(pad, top - 36, "Apps & features", C.Text);
            Gfx.Text(pad, top - 16, "Install or uninstall the built-in applications.", C.TextSub);
            for (int i = 0; i < 12; i++)
            {
                int ry = top + i * rowH;
                if (i > 0) Gfx.DrawLine(pad, ry, pad + listW, ry, C.Border);
                uint col = Desktop.AppColor(i); int let = Desktop.AppLetter(i);
                if (Gfx.HasImage(Tex.Icon + i) != 0)
                    Gfx.Image(Tex.Icon + i, pad, ry + (rowH - 18) / 2, 18, 18);
                else
                    Gfx.Icon(pad, ry + (rowH - 18) / 2, 18, (uint)col, let, 0xFFFFFF);
                Gfx.Text(pad + 26, ry + 4, Desktop.KindName(i), C.Text);
                Gfx.Text(pad + 26, ry + 22, U.Cat("v", Desktop.AppVersion(i), "  ", Desktop.AppDesc(i)), C.TextSub);
                int by = ry + (rowH - btnH) / 2;
                int inst = Desktop.IsInstalled(i);
                uint bcol = inst != 0 ? 0xB04848u : C.Accent;
                string label = inst != 0 ? "Uninstall" : "Install";
                Gfx.FillRound(btnX, by, btnW, btnH, 6, bcol);
                Gfx.Text(btnX + (btnW - Gfx.Measure(label)) / 2, by + 7, label, 0xFFFFFF);
            }
        }
    }

    // =================================================================
    //  Terminal
    // =================================================================
    // =================================================================
    //  Terminal  --  GNOME Terminal compatible emulator.
    //  One TerminalApp window hosts N tabs; each tab (Term) owns its own
    //  scrollback, input line, history and selection.
    // =================================================================
    public class TerminalApp : App
    {
        const int SCROLL_MAX = 2000, HIST_MAX = 200, TAB_MAX = 8;
        const int GRIDCAP = 65536;   // cell grid cap (MiniCLR heap-safe: 256KB/array)
        const int TAB_H = 26, PAD = 8, ROWCAP = 16000;
        const int BG = 0x300A24, FG = 0xFFFFFF, SEL = 0x4A2E5A;
        const int URLC = 0x55AAFF;   // link-blue for URL hover highlight
        const int ESC = 27;

        // One session == one tab.
        public class Term
        {
            public string[] lines = new string[SCROLL_MAX];
            public int head = 0, count = 0;   // ring buffer of logical lines
            public int view = 0;               // scrollback offset from bottom
            public string input = "";
            public int caret = 0;
            public bool hasSel = false;
            public int selLineA = -1, selCharA = -1, selLineB = -1, selCharB = -1;
            public string[] hist = new string[HIST_MAX];
            public int histN = 0, histPos = 0;
            public bool search = false;
            public string sbuf = "";
            public string title = "root@nexos";
            public bool dragging = false;
        }

        // Parsed line: displayed chars (ANSI stripped) + per-char fg/bg
        // (0xRRGGBB, -1 = default).  Stored as a char[] so OnPaint can
        // render without building a string per frame (the O(n^2) Cat loop
        // warned about in U.Sub would fault the MiniCLR bump heap).
        public class PR
        {
            public char[] ch;
            public int len;
            public int[] fg;
            public int[] bg;
            public int[] url;   // per-char URL flag (0/1), for hover highlight
        }

        int[] PAL;                  // 16-colour Ubuntu GNOME palette
        int active = 0;
        Term[] tabs;
        int tabN = 1;

        // Layout caches (recomputed every frame for the active tab).
        int cols, rows, cellW, cellH, contentX, contentY, contentW, contentH;
        int[] gLine, gChar; int gN;
        int[] rowLine, rowStart, rowEnd; int rowN;
        PR[] prs; int firstAbsRow, visRows, lastTotalRows;

        int hoverX = -1, hoverY = -1;
        int hoverLine = -1, hoverChar = -1;
        int lastDownMs = 0, lastDownCell = -1, clickCount = 0;
        string user = "root", host = "nexos", cwd = "~";

        public TerminalApp()
        {
            PAL = new int[] { 0x2E3436,0xCC0000,0x4E9A06,0xC4A000,0x3465A4,0x75507B,
                              0x06989A,0xD3D7CF,0x555753,0xEF2929,0x8AE234,0xFCE94F,
                              0x729FCF,0xAD7FA8,0x34E2E2,0xEEEEEC };
            tabs = new Term[TAB_MAX];
            tabs[0] = new Term();
            tabN = 1; active = 0;
            gLine = new int[GRIDCAP]; gChar = new int[GRIDCAP];
            AppendLine(tabs[0],
                U.Cat("NexOS Terminal - GNOME Terminal compatible.\n",
                      "Type `help`.  Ctrl+Shift+T new tab, Ctrl+Shift+C copy,\n",
                      "Ctrl+Shift+V paste, middle-click paste, wheel = scrollback."));
        }

        Term T() { return tabs[active]; }

        // Test / introspection helpers (used by the WinHost --termtest
        // harness).  They expose otherwise-private per-tab state without
        // changing any runtime behaviour.
        public Term ActiveTerm() { return T(); }
        public string SelText() { return GetSelText(T()); }
        public int TabCount() { return tabN; }
        public string UserHost() { return U.Cat(user, "@", host); }
        // Test-only helpers (WinHost --termtest harness): inject a logical
        // line (e.g. ANSI-coloured) and read the most recently appended one.
        public void TestAppend(string s) { AppendLine(T(), s); }
        public string LastLine()
        {
            Term t = T(); int b = t.head - 1; if (b < 0) b += SCROLL_MAX;
            return t.lines[b];
        }

        public override string GetTitle() { return U.Cat(user, "@", host, ":", cwd, "$", ""); }

        // ---- scrollback ----------------------------------------------
        string RingAt(Term t, int idx)
        {
            int b = t.head - t.count; if (b < 0) b += SCROLL_MAX;
            return t.lines[(b + idx) % SCROLL_MAX];
        }
        void AppendLine(Term t, string s)
        {
            if (s == null) return;
            int n = s.Length, start = 0;
            for (int i = 0; i <= n; i++)
            {
                if (i == n || s[i] == '\n')
                {
                    t.lines[t.head] = U.Sub(s, start, i - start);
                    t.head = (t.head + 1) % SCROLL_MAX;
                    if (t.count < SCROLL_MAX) t.count++;
                    start = i + 1;
                }
            }
            if (t.view > 0) t.view = 0;   // stick to bottom on new output
        }
        void ClearScreen(Term t) { t.count = 0; t.head = 0; t.view = 0; }

        // ---- prompt / title ------------------------------------------
        string Pad2(int v) { string s = U.I(v); if (s.Length < 2) s = U.Cat("0", s); return s; }
        string PS1()
        {
            string ts = U.Cat("[", U.I(Host.Hour()), ":", Pad2(Host.Minute()), ":",
                              Pad2(Host.Second()), "] ");
            return U.Cat(ts, user, "@", host, ":", cwd, "$ ");
        }

        // ---- ANSI / SGR colour ---------------------------------------
        int Cube(int n)
        {
            int v = n - 16, r = v / 36, g = (v / 6) % 6, b = v % 6;
            return (CubeC(r) << 16) | (CubeC(g) << 8) | CubeC(b);
        }
        int CubeC(int x) { return x == 0 ? 0 : 55 + x * 40; }
        int Gray(int n) { int v = 8 + (n - 232) * 10; return (v << 16) | (v << 8) | v; }

        PR ParseLine(string s)
        {
            PR pr = new PR();
            int n = (s == null) ? 0 : s.Length;
            char[] ch = new char[n + 1];
            int[] fg = new int[n + 1], bg = new int[n + 1];
            int[] url = new int[n + 1];
            int curFg = -1, curBg = -1, k = 0, i = 0;
            bool inUrl = false;
            while (i < n)
            {
                char c = s[i];
                if (c == (char)ESC && i + 1 < n && s[i + 1] == '[')
                {
                    int j = i + 2;
                    int[] p = new int[16]; int pn = 0, cur = 0; bool any = false;
                    while (j < n && s[j] != 'm' && s[j] != (char)ESC)
                    {
                        if (s[j] >= '0' && s[j] <= '9') { cur = cur * 10 + (s[j] - '0'); any = true; }
                        else if (s[j] == ';') { if (pn < 16) p[pn] = any ? cur : 0; pn++; cur = 0; any = false; }
                        j++;
                    }
                    if (pn < 16 && any) p[pn++] = cur;
                    else if (!any && pn == 0) { p[0] = 0; pn = 1; }
                    int kk = 0;
                    while (kk < pn)
                    {
                        int code = p[kk];
                        if (code == 0) { curFg = -1; curBg = -1; }
                        else if (code >= 30 && code <= 37) curFg = PAL[code - 30];
                        else if (code >= 90 && code <= 97) curFg = PAL[code - 90 + 8];
                        else if (code >= 40 && code <= 47) curBg = PAL[code - 40];
                        else if (code >= 100 && code <= 107) curBg = PAL[code - 100 + 8];
                        else if (code == 38 || code == 48)
                        {
                            int isFg = (code == 38) ? 1 : 0;
                            if (kk + 2 < pn && p[kk + 1] == 5)
                            {
                                int v = p[kk + 2];
                                int col = (v < 16) ? PAL[v] : (v < 232 ? Cube(v) : Gray(v));
                                if (isFg != 0) curFg = col; else curBg = col; kk += 2;
                            }
                            else if (kk + 4 < pn && p[kk + 1] == 2)
                            {
                                int r = p[kk + 2], g = p[kk + 3], b = p[kk + 4];
                                int col = (r << 16) | (g << 8) | b;
                                if (isFg != 0) curFg = col; else curBg = col; kk += 4;
                            }
                        }
                        kk++;
                    }
                    i = (j < n && s[j] == 'm') ? j + 1 : j;
                }
                else if (c == '\r' || c == '\n') i++;
                else
                {
                    bool isU = IsUrlChar(c);
                    if (inUrl)
                    {
                        if (isU) { ch[k] = c; fg[k] = curFg; bg[k] = curBg; url[k] = 1; k++; }
                        else { inUrl = false; ch[k] = c; fg[k] = curFg; bg[k] = curBg; k++; }
                    }
                    else
                    {
                        int sl = UrlSchemeLen(s, i, n);
                        if (sl > 0) { inUrl = true; ch[k] = c; fg[k] = curFg; bg[k] = curBg; url[k] = 1; k++; }
                        else { ch[k] = c; fg[k] = curFg; bg[k] = curBg; k++; }
                    }
                    i++;
                }
            }
            pr.ch = ch; pr.fg = fg; pr.bg = bg; pr.url = url; pr.len = k;
            return pr;
        }

        // ---- mini-BCL string helpers (MiniCLR has no Substring/IndexOf/etc) ----
        static bool IsWordChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                   c == '/' || c == '.' || c == ':';
        }
        // ---- URL detection (MiniCLR-safe: no Substring/IndexOf) ----------
        static bool IsUrlChar(char c)
        {
            if (c >= 'a' && c <= 'z') return true;
            if (c >= 'A' && c <= 'Z') return true;
            if (c >= '0' && c <= '9') return true;
            if (c == '.' || c == '-' || c == '_' || c == ':' || c == '/' ||
                c == '?' || c == '#' || c == '&' || c == '=' || c == '+' ||
                c == '%' || c == '@' || c == '~') return true;
            return false;
        }
        // Returns the length of a URL scheme prefix at s[i..] (http(s)://,
        // ftp://, www.), else 0.  Bounds-checked so it is safe to call with
        // any i.
        static int UrlSchemeLen(string s, int i, int n)
        {
            if (i + 6 < n &&
                s[i] == 'h' && s[i + 1] == 't' && s[i + 2] == 't' && s[i + 3] == 'p' &&
                s[i + 4] == ':' && s[i + 5] == '/' && s[i + 6] == '/') return 7;
            if (i + 7 < n &&
                s[i] == 'h' && s[i + 1] == 't' && s[i + 2] == 't' && s[i + 3] == 'p' &&
                s[i + 4] == 's' && s[i + 5] == ':' && s[i + 6] == '/' && s[i + 7] == '/') return 8;
            if (i + 5 < n &&
                s[i] == 'f' && s[i + 1] == 't' && s[i + 2] == 'p' &&
                s[i + 3] == ':' && s[i + 4] == '/' && s[i + 5] == '/') return 6;
            if (i + 3 < n &&
                s[i] == 'w' && s[i + 1] == 'w' && s[i + 2] == 'w' && s[i + 3] == '.') return 4;
            return 0;
        }
        static string Trim(string s)
        {
            int a = 0, b = s.Length;
            while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
            if (b < a) b = a;
            return U.Sub(s, a, b - a);
        }
        static string Lower(string s)
        {
            string r = "";
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                r = U.Cat(r, Host.CharStr((int)c));
            }
            return r;
        }
        static int IndexOf(string s, string sub)
        {
            int n = s.Length, m = sub.Length;
            if (m == 0) return 0;
            for (int i = 0; i + m <= n; i++)
            {
                bool ok = true;
                for (int j = 0; j < m; j++) { if (s[i + j] != sub[j]) { ok = false; break; } }
                if (ok) return i;
            }
            return -1;
        }
        static bool StartsWith(string s, string p)
        {
            int m = p.Length;
            if (s.Length < m) return false;
            for (int i = 0; i < m; i++) if (s[i] != p[i]) return false;
            return true;
        }
        // Insert / delete on a string (used for the input line editing).
        static string InsStr(string s, int pos, string ins)
        {
            return U.Cat(U.Sub(s, 0, pos), U.Cat(ins, U.Sub(s, pos, s.Length - pos)));
        }
        static string DelStr(string s, int pos, int len)
        {
            return U.Cat(U.Sub(s, 0, pos), U.Sub(s, pos + len, s.Length - pos - len));
        }

        // ---- layout --------------------------------------------------
        void ComputeColsRows()
        {
            int W = Gfx.Width(), H = Gfx.Height();
            contentX = PAD; contentY = TAB_H + PAD;
            contentW = W - contentX * 2 - 8;
            contentH = H - contentY - PAD;
            if (contentW < 40) contentW = 40;
            if (contentH < 40) contentH = 40;
            int ch2 = Theme.TermCellH; if (ch2 < 12) ch2 = 12; if (ch2 > 28) ch2 = 28;
            cellH = ch2;
            int w0 = Gfx.Measure("0"); if (w0 < 4) w0 = 8; if (w0 > 20) w0 = 10;
            cellW = (w0 * ch2) / 18; if (cellW < 4) cellW = 4;
            rows = contentH / cellH; cols = contentW / cellW;
            if (rows < 1) rows = 1; if (cols < 1) cols = 1;
        }
        void ComputeLayout(Term t)
        {
            int ndoc = t.count + 1;
            prs = new PR[ndoc];
            rowLine = new int[ROWCAP]; rowStart = new int[ROWCAP]; rowEnd = new int[ROWCAP]; rowN = 0;
            for (int idx = 0; idx < ndoc; idx++)
            {
                string s = (idx < t.count) ? RingAt(t, idx) : U.Cat(PS1(), t.input);
                PR pr = ParseLine(s); prs[idx] = pr;
                int L = pr.len, pos = 0;
                while (pos < L && rowN < ROWCAP - 1)
                {
                    int take = (L - pos < cols) ? (L - pos) : cols;
                    rowLine[rowN] = idx; rowStart[rowN] = pos; rowEnd[rowN] = pos + take; rowN++;
                    pos += take;
                }
            }
            lastTotalRows = rowN;
            visRows = rows;
            firstAbsRow = rowN - visRows - t.view;
            if (firstAbsRow < 0) firstAbsRow = 0;
            for (int vr = 0; vr < rows; vr++)
            {
                int absRow = firstAbsRow + vr, b = vr * cols;
                if (absRow < 0 || absRow >= rowN) { for (int c = 0; c < cols && b + c < GRIDCAP; c++) gLine[b + c] = -1; continue; }
                for (int c = 0; c < cols && b + c < GRIDCAP; c++)
                {
                    int ca = rowStart[absRow] + c;
                    if (ca < rowEnd[absRow]) { gLine[b + c] = rowLine[absRow]; gChar[b + c] = ca; }
                    else gLine[b + c] = -1;
                }
            }
            gN = rows * cols; if (gN > GRIDCAP) gN = GRIDCAP;
        }
        int[] CellAt(int mx, int my)
        {
            if (my < contentY || my >= contentY + contentH || mx < contentX || mx >= contentX + contentW)
                return new int[] { -1, -1 };
            int col = (mx - contentX) / cellW, vr = (my - contentY) / cellH;
            if (vr < 0 || vr >= rows) return new int[] { -1, -1 };
            int idx = vr * cols + col; if (idx >= gN) return new int[] { -1, -1 };
            int line = gLine[idx]; if (line < 0) return new int[] { -1, -1 };
            return new int[] { line, gChar[idx] };
        }
        bool CellSelected(Term t, int line, int chr)
        {
            if (!t.hasSel) return false;
            long ka = (long)line * 1000000L + (long)chr;
            long a = (long)t.selLineA * 1000000L + (long)t.selCharA;
            long b = (long)t.selLineB * 1000000L + (long)t.selCharB;
            if (a > b) { long tmp = a; a = b; b = tmp; }
            return ka >= a && ka <= b;
        }
        int LineLen(Term t, int line)
        {
            string s = (line < t.count) ? RingAt(t, line) : U.Cat(PS1(), t.input);
            return ParseLine(s).len;
        }
        int[] WordBounds(Term t, int line, int chr)
        {
            string s = (line < t.count) ? RingAt(t, line) : U.Cat(PS1(), t.input);
            PR pr = ParseLine(s);
            int start = WordStartC(pr.ch, pr.len, chr), end = WordEndC(pr.ch, pr.len, chr);
            return new int[] { start, end };
        }
        bool IsWord(char c) { return IsWordChar(c); }
        int WordStart(string s, int pos)
        {
            int i = pos; while (i > 0 && !IsWord(s[i - 1])) i--;
            while (i > 0 && IsWord(s[i - 1])) i--; return i;
        }
        int WordEnd(string s, int pos)
        {
            int i = pos, n = s.Length;
            while (i < n && !IsWord(s[i])) i++;
            while (i < n && IsWord(s[i])) i++; return i;
        }
        int WordStartC(char[] s, int n, int pos)
        {
            int i = pos; while (i > 0 && !IsWordChar(s[i - 1])) i--;
            while (i > 0 && IsWordChar(s[i - 1])) i--; return i;
        }
        int WordEndC(char[] s, int n, int pos)
        {
            int i = pos;
            while (i < n && !IsWordChar(s[i])) i++;
            while (i < n && IsWordChar(s[i])) i++; return i;
        }
        string GetSelText(Term t)
        {
            if (!t.hasSel) return "";
            if (t.selLineA > t.selLineB || (t.selLineA == t.selLineB && t.selCharA > t.selCharB))
            {
                int tl = t.selLineA; t.selLineA = t.selLineB; t.selLineB = tl;
                int tc = t.selCharA; t.selCharA = t.selCharB; t.selCharB = tc;
            }
            string r = "";
            for (int L = t.selLineA; L <= t.selLineB; L++)
            {
                PR pr = (L < t.count) ? ParseLine(RingAt(t, L)) : ParseLine(U.Cat(PS1(), t.input));
                int s0 = (L == t.selLineA) ? t.selCharA : 0;
                int e0 = (L == t.selLineB) ? t.selCharB : pr.len;
                if (e0 > s0) { for (int k2 = s0; k2 < e0; k2++) r = U.Cat(r, Host.CharStr((int)pr.ch[k2])); }
                if (L < t.selLineB) r = U.Cat(r, "\n");
            }
            return r;
        }
        int MaxView(Term t) { return (lastTotalRows > rows) ? (lastTotalRows - rows) : 0; }

        // Wallpaper hue at a given *screen* Y, blended 55% toward the Ubuntu
        // terminal purple.  Used when TermBgMode == 1 to make the terminal
        // background echo the live desktop behind it (pure-C# transparency
        // approximation -- the framebuffer has no alpha channel).
        int WallColorAt(int sy)
        {
            int H = Gfx.ScreenH(); if (H < 1) H = 1;
            int t = (sy * 100) / H; if (t < 0) t = 0; if (t > 100) t = 100;
            int a = (int)Theme.WallTop, b = (int)Theme.WallBot;
            int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
            int br = (b >> 16) & 0xFF, bg2 = (b >> 8) & 0xFF, bb = b & 0xFF;
            int r = (ar * (100 - t) + br * t) / 100;
            int g = (ag * (100 - t) + bg2 * t) / 100;
            int bl = (ab * (100 - t) + bb * t) / 100;
            int ur = (BG >> 16) & 0xFF, ug = (BG >> 8) & 0xFF, ub = BG & 0xFF;
            r = (r * 45 + ur * 55) / 100;
            g = (g * 45 + ug * 55) / 100;
            bl = (bl * 45 + ub * 55) / 100;
            return (r << 16) | (g << 8) | bl;
        }

        // Cycle the terminal cell size through a discrete set.  dir +1 = zoom
        // in, -1 = zoom out.  Pure int math (no BCL arrays-of-literals needed).
        public static int ZoomStep(int cur, int dir)
        {
            int[] steps = new int[6];
            steps[0] = 14; steps[1] = 16; steps[2] = 18; steps[3] = 20; steps[4] = 22; steps[5] = 24;
            int idx = 2;
            for (int i = 0; i < 6; i++) if (steps[i] == cur) { idx = i; break; }
            idx += dir; if (idx < 0) idx = 0; if (idx > 5) idx = 5;
            return steps[idx];
        }

        // ---- tabs ----------------------------------------------------
        void NewTab()
        {
            if (tabN >= TAB_MAX) return;
            tabs[tabN] = new Term(); tabN++; active = tabN - 1;
        }
        void CloseTab()
        {
            if (tabN <= 1) { Shell.Close(this.id); return; }
            for (int i = active; i < tabN - 1; i++) tabs[i] = tabs[i + 1];
            tabN--; if (active >= tabN) active = tabN - 1;
        }

        // ---- commands -------------------------------------------------
        void Run()
        {
            Term t = T();
            string cmd = t.input;
            t.input = ""; t.caret = 0; t.hasSel = false; t.search = false; t.sbuf = "";
            t.histPos = t.histN;
            AppendLine(t, U.Cat(PS1(), cmd));
            if (Trim(cmd).Length == 0) return;
            if (t.histN == 0 || t.hist[t.histN - 1] != cmd)
            {
                if (t.histN < HIST_MAX) { t.hist[t.histN] = cmd; t.histN++; }
            }
            t.histPos = t.histN;
            if (cmd == "exit" || cmd == "logout") { CloseTab(); return; }
            if (cmd == "clear" || cmd == "cls") { ClearScreen(t); return; }
            string outp = RunBuiltin(t, cmd);
            if (outp == null) outp = Host.Exec(cmd);
            if (outp != null && outp.Length > 0) AppendLine(t, outp);
        }
        string RunBuiltin(Term t, string cmd)
        {
            string c = Trim(cmd), low = Lower(c);
            if (low == "help") return "NexOS shell - builtins: help ver mem date time ls cat echo clear pwd whoami history uname exit";
            if (low == "ver") return "NexOS 1.0 / .NET";
            if (low == "mem") return U.Cat("total ", U.I(Host.MemTotalKb() / 1024), " MB");
            if (low == "date" || low == "time") return U.Cat(U.I(Host.Hour()), ":", Pad2(Host.Minute()), ":", Pad2(Host.Second()));
            if (low == "whoami") return user;
            if (low == "pwd") return U.Cat("/home/", user);
            if (low == "uname") return "NexOS";
            if (low == "history")
            {
                string r = "";
                for (int i = 0; i < t.histN; i++) r = U.Cat(r, U.I(i + 1), "  ", t.hist[i], "\n");
                return r;
            }
            if (low == "ls")
            {
                string r = "";
                for (int fs = 0; fs < 2; fs++)
                {
                    int n = Host.FileCount(fs);
                    for (int i = 0; i < n && i < 40; i++) r = U.Cat(r, Host.FileName(fs, i), " ");
                }
                return r.Length > 0 ? r : "(empty)";
            }
            if (StartsWith(low, "cat "))
            {
                string f = Trim(U.Sub(c, 4, c.Length - 4));
                string b = Host.ReadText(1, f); if (b == null || b.Length == 0) b = Host.ReadText(0, f);
                if (b == null || b.Length == 0) return U.Cat("cat: ", f, ": No such file");
                return b;
            }
            if (StartsWith(low, "echo ")) return U.Sub(c, 5, c.Length - 5);
            return null;
        }

        // ---- keyboard ------------------------------------------------
        public override void OnKey(int ch)
        {
            Term t = T();
            if (t.search) { KeySearch(t, ch); return; }
            if (ch >= 32) { t.input = InsStr(t.input, t.caret, Host.CharStr((char)ch)); t.caret++; t.hasSel = false; return; }
            switch (ch)
            {
                case VK.Enter: Run(); break;
                case VK.Back: if (t.caret > 0) { t.input = DelStr(t.input, t.caret - 1, 1); t.caret--; } break;
                case VK.Delete: if (t.caret < t.input.Length) t.input = DelStr(t.input, t.caret, 1); break;
                case VK.CtrlC:
                    if (t.hasSel) Host.SetClipboard(GetSelText(t));
                    else { AppendLine(t, U.Cat(PS1(), "^C")); t.input = ""; t.caret = 0; t.hasSel = false; }
                    break;
                case VK.CtrlV:
                case VK.CsV:
                    { string cb = Host.GetClipboard(); if (cb != null && cb.Length > 0) { t.input = InsStr(t.input, t.caret, cb); t.caret += cb.Length; } }
                    break;
                case VK.CsC: if (t.hasSel) Host.SetClipboard(GetSelText(t)); break;
                case VK.CsT: NewTab(); break;          // Ctrl+Shift+T new tab
                case VK.CsW: CloseTab(); break;        // Ctrl+Shift+W close tab
                case VK.CtrlA: case VK.HomeK: t.caret = 0; break;
                case VK.CtrlE: case VK.EndK: t.caret = t.input.Length; break;
                case VK.CtrlU: t.input = U.Sub(t.input, t.caret, t.input.Length - t.caret); t.caret = 0; break;
                case VK.CtrlK: t.input = U.Sub(t.input, 0, t.caret); break;
                case VK.CtrlW: { int s = WordStart(t.input, t.caret); t.input = DelStr(t.input, s, t.caret - s); t.caret = s; } break;
                case VK.CtrlL: ClearScreen(t); break;
                case VK.CtrlD: if (t.input.Length == 0) CloseTab(); break;
                case VK.CtrlZ: AppendLine(t, "[1]+  Stopped"); break;
                case VK.CtrlR: t.search = true; t.sbuf = ""; break;
                case VK.AltF: t.caret = WordEnd(t.input, t.caret); break;
                case VK.AltB: t.caret = WordStart(t.input, t.caret); break;
                case VK.Tab: Complete(t); break;
                case VK.Up: HistUp(t); break;
                case VK.Down: HistDown(t); break;
                case VK.Left: if (t.caret > 0) t.caret--; break;
                case VK.Right: if (t.caret < t.input.Length) t.caret++; break;
                case VK.PageUp: t.view += rows; if (t.view > MaxView(t)) t.view = MaxView(t); break;
                case VK.PageDown: t.view -= rows; if (t.view < 0) t.view = 0; break;
                case VK.Esc: t.hasSel = false; break;
                case VK.CtrlPlus:  Theme.TermCellH = ZoomStep(Theme.TermCellH, 1);  Theme.Save(); break;
                case VK.CtrlMinus: Theme.TermCellH = ZoomStep(Theme.TermCellH, -1); Theme.Save(); break;
                case VK.Ctrl0:     Theme.TermCellH = 18; Theme.Save(); break;
                default:
                    if (ch <= -40 && ch >= -51) AppendLine(t, U.Cat("F", U.I(-ch - 39), " pressed"));
                    break;
            }
        }
        void KeySearch(Term t, int ch)
        {
            if (ch == VK.Enter) { t.search = false; t.sbuf = ""; return; }
            if (ch == VK.Esc) { t.search = false; t.sbuf = ""; t.input = ""; t.caret = 0; return; }
            if (ch == VK.Back) { if (t.sbuf.Length > 0) t.sbuf = U.Sub(t.sbuf, 0, t.sbuf.Length - 1); }
            else if (ch >= 32) t.sbuf = U.Cat(t.sbuf, Host.CharStr((char)ch));
            else return;
            string low = Lower(t.sbuf);
            for (int i = t.histN - 1; i >= 0; i--)
                if (IndexOf(Lower(t.hist[i]), low) >= 0) { t.input = t.hist[i]; t.caret = t.input.Length; break; }
        }
        void HistUp(Term t)
        {
            if (t.histN == 0) return;
            if (t.histPos > 0) t.histPos--;
            t.input = (t.histPos < t.histN) ? t.hist[t.histPos] : "";
            t.caret = t.input.Length;
        }
        void HistDown(Term t)
        {
            if (t.histPos < t.histN) t.histPos++;
            t.input = (t.histPos >= t.histN) ? "" : t.hist[t.histPos];
            t.caret = t.input.Length;
        }
        void Complete(Term t)
        {
            string[] cmds = { "help","ver","mem","date","time","ls","cat","echo",
                              "clear","pwd","whoami","history","uname","exit","net" };
            string tok = t.input; int m = 0; string first = "";
            for (int i = 0; i < cmds.Length; i++)
                if (StartsWith(cmds[i], tok)) { m++; if (m == 1) first = cmds[i]; }
            if (m == 1) { t.input = first; t.caret = first.Length; }
            else if (m > 1)
            {
                string r = "";
                for (int i = 0; i < cmds.Length; i++) if (StartsWith(cmds[i], tok)) r = U.Cat(r, cmds[i], " ");
                AppendLine(t, r);
            }
        }

        // ---- mouse ---------------------------------------------------
        public override void OnMouseDown(int btn, int mx, int my)
        {
            if (my < TAB_H)   // tab bar: switch
            {
                int tw = 140;
                for (int i = 0; i < tabN; i++)
                {
                    int tx = contentX + i * (tw + 4);
                    if (mx >= tx && mx < tx + tw) { active = i; break; }
                }
                return;
            }
            Term t = T();
            if (btn == 1)   // middle-click paste
            {
                string cb = Host.GetClipboard();
                if (cb != null && cb.Length > 0) { t.input = InsStr(t.input, t.caret, cb); t.caret += cb.Length; }
                return;
            }
            int[] cell = CellAt(mx, my); if (cell[0] < 0) return;
            int line = cell[0], chr = cell[1];
            int now = Host.TickMs(), cellKey = line * 100000 + chr;
            if (now - lastDownMs < 400 && cellKey == lastDownCell) clickCount++; else clickCount = 1;
            if (clickCount > 3) clickCount = 3;
            lastDownMs = now; lastDownCell = cellKey;
            if (clickCount >= 3)
            {
                int L = LineLen(t, line);
                t.selLineA = line; t.selCharA = 0; t.selLineB = line; t.selCharB = L; t.hasSel = true; t.dragging = false;
            }
            else if (clickCount == 2)
            {
                int[] wb = WordBounds(t, line, chr);
                t.selLineA = line; t.selCharA = wb[0]; t.selLineB = line; t.selCharB = wb[1]; t.hasSel = true; t.dragging = false;
            }
            else
            {
                t.dragging = true;
                t.selLineA = line; t.selCharA = chr; t.selLineB = line; t.selCharB = chr; t.hasSel = false;
            }
        }
        public override void OnMouseMove(int mx, int my)
        {
            hoverX = mx; hoverY = my;
            Term t = T();
            if (my < TAB_H) { hoverLine = -1; hoverChar = -1; }
            else if (gN > 0)
            {
                int[] cell = CellAt(mx, my);
                if (cell[0] >= 0) { hoverLine = cell[0]; hoverChar = cell[1]; }
                else { hoverLine = -1; hoverChar = -1; }
            }
            if (!t.dragging) return;
            if (my < TAB_H) return;
            int[] c2 = CellAt(mx, my); if (c2[0] < 0) return;
            t.selLineB = c2[0]; t.selCharB = c2[1]; t.hasSel = true;
        }
        public override void OnMouseUp(int btn, int mx, int my)
        {
            Term t = T();
            if (t.dragging) t.dragging = false;
            // GNOME Terminal copies to the PRIMARY selection the moment a
            // selection is finalised -- by drag-release OR by double / triple
            // click.  Mirror that: any active selection lands on the clipboard.
            if (t.hasSel) Host.SetClipboard(GetSelText(t));
        }
        public override void OnWheel(int dy)
        {
            Term t = T();
            if (dy > 0) { t.view++; if (t.view > MaxView(t)) t.view = MaxView(t); }
            else if (dy < 0) { t.view--; if (t.view < 0) t.view = 0; }
        }
        public override void OnClick(int mx, int my) { }

        // ---- paint ---------------------------------------------------
        void DrawTabBar()
        {
            Gfx.FillRect(0, 0, Gfx.Width(), TAB_H, 0x1A0820);
            int tw = 140;
            for (int i = 0; i < tabN; i++)
            {
                int tx = contentX + i * (tw + 4), ty = 4, th = TAB_H - 8;
                bool act = (i == active);
                Gfx.FillRound(tx, ty, tw, th, 4, (uint)(act ? 0x3465A4 : 0x2A1030));
                Gfx.Text(tx + 10, ty + (th - 16) / 2, tabs[i].title, (uint)(act ? 0xFFFFFF : 0xC0C0C0));
                if (act) Gfx.Text(tx + tw - 16, ty + (th - 16) / 2, "x", 0xDDDDDD);
            }
        }
        void DrawScrollbar(Term t)
        {
            int x = contentX + contentW + 2, y = contentY, w = 6, h = contentH;
            Gfx.FillRect(x, y, w, h, 0x1A0820);
            int total = lastTotalRows; if (total < 1) total = 1;
            int thumbH = h, ty = y;
            if (total > rows)
            {
                thumbH = (h * rows) / total; if (thumbH < 10) thumbH = 10;
                int maxOff = h - thumbH;
                int off = (total <= rows) ? 0 : (maxOff * (total - rows - t.view) / (total - rows));
                ty = y + off;
            }
            Gfx.FillRect(x, ty, w, thumbH, 0x555555);
        }
        public override void OnPaint()
        {
            int W = Gfx.Width(), H = Gfx.Height();
            Term t = T();
            ComputeColsRows();
            ComputeLayout(t);
            DrawTabBar();
            uint baseBg = (Theme.TermBgMode != 0)
                ? (uint)WallColorAt(Gfx.OriginY() + contentY)
                : (uint)BG;
            Gfx.FillRect(0, 0, W, H, baseBg);
            for (int vr = 0; vr < rows; vr++)
            {
                int y = contentY + vr * cellH;
                int absRow = firstAbsRow + vr;
                if (absRow < 0 || absRow >= rowN) continue;
                PR pr = prs[rowLine[absRow]];
                int start = rowStart[absRow], end = rowEnd[absRow];
                int isInput = (rowLine[absRow] == t.count) ? 1 : 0;
                uint rowBg = (Theme.TermBgMode != 0)
                    ? (uint)WallColorAt(Gfx.OriginY() + y)
                    : (uint)BG;
                Gfx.FillRect(0, y, W, cellH, rowBg);   // band covers full width + margins
                int defBg = (int)rowBg;
                for (int i = start; i < end; i++)
                {
                    int xc = contentX + (i - start) * cellW;
                    int ch = (int)pr.ch[i];
                    int fg = pr.fg[i]; if (fg < 0) fg = FG;
                    int bg = pr.bg[i]; if (bg < 0) bg = defBg;
                    bool sel = CellSelected(t, rowLine[absRow], i);
                    if (sel) Gfx.FillRect(xc, y, cellW, cellH, SEL);
                    // URL hover highlight: expand from hoverChar to the URL span
                    bool hot = false;
                    if (hoverLine == rowLine[absRow] && pr.url != null && pr.url[i] != 0)
                    {
                        int s = i, e = i;
                        while (s - 1 >= start && pr.url[s - 1] != 0) s--;
                        while (e + 1 < end && pr.url[e + 1] != 0) e++;
                        if (hoverChar >= s && hoverChar <= e) hot = true;
                    }
                    if (ch != ' ' && ch != '\t')
                    {
                        string s2 = Host.CharStr((char)ch);
                        if (hot) fg = URLC;
                        if (bg == defBg)
                        {
                            if (hot) Gfx.TextBg(xc, y, s2, (uint)fg, rowBg);
                            else Gfx.Text(xc, y, s2, (uint)fg);
                        }
                        else Gfx.TextBg(xc, y, s2, (uint)fg, (uint)bg);
                    }
                    if (hot) Gfx.FillRect(xc, y + cellH - 2, cellW, 2, URLC);
                    if (isInput != 0 && i == t.caret)
                    {
                        if (t.hasSel) Gfx.FillRect(xc, y, 2, cellH, 0xFFFFFF);          // I-beam
                        else
                        {
                            Gfx.FillRect(xc, y, cellW, cellH, 0xFFFFFF);                 // block
                            if (ch != ' ' && ch != '\t') Gfx.Text(xc, y, Host.CharStr((char)ch), (uint)defBg);
                        }
                    }
                }
            }
            DrawScrollbar(t);
        }
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
        bool saved;    // last action was a successful Ctrl+S (title hint)

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
            saved = false;
        }

        // Ctrl+S save: persist the document body to the MKFS data disk.
        // Mirrors the ReadText(0, name) fallback used when a file is opened,
        // so a re-open lands on the saved copy.  Silent on purpose -- the
        // title shows a "(saved)" hint until the next edit.
        void Save()
        {
            Host.WriteText(0, name, t.text);
            saved = true;
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
            if (saved) { int sw = Gfx.Measure(name); Gfx.Text(pad + 178 + sw, 12, "(saved)", 0x107C10); }

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
            if (ch == -10) { Save(); return; }          // Ctrl+S -> save
            if (ch == -2) { t.Insert("\n"); saved = false; return; }
            t.Key(ch);
            saved = false;
        }
    }
}
