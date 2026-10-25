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
    // -----------------------------------------------------------------
    //  Reworked to feel like a modern Windows (Win11) Explorer:
    //    * Win11-style command bar (Back / Forward / Up / New Folder /
    //      Refresh / View toggle) with a real navigation history.
    //    * A "This PC" root that lists the two volumes as drives, so
    //      Back / Forward / Up actually mean something.
    //    * Two layouts: the classic icon+name List, and a Details view
    //      with sortable Name / Type columns and a status bar.
    //    * Multi-select (Ctrl+A selects all; a click selects one) that
    //      drives the selection count in the status bar and bulk delete.
    //  The file system is still flat (MKFS / SFS volumes, no sub-folders
    //  and no per-file size/date), so the columns reflect what the host
    //  actually exposes: name and type.
    // =================================================================
    public class FileExplorerApp : App
    {
        // Location: -1 = This PC (root), 0 = MKFS, 1 = SFS.
        int fs;        // current location
        int sel;       // focused / primary list index, -1 = none
        int scroll;    // first visible row (native scrollbar drives this)
        int editMode;  // 0 none, 1 rename, 2 new folder
        string editBuf;// current text in the inline editor
        string editOld;// original name (rename source)
        bool editDirty;// true once the user has started editing the buffer
        int dblT;      // TickMs() of the last row click (double-click detect)
        int dblIdx;    // row index of that click
        static string clip;   // last "copied" path

        // Address bar (location box) at the top of the window.
        string addr;     // committed address text, e.g. "C:\"
        string addrBuf;  // scratch buffer while editing
        bool   addrEdit; // true while the box owns the keyboard
        bool   addrDirty;// true once the user typed -- first key replaces the text
        bool   addrErr;  // true while the "invalid address" dialog is up
        bool   openWith; // true while the "open with" chooser is up
        string owFile;   // file being opened by the chooser
        string owExt;    // its extension (recorded on pick)

        // Navigation history for Back / Forward.
        int[] hist;  int histN; int histPos;

        // View + sort state.
        int viewMode;   // 0 = List, 1 = Details
        int sortMode;   // 0 name asc, 1 name desc, 2 type asc, 3 type desc

        // Multi-select set (list indices).  `sel` is the keyboard focus.
        int[] selSet; int selN; int anchor;

        // Display-order permutation of list indices (for sorting).
        int[] order;

        // Toolbar button geometry (filled during paint, read during click).
        int[] tbX; int[] tbY; int[] tbW; int[] tbH;
        const int TB_BACK = 1, TB_FWD = 2, TB_UP = 3, TB_NEW = 4, TB_REFRESH = 5, TB_VIEW = 6;
        // Details header geometry.
        int hdY; int hdH; int hdNameX; int hdNameW; int hdTypeX; int hdTypeW;

        // Semi-blocking open plumbing.
        int    pendKind = 0;
        int    pendArg2 = 0;
        string pendName = null;
        string pendWhat = "";

        const int NAVW = 150;
        const int PAD = 12;

        public FileExplorerApp()
        {
            fs = 0; sel = -1; scroll = 0; editMode = 0; editBuf = ""; editOld = ""; editDirty = false;
            dblT = -100000; dblIdx = -1;
            addr = "MKFS"; addrBuf = ""; addrEdit = false; addrDirty = false;
            addrErr = false; openWith = false; owFile = ""; owExt = "";
            hist = new int[64]; histN = 0; histPos = 0;
            viewMode = 0; sortMode = 0;
            selSet = new int[256]; selN = 0; anchor = -1;
            order = new int[256];
            tbX = new int[8]; tbY = new int[8]; tbW = new int[8]; tbH = new int[8];
            hdY = 0; hdH = 26; hdNameX = 0; hdNameW = 0; hdTypeX = 0; hdTypeW = 0;
            // Seed the history with the default location.
            hist[0] = 0; histN = 1; histPos = 0;
            RebuildOrder();
        }

        public override string GetTitle() { return "File Explorer"; }

        // ---- location helpers -------------------------------------
        // Wrappers so the rest of the app can treat "This PC" (fs < 0) and a
        // real volume uniformly: This PC lists the two volumes as folders.
        int ListCount()
        {
            if (fs < 0) return 2;
            int n = Host.FileCount(fs);
            if (n > 256) n = 256;
            return n;
        }
        string ListName(int i)
        {
            if (fs < 0) return i == 0 ? "Local Disk (MKFS)" : "System (SFS)";
            if (i < 0 || i >= Host.FileCount(fs)) return "";
            return Host.FileName(fs, i);
        }
        bool ListIsDir(int i)
        {
            if (fs < 0) return true;          // volumes render as folders
            if (i < 0 || i >= Host.FileCount(fs)) return false;
            return Host.FileIsDir(fs, i) != 0;
        }

        // Navigate to a location, recording it in the history (Back/Forward).
        void Go(int loc)
        {
            if (histN == 0 || hist[histPos] != loc)
            {
                if (histPos < histN - 1) histN = histPos + 1;   // drop redo branch
                if (histN < 64) { hist[histN] = loc; histN++; histPos = histN - 1; }
            }
            fs = loc; sel = -1; scroll = 0; anchor = -1; selN = 0;
            addr = AddrOf(loc);
            Host.FileRefresh();
            RebuildOrder();
        }
        // Jump within the existing history (no new entry).
        void ApplyHist()
        {
            int loc = hist[histPos];
            fs = loc; sel = -1; scroll = 0; anchor = -1; selN = 0;
            addr = AddrOf(loc);
            Host.FileRefresh();
            RebuildOrder();
        }
        void GoVolume(int nf) { Go(nf); }     // legacy name, volume only
        void Back()    { if (histPos > 0) { histPos--; ApplyHist(); } }
        void Forward() { if (histPos < histN - 1) { histPos++; ApplyHist(); } }
        void NavigateUp() { if (fs >= 0) Go(-1); }   // volume -> This PC

        // ---- selection helpers -------------------------------------
        bool IsSelected(int li)
        {
            for (int k = 0; k < selN; k++) if (selSet[k] == li) return true;
            return false;
        }
        void SelectOnly(int li) { sel = li; anchor = li; selN = 0; if (selN < 256) { selSet[selN] = li; selN++; } }
        void SelectAll()
        {
            int n = ListCount(); selN = 0;
            for (int i = 0; i < n && selN < 256; i++) { selSet[selN] = i; selN++; }
            if (n > 0) { sel = 0; anchor = 0; }
        }
        void ClearSel() { selN = 0; sel = -1; anchor = -1; }

        // ---- sorting -------------------------------------------------
        static int StrCmp(string a, string b)
        {
            int n = a.Length; if (b.Length < n) n = b.Length;
            for (int i = 0; i < n; i++)
            {
                int ca = (int)a[i], cb = (int)b[i];
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca < cb) return -1;
                if (ca > cb) return 1;
            }
            if (a.Length < b.Length) return -1;
            if (a.Length > b.Length) return 1;
            return 0;
        }
        // Returns > 0 if list-index `a` should sort AFTER `b` (for ascending).
        int Cmp(int a, int b)
        {
            string na = ListName(a), nb = ListName(b);
            bool da = ListIsDir(a), db = ListIsDir(b);
            if (sortMode == 0 || sortMode == 1)        // by name
            {
                int c = StrCmp(na, nb);
                if (sortMode == 1) c = -c;
                return c;
            }
            // by type: folders first, then by extension, then by name
            if (da && !db) return sortMode == 2 ? -1 : 1;
            if (!da && db) return sortMode == 2 ? 1 : -1;
            int ce = StrCmp(ExtOf(na), ExtOf(nb));
            if (ce == 0) ce = StrCmp(na, nb);
            if (sortMode == 3) ce = -ce;
            return ce;
        }
        void RebuildOrder()
        {
            int n = ListCount();
            for (int i = 0; i < n; i++) order[i] = i;
            for (int a = 0; a < n; a++)
                for (int b = a + 1; b < n; b++)
                    if (Cmp(order[a], order[b]) > 0)
                    { int t = order[a]; order[a] = order[b]; order[b] = t; }
        }
        string TypeStr(int li)
        {
            if (ListIsDir(li)) return "Folder";
            string e = ExtOf(ListName(li));
            if (e == "") return "File";
            // uppercase the extension for display: "TXT File"
            string r = "";
            for (int i = 0; i < e.Length; i++) { int c = (int)e[i]; if (c >= 'a' && c <= 'z') c -= 32; r = U.Cat(r, Host.CharStr(c)); }
            return U.Cat(r, " File");
        }

        // ---- layout --------------------------------------------------
        int BarTop() { return PAD; }
        int BarH()   { return 30; }
        int ToolY()  { return BarTop() + BarH() + 6; }
        int ListTop() { return ToolY() + 32 + 8; }
        int StatusH() { return 24; }
        int ClientW(int w) { return w - (NAVW + PAD) - PAD; }
        int ClientX() { return NAVW + PAD; }

        bool TBEnabled(int id)
        {
            if (id == TB_BACK)    return histPos > 0;
            if (id == TB_FWD)     return histPos < histN - 1;
            if (id == TB_UP)      return fs >= 0;
            if (id == TB_NEW)     return fs >= 0;
            if (id == TB_REFRESH) return true;
            if (id == TB_VIEW)    return true;
            return false;
        }
        // Recompute + paint one toolbar button; caches its rect for click tests.
        void ToolBtn(int id, int x, int y, int w, int h, string glyph, bool enabled)
        {
            bool hot = enabled && W.Hot(x, y, w, h);
            uint bg = !enabled ? C.Card : (hot ? C.Hover : C.Card);
            Gfx.FillRound(x, y, w, h, 6, bg);
            Gfx.DrawRound(x, y, w, h, 6, enabled ? C.BorderMid : C.Border);
            uint fg = enabled ? C.Text : C.TextFaint;
            Gfx.TextCenter(x, y + (h - 16) / 2, w, glyph, fg);
            tbX[id] = x; tbY[id] = y; tbW[id] = w; tbH[id] = h;
        }

        // =============================================================
        //  PAINT
        // =============================================================
        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();
            int ax = ClientX(), aw = ClientW(w);

            // ---- left navigation rail ----
            Gfx.FillRect(0, 0, NAVW, h, 0xFAFAFA);
            Gfx.DrawLine(NAVW, 0, NAVW, h, C.Border);
            Nav(10, 14,           NAVW - 20, "This PC", fs < 0);
            Nav(10, 14 + 40,      NAVW - 20, "MKFS",    fs == 0);
            Nav(10, 14 + 80,      NAVW - 20, "SFS",     fs == 1);

            // ---- address (location) bar ----
            int ay = BarTop(), bh = BarH();
            Gfx.FillRound(ax, ay, aw, bh, 6, addrEdit ? 0xFFFFFF : C.Card);
            Gfx.DrawRound(ax, ay, aw, bh, 6, addrEdit ? C.Accent : C.Border);
            Gfx.Icon(ax + 8, ay + (bh - 18) / 2, 18, 0x0078D4u, 'P', 0xFFFFFF);
            string shownAddr = addrEdit ? addrBuf : addr;
            if (addrEdit && (Host.Ticks() / 30) % 2 == 0) shownAddr = U.Cat(shownAddr, "|");
            Gfx.Text(ax + 32, ay + (bh - 16) / 2, shownAddr, C.Text);

            // ---- command toolbar (Win11-style) ----
            int by = ToolY(), bs = 30, bg = 6;
            int tx = ax;
            ToolBtn(TB_BACK,    tx, by, bs, bs, "<", TBEnabled(TB_BACK));    tx += bs + bg;
            ToolBtn(TB_FWD,     tx, by, bs, bs, ">", TBEnabled(TB_FWD));     tx += bs + bg;
            ToolBtn(TB_UP,      tx, by, bs, bs, "^", TBEnabled(TB_UP));      tx += bs + bg;
            tx += 10;
            ToolBtn(TB_NEW,     tx, by, bs, bs, "+", TBEnabled(TB_NEW));     tx += bs + bg;
            ToolBtn(TB_REFRESH, tx, by, bs, bs, "R", TBEnabled(TB_REFRESH)); tx += bs + bg;
            tx += 10;
            ToolBtn(TB_VIEW,    tx, by, 72, bs, viewMode == 0 ? "Details" : "List", true);

            // ---- file list / details area ----
            int ly = ListTop();
            int n = ListCount();
            int rows = (h - ly - StatusH() - PAD) / W.RowH;

            if (viewMode == 1)   // Details: column header + aligned rows
            {
                hdY = ly; hdH = 26;
                hdNameX = ax + 40;                        // clear the 22 px type icon
                hdNameW = ((aw - 52) * 58) / 100;
                hdTypeX = hdNameX + hdNameW + 24;
                hdTypeW = ax + aw - hdTypeX - 12;
                int hy = ly;
                Gfx.FillRound(ax, hy, aw, hdH, 6, C.CardAlt);
                Gfx.DrawRound(ax, hy, aw, hdH, 6, C.Border);
                // sort indicators
                string nameArrow = (sortMode == 0) ? " ▲" : (sortMode == 1) ? " ▼" : "";
                string typeArrow = (sortMode == 2) ? " ▲" : (sortMode == 3) ? " ▼" : "";
                Gfx.Text(hdNameX, hy + 5, U.Cat("Name", nameArrow), C.Text);
                Gfx.Text(hdTypeX, hy + 5, U.Cat("Type", typeArrow), C.Text);
                ly = hy + hdH + 2;
            }

            for (int d = 0; d < n; d++)
            {
                int li = order[d];
                int y = ly + d * W.RowH;
                bool isDir = ListIsDir(li);
                string nm = ListName(li);
                bool isSel = IsSelected(li);
                bool focused = (li == sel);
                if (isSel)  Gfx.FillRound(ax, y, aw, W.RowH - 2, 6, focused ? C.Sel : 0xEFEFEF);
                else if (W.Hot(ax, y, aw, W.RowH - 2)) Gfx.FillRound(ax, y, aw, W.RowH - 2, 6, C.Hover);
                if (viewMode == 0)
                {
                    Gfx.Icon(ax + 8, y + 4, 22, isDir ? 0xF7C948u : 0x60A5FAu, isDir ? 'D' : 'F', 0xFFFFFF);
                    Gfx.Text(ax + 40, y + 8, nm, focused ? C.Text : C.Text);
                }
                else
                {
                    Gfx.Icon(ax + 8, y + 4, 22, isDir ? 0xF7C948u : 0x60A5FAu, isDir ? 'D' : 'F', 0xFFFFFF);
                    Gfx.Text(hdNameX, y + 8, nm, C.Text);
                    Gfx.Text(hdTypeX, y + 8, TypeStr(li), C.TextSub);
                }
            }

            // ---- inline rename / new-folder editor ----
            if (editMode != 0)
            {
                int inputY = ly;
                if (editMode == 1 && sel >= 0) inputY = ly + RowOf(sel) * W.RowH;
                if (editMode == 2) inputY = ly;   // new folder name typed at top
                int bx = ax + 4, bw = aw - 8, ebh = W.RowH - 2;
                Gfx.FillRound(bx, inputY, bw, ebh, 4, 0xFFFFFFFF);
                Gfx.DrawRound(bx, inputY, bw, ebh, 4, C.Accent);
                string shown = editBuf;
                if ((Host.Ticks() / 30) % 2 == 0) shown = U.Cat(shown, "|");
                Gfx.Text(bx + 6, inputY + 6, shown, C.Text);
                if (editMode == 1)
                    Gfx.Text(ax + 4, inputY + W.RowH - 4, "Enter 确认 · Esc 取消", C.TextSub);
            }

            // ---- status bar ----
            int sy = h - StatusH();
            Gfx.FillRect(0, sy, w, StatusH(), C.CardAlt);
            Gfx.DrawLine(0, sy, w, sy, C.Border);
            string status = U.Cat(U.I(n), " items");
            if (selN > 1) status = U.Cat(status, U.Cat("  ·  ", U.Cat(U.I(selN), " selected")));
            else if (selN == 1) status = U.Cat(status, "  ·  1 selected");
            Gfx.Text(PAD, sy + 5, status, C.TextSub);

            // ---- invalid-address dialog (in-window modal) ----
            if (addrErr)
            {
                int dw = 320; if (dw > w - 40) dw = w - 40;
                int dh = 120;
                int dx = (w - dw) / 2, dy = (h - dh) / 2;
                Gfx.FillRound(dx, dy, dw, dh, 8, 0xFFFFFFFF);
                Gfx.DrawRound(dx, dy, dw, dh, 8, C.Accent);
                Gfx.DrawLine(dx + 1, dy + 44, dx + dw - 1, dy + 44, C.Border);
                Gfx.TextCenter(dx, dy + 16, dw, "你输入的地址无效", C.Text);
                int dbw = 96, dbh = 30;
                int dbx = dx + (dw - dbw) / 2, dby = dy + dh - dbh - 16;
                Gfx.FillRound(dbx, dby, dbw, dbh, 6, C.Accent);
                Gfx.TextCenter(dbx, dby + (dbh - 16) / 2, dbw, "确定", C.White);
            }

            // ---- "Open with" chooser (in-window modal) ----
            if (openWith)
            {
                int rowH = OwRowH();
                int pw = 300; if (pw > w - 40) pw = w - 40;
                int ph = 34 + OW_N * rowH + 26;
                int px = (w - pw) / 2, py = (h - ph) / 2;
                Gfx.FillRound(px, py, pw, ph, 8, 0xFFFFFFFF);
                Gfx.DrawRound(px, py, pw, ph, 8, C.Accent);
                Gfx.Text(px + 12, py + 8, U.Cat("选择打开方式：", owFile), C.TextSub);
                for (int i = 0; i < OW_N; i++)
                {
                    int ry = py + 34 + i * rowH;
                    bool hot = W.Hot(px + 6, ry, pw - 12, rowH - 2);
                    if (hot) Gfx.FillRound(px + 6, ry, pw - 12, rowH - 2, 4, C.Hover);
                    Gfx.Text(px + 20, ry + 8, OwLabel(i), C.Text);
                }
                Gfx.Text(px + 12, py + ph - 20, "选择后该后缀以后都用此应用打开（Esc 取消）", C.TextSub);
            }

            // ---- semi-blocking tail: paint scrim, flip, then block ----
            if (pendKind != 0)
            {
                int sx = ax, shy = ListTop();
                int sw = aw, sh2 = h - shy - StatusH() - PAD;
                if (sw > 40 && sh2 > 40)
                {
                    Gfx.FillRound(sx, shy, sw, sh2, 10, 0xF2FFFFFFu);
                    Gfx.TextCenter(sx, shy + sh2 / 2 - 8, sw, pendWhat, C.Text);
                }
                Host.Repaint();
                RunPending();
            }
        }

        // Map a list index to its display row (used to place the inline editor).
        int RowOf(int li)
        {
            int n = ListCount();
            for (int d = 0; d < n; d++) if (order[d] == li) return d;
            return 0;
        }

        static void Nav(int x, int y, int w, string label, bool active)
        {
            if (active) Gfx.FillRound(x, y, w, 30, 6, C.Sel);
            else if (W.Hot(x, y, w, 30)) Gfx.FillRound(x, y, w, 30, 6, C.Hover);
            Gfx.Text(x + 12, y + 7, label, active ? C.Accent : C.Text);
        }

        // ---- address bar (location box) -----------------------------
        // Canonical address text for a location.
        static string AddrOf(int f) { return f < 0 ? "This PC" : (f == 1 ? "SFS" : "MKFS"); }

        static string TrimLow(string s)
        {
            if (s == null) return "";
            int a = 0, b = s.Length;
            while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
            string r = "";
            for (int i = a; i < b; i++)
            {
                int c = (int)s[i];
                if (c >= 'A' && c <= 'Z') c = c + 32;
                r = U.Cat(r, Host.CharStr(c));
            }
            return r;
        }

        static int ParseAddr(string s)
        {
            string t = TrimLow(s);
            if (t.Length > 0)
            {
                int last = (int)t[t.Length - 1];
                if (last == '\\' || last == '/')
                {
                    string r = "";
                    for (int i = 0; i < t.Length - 1; i++) r = U.Cat(r, Host.CharStr((int)t[i]));
                    t = r;
                }
            }
            if (t == "this pc" || t == "pc" || t == "") return -1;   // This PC root
            if (t == "c:" || t == "c" || t == "mkfs" || t == "local disk (mkfs)" || t == "local disk") return 0;
            if (t == "s:" || t == "s" || t == "sfs" || t == "system (sfs)" || t == "system") return 1;
            return -2;   // genuinely unknown
        }

        void BeginAddrEdit()
        {
            addrEdit = true; addrDirty = false;
            addrBuf = addr;   // show current location; first keystroke replaces it
        }

        void CommitAddr()
        {
            addrEdit = false;
            int nf = ParseAddr(addrBuf);
            addrBuf = ""; addrDirty = false;
            if (nf >= -1) { Go(nf); return; }
            Go(0);               // unknown -> default volume
            ShowInvalidAddr();
        }

        void ShowInvalidAddr() { addrErr = true; }

        // =============================================================
        //  File-type associations + "Open with"  (unchanged behaviour)
        // =============================================================
        static string assocData;
        static bool   assocLoaded;

        static string FxSub(string s, int a, int b)
        {
            string r = "";
            for (int i = a; i < b && i < s.Length; i++) r = U.Cat(r, Host.CharStr((int)s[i]));
            return r;
        }
        static bool FxEq(string a, string b)
        {
            if (a == null || b == null) return false;
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++)
            {
                int x = (int)a[i], y = (int)b[i];
                if (x >= 'A' && x <= 'Z') x += 32;
                if (y >= 'A' && y <= 'Z') y += 32;
                if (x != y) return false;
            }
            return true;
        }
        static int FxAtoi(string s)
        {
            int v = 0;
            for (int i = 0; i < s.Length; i++)
            {
                int c = (int)s[i];
                if (c < '0' || c > '9') break;
                v = v * 10 + (c - '0');
            }
            return v;
        }
        static string ExtOf(string nm)
        {
            if (nm == null) return "";
            int dot = -1;
            for (int i = 0; i < nm.Length; i++) if (nm[i] == '.') dot = i;
            if (dot < 0 || dot == nm.Length - 1) return "";
            string r = "";
            for (int i = dot + 1; i < nm.Length; i++)
            {
                int c = (int)nm[i];
                if (c >= 'A' && c <= 'Z') c += 32;
                r = U.Cat(r, Host.CharStr(c));
            }
            return r;
        }
        static void LoadAssoc()
        {
            if (assocLoaded) return;
            assocData = Host.ReadText(0, "assoc.cfg");
            if (assocData == null) assocData = "";
            assocLoaded = true;
        }
        static int AssocOf(string ext)
        {
            LoadAssoc();
            string d = assocData;
            if (d == null) return -1;
            int i = 0, n = d.Length;
            while (i < n)
            {
                int j = i; while (j < n && d[j] != '\n') j++;
                int eq = -1;
                for (int k = i; k < j; k++) if (d[k] == '=') { eq = k; break; }
                if (eq > i && FxEq(FxSub(d, i, eq), ext))
                    return FxAtoi(FxSub(d, eq + 1, j));
                i = j + 1;
            }
            return -1;
        }
        static void SetAssoc(string ext, int kind)
        {
            LoadAssoc();
            string outS = "";
            string d = assocData; int i = 0, n = d.Length;
            while (i < n)
            {
                int j = i; while (j < n && d[j] != '\n') j++;
                int eq = -1;
                for (int k = i; k < j; k++) if (d[k] == '=') { eq = k; break; }
                bool drop = (eq > i) && FxEq(FxSub(d, i, eq), ext);
                if (!drop)
                {
                    for (int k = i; k < j; k++) outS = U.Cat(outS, Host.CharStr((int)d[k]));
                    outS = U.Cat(outS, "\n");
                }
                i = j + 1;
            }
            outS = U.Cat(outS, ext); outS = U.Cat(outS, "=");
            outS = U.Cat(outS, U.I(kind)); outS = U.Cat(outS, "\n");
            assocData = outS;
            Host.WriteText(0, "assoc.cfg", outS);
        }
        static int KindForMex(string nm)
        {
            int dot = -1;
            for (int i = 0; i < nm.Length; i++) if (nm[i] == '.') { dot = i; break; }
            string b = dot > 0 ? FxSub(nm, 0, dot) : nm;
            if (FxEq(b, "ControlPanel")) return Kind.ControlPanel;
            if (FxEq(b, "FileExplorer")) return Kind.FileExplorer;
            if (FxEq(b, "TaskManager"))  return Kind.TaskManager;
            if (FxEq(b, "Terminal"))     return Kind.Terminal;
            if (FxEq(b, "Calculator"))   return Kind.Calculator;
            if (FxEq(b, "About"))        return Kind.About;
            if (FxEq(b, "MemOptimizer")) return Kind.MemOptimizer;
            if (FxEq(b, "Notepad"))      return Kind.Notepad;
            if (FxEq(b, "Browser"))      return Kind.Browser;
            if (FxEq(b, "AiSetup"))      return Kind.AiSetup;
            if (FxEq(b, "AiAgent"))      return Kind.AiAgent;
            if (FxEq(b, "Demo"))         return Kind.Demo;
            return -1;
        }

        void SelectAt(int mx, int my)
        {
            int ax = ClientX(), ly = ListTop();
            int n = ListCount();
            for (int d = 0; d < n; d++)
            {
                int y = ly + d * W.RowH;
                if (U.In(mx, my, ax, y, ClientW(Gfx.Width()), W.RowH - 2)) { sel = order[d]; return; }
            }
        }

        // =============================================================
        //  CLICK
        // =============================================================
        public override void OnClick(int mx, int my)
        {
            // Modals first.
            if (addrErr) { addrErr = false; return; }

            if (openWith)
            {
                int rowH = OwRowH();
                int pw = 300; if (pw > Gfx.Width() - 40) pw = Gfx.Width() - 40;
                int ph = 34 + OW_N * rowH + 26;
                int px = (Gfx.Width() - pw) / 2, py = (Gfx.Height() - ph) / 2;
                for (int i = 0; i < OW_N; i++)
                {
                    int ry = py + 34 + i * rowH;
                    if (U.In(mx, my, px + 6, ry, pw - 12, rowH - 2)) { ApplyOpenWith(OwKind(i)); return; }
                }
                openWith = false; return;
            }

            // Address bar owns the top strip: a click focuses it for editing.
            int ax = ClientX(), ay = BarTop(), aw = ClientW(Gfx.Width());
            if (U.In(mx, my, ax, ay, aw, BarH()))
            {
                if (editMode != 0) CommitEdit();
                BeginAddrEdit();
                return;
            }
            if (addrEdit) { CommitAddr(); return; }

            // While editing, a click outside the inline box commits the edit.
            if (editMode != 0)
            {
                int ely = ListTop();
                int inputY = ely;
                if (editMode == 1 && sel >= 0) inputY = ely + RowOf(sel) * W.RowH;
                if (U.In(mx, my, ax + 4, inputY, aw - 8, W.RowH - 2)) return;
                CommitEdit();
            }

            // ---- command toolbar ----
            for (int id = TB_BACK; id <= TB_VIEW; id++)
            {
                if (U.In(mx, my, tbX[id], tbY[id], tbW[id], tbH[id]))
                {
                    if (!TBEnabled(id)) return;
                    if (id == TB_BACK) Back();
                    else if (id == TB_FWD) Forward();
                    else if (id == TB_UP) NavigateUp();
                    else if (id == TB_NEW) BeginNewFolder();
                    else if (id == TB_REFRESH) { Host.FileRefresh(); RebuildOrder(); }
                    else if (id == TB_VIEW) { viewMode = viewMode == 0 ? 1 : 0; }
                    return;
                }
            }

            // ---- sidebar ----
            if (U.In(mx, my, 10, 14,           NAVW - 20, 30)) { Go(-1); return; }
            if (U.In(mx, my, 10, 14 + 40,      NAVW - 20, 30)) { Go(0);  return; }
            if (U.In(mx, my, 10, 14 + 80,      NAVW - 20, 30)) { Go(1);  return; }

            // ---- details header (sort) ----
            if (viewMode == 1 && U.In(mx, my, ax, hdY, aw, hdH))
            {
                if (mx >= hdNameX && mx < hdNameX + hdNameW) sortMode = (sortMode == 0) ? 1 : 0;
                else if (mx >= hdTypeX && mx < hdTypeX + hdTypeW) sortMode = (sortMode == 2) ? 3 : 2;
                else return;
                RebuildOrder();
                return;
            }

            // ---- file list rows ----
            int ly = ListTop();
            int n = ListCount();
            for (int d = 0; d < n; d++)
            {
                int li = order[d];
                int y = ly + d * W.RowH;
                if (U.In(mx, my, ax, y, aw, W.RowH - 2))
                {
                    int now = Host.TickMs();
                    bool dbl = (now - dblT) < 500 && dblIdx == li;
                    dblT = now; dblIdx = li;
                    sel = li; anchor = li;
                    if (!IsSelected(li)) SelectOnly(li);
                    else if (selN == 1) SelectOnly(li);   // keep click = single-select
                    if (dbl) OpenSelected();
                    return;
                }
            }
        }

        // =============================================================
        //  KEY
        // =============================================================
        public override void OnKey(int ch)
        {
            if (addrErr) { addrErr = false; return; }
            if (openWith) { openWith = false; return; }

            if (addrEdit)
            {
                if (ch == -2 || ch == 10 || ch == 13) { CommitAddr(); return; }
                if (ch == 27) { addrEdit = false; addrBuf = ""; addrDirty = false; return; }
                if (ch == -1 || ch == 8)
                {
                    if (!addrDirty) { addrBuf = ""; addrDirty = true; return; }
                    int m = addrBuf.Length;
                    if (m > 0) { string r = ""; for (int i = 0; i < m - 1; i++) r = U.Cat(r, Host.CharStr((int)addrBuf[i])); addrBuf = r; }
                    return;
                }
                if ((ch >= 0x20 && ch < 0x7F) || (ch >= 0x80 && ch <= 0xFFFF)) {
                    if (!addrDirty) { addrBuf = Host.CharStr(ch); addrDirty = true; return; }
                    addrBuf = U.Cat(addrBuf, Host.CharStr(ch)); return;
                }
                return;
            }
            if (editMode != 0)
            {
                if (ch == 27) { CancelEdit(); return; }
                if (ch == -2 || ch == 10 || ch == 13) { CommitEdit(); return; }
                if (ch == -1 || ch == 8) {
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

            // ---- navigation / selection keys ----
            if (ch == -2 || ch == 10 || ch == 13) { OpenSelected(); return; }   // Enter
            if (ch == -1 || ch == 8) { NavigateUp(); return; }                   // Backspace = Up
            if (ch == -6) { SelectAll(); return; }                               // Ctrl+A
            int n = ListCount();
            if (ch == -3) { if (sel > 0) sel--; else sel = 0; }                  // up (host harness)
            else if (ch == -4) { if (sel < n - 1) sel++; }                       // down
        }

        public override void OnRightClick(int mx, int my, int ox, int oy)
        {
            dblT = -100000; dblIdx = -1;   // a right-click never starts a double-click
            int ax = ClientX(), ly = ListTop();
            int rows = (Gfx.Height() - ly - StatusH() - PAD) / W.RowH;
            // Inside the file-list client area (and only on a real volume) ->
            // the file action menu.
            if (fs >= 0 && mx >= ax && my >= ly && my < ly + rows * W.RowH)
            {
                SelectAt(mx, my);
                if (sel >= 0 && sel < Host.FileCount(fs))
                    Desktop.OpenFileMenu(id, fs, sel, ox + mx, oy + my);
                return;
            }
            base.OnRightClick(mx, my, ox, oy);
        }

        // ---- file-action dispatch (from the context menu) -----------
        public override void DoFileAction(int code)
        {
            if (code == Desktop.A_F_OPEN)        OpenSelected();
            else if (code == Desktop.A_F_EDIT)   OpenInNotepad();
            else if (code == Desktop.A_F_NOTEPAD) OpenInNotepad();
            else if (code == Desktop.A_F_TERM)   Host.OpenApp(Kind.Terminal);
            else if (code == Desktop.A_F_COPY)   CopySelected();
            else if (code == Desktop.A_F_DEL)    DeleteSelected();
            else if (code == Desktop.A_F_RENAME) BeginRename();
            else if (code == Desktop.A_F_PROPS)  ShowProps();
            else if (code == Desktop.A_F_MKDIR)  BeginNewFolder();
            else if (code == Desktop.A_F_NEWFILE) BeginNewFile();
            else if (code == Desktop.A_F_CUT)   CopySelected();
            else if (code == Desktop.A_F_PASTE) Host.Log("[FILE] paste (no-op)");
            else if (code == Desktop.A_F_SHARE) Host.Log("[FILE] share (no-op)");
        }

        // ---- semi-blocking: record the request, paint, then block ----
        void RequestOpen(string nm, int kind, string what) { RequestOpen2(nm, kind, 0, what); }
        void RequestOpen2(string nm, int kind, int arg2, string what)
        {
            pendKind = kind; pendName = nm; pendArg2 = arg2; pendWhat = what;
            Host.SetAnim(1);
            Host.Log(U.Cat("[FILES] defer kind=", U.I(kind), " ", nm));
        }
        void RunPending()
        {
            int k = pendKind, k2 = pendArg2;
            string nm = pendName;
            pendKind = 0; pendName = null; pendArg2 = 0;
            Host.Log(U.Cat("[FILES] run queued kind=", U.I(k)));
            if (k == 1)      DoOpenFile(nm);
            else if (k == 2) DoRunExe(nm);
            else if (k == 3) Shell.OpenNotepad(nm);
            else if (k == 4) OpenWith(k2, nm);
            Host.SetAnim(0);
        }

        void OpenSelected()
        {
            if (fs < 0)   // This PC: enter the chosen volume
            {
                if (sel == 0) Go(0);
                else if (sel == 1) Go(1);
                return;
            }
            if (sel < 0 || sel >= Host.FileCount(fs)) return;
            if (Host.FileIsDir(fs, sel) != 0) return;     // dirs: no viewer
            string nm = Host.FileName(fs, sel);
            RequestOpen(nm, 1, U.Cat("正在打开 ", nm));
        }

        void DoOpenFile(string nm)
        {
            if (nm == null || nm == "") return;
            string ext = ExtOf(nm);
            if (ext == "mex") { RunMex(nm); return; }
            if (U.IsExe(nm))  { DoRunExe(nm); return; }
            int k = AssocOf(ext);
            if (k >= 0) { OpenWith(k, nm); return; }
            OpenWithChooser(nm, ext);
        }

        void RunMex(string nm)
        {
            Host.Log(U.Cat("[FILES] run mex ", nm));
            int k = KindForMex(nm);
            if (k >= 0) { Host.OpenApp(k); return; }
            Host.Exec(U.Cat("clrapp ", nm));
        }

        void OpenWith(int kind, string nm)
        {
            if (kind == Kind.Notepad) { Shell.OpenNotepad(nm); return; }
            Host.Log(U.Cat("[FILES] open ", nm, " with ", Desktop.KindName(kind)));
            Host.OpenApp(kind);
        }

        const int OW_N = 6;
        static int OwRowH() { return 32; }
        static string OwLabel(int i)
        {
            if (i == 0) return "记事本 (文本/二进制)";
            if (i == 1) return "浏览器";
            if (i == 2) return "终端";
            if (i == 3) return "计算器";
            if (i == 4) return "任务管理器";
            return "设置";
        }
        static int OwKind(int i)
        {
            if (i == 0) return Kind.Notepad;
            if (i == 1) return Kind.Browser;
            if (i == 2) return Kind.Terminal;
            if (i == 3) return Kind.Calculator;
            if (i == 4) return Kind.TaskManager;
            return Kind.ControlPanel;
        }
        void OpenWithChooser(string nm, string ext) { openWith = true; owFile = nm; owExt = ext; }
        void ApplyOpenWith(int kind)
        {
            SetAssoc(owExt, kind);
            string nm = owFile;
            openWith = false; owFile = ""; owExt = "";
            RequestOpen2(nm, 4, kind, "正在启动应用…");
        }

        void DoRunExe(string nm)
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

        void OpenInNotepad()
        {
            if (fs < 0 || sel < 0 || sel >= Host.FileCount(fs)) return;
            if (Host.FileIsDir(fs, sel) != 0) return;
            string nm = Host.FileName(fs, sel);
            RequestOpen(nm, 3, U.Cat("正在读取 ", nm));
        }
        void CopySelected()
        {
            if (fs < 0 || sel < 0 || sel >= Host.FileCount(fs)) return;
            clip = Host.FileName(fs, sel);
        }
        void DeleteSelected()
        {
            if (fs < 0) return;
            for (int k = 0; k < selN; k++)
            {
                int li = selSet[k];
                if (li >= 0 && li < Host.FileCount(fs))
                    Host.FileDelete(fs, Host.FileName(fs, li));
            }
            Host.FileRefresh();
            RebuildOrder();
            ClearSel();
        }
        void BeginRename()
        {
            if (fs < 0 || sel < 0 || sel >= Host.FileCount(fs)) return;
            editOld = Host.FileName(fs, sel);
            editBuf = editOld;
            editMode = 1; editDirty = false;
        }
        void BeginNewFolder()
        {
            if (fs < 0) return;
            // BUG #8: read-only volumes cannot gain a directory.
            if (fs != 0) { Host.Log("[FILES] new folder refused: read-only volume"); return; }
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
            if (Host.FileMkDir(fs, name) < 0)
            { Host.Log("[FILES] new folder FAILED: create refused"); return; }
            // BUG #2: FileMkDir already refreshes the host listing, but this
            // window's order[] array is an independent cache -- rebuild it or
            // the new folder stays invisible until the user navigates away.
            RebuildOrder();
            int nn = ListCount();
            sel = -1;
            for (int i = 0; i < nn; i++)
                if (ListName(i) == name) { sel = i; break; }
            if (sel >= 0)
            {
                int rows = (Gfx.Height() - 60) / W.RowH;
                if (sel >= scroll + rows) scroll = sel - rows + 1;
                if (sel < scroll) scroll = sel;
            }
            editOld = name; editBuf = name;
            editMode = 1; editDirty = false;
        }
        void BeginNewFile()
        {
            if (fs < 0) return;
            // BUG #8: SFS is read-only.  The context menu already hides the
            // verb, but the toolbar and the keyboard can reach here, so the
            // volume itself is checked before anything is created.
            if (fs != 0) { Host.Log("[FILES] new file refused: read-only volume"); return; }
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
            int wr = Host.WriteText(fs, name, "");
            if (wr < 0) { Host.Log("[FILES] new file FAILED: write refused"); return; }
            Host.Log(U.Cat("[FILES] new file created: ", name));
            // BUG #2 (ghost new file): WriteText already called FileRefresh,
            // but the paint order[] array is a SEPARATE cache -- it was only
            // rebuilt on navigation, so the freshly created file was missing
            // from the very next frame even though Host.FileCount() grew.
            // Rebuild the order and re-derive the selection every time.
            RebuildOrder();
            int nn = ListCount();
            sel = -1;
            for (int i = 0; i < nn; i++)
                if (ListName(i) == name) { sel = i; break; }
            if (sel >= 0)
            {
                int rows = (Gfx.Height() - 60) / W.RowH;
                if (sel >= scroll + rows) scroll = sel - rows + 1;
                if (sel < scroll) scroll = sel;
            }
            editOld = name; editBuf = name;
            editMode = 1; editDirty = false;
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
            RebuildOrder();
            editMode = 0; editBuf = ""; editDirty = false;
        }
        void CancelEdit() { editMode = 0; editBuf = ""; editDirty = false; }

        void ShowProps()
        {
            if (fs < 0 || sel < 0 || sel >= Host.FileCount(fs)) return;
            string nm = Host.FileName(fs, sel);
            string ty = Host.FileIsDir(fs, sel) != 0 ? "Folder" : "File";
            string loc = fs == 0 ? "Local Disk (MKFS)" : "System (SFS)";
            string[] labs = new string[4];
            int[]    acts = new int[4];
            labs[0] = U.Cat("Name:    ", nm); acts[0] = -3;
            labs[1] = U.Cat("Type:    ", ty); acts[1] = -3;
            labs[2] = U.Cat("Location:", loc); acts[2] = -3;
            labs[3] = "Close";                  acts[3] = -3;
            Popup.Open(Desktop.OWNER_FILE, Gfx.Width() / 2 - 90, Gfx.Height() / 2 - 70, labs, acts, 4);
        }

        public override string SelectedFile()
        { return (fs >= 0 && sel >= 0 && sel < Host.FileCount(fs)) ? Host.FileName(fs, sel) : ""; }
        public override int SelectedFs()   { return fs; }
        public override int SelectedIsDir()
        { return (fs >= 0 && sel >= 0 && sel < Host.FileCount(fs)) ? Host.FileIsDir(fs, sel) : 0; }
    }

    // =================================================================
    //  Control Panel
    // =================================================================
    public class ControlPanelApp : App
    {
        int page;   // -1 tiles, 0 System, 1 Power, 2 Display, 3 Network,
                    // 4 Storage, 5 Devices, 6 Personalize, 7 Taskbar, 8 Plugins
        int winTransparent;   // 0 = normal glass, 1 = whole window fully transparent
        const int SW = 92, SGap = 12, SCols = 6;   // swatch grid

        // ---- advanced search -----------------------------------------
        int searchFocus = 0;          // search box has keyboard focus
        string searchBuf = "";        // query text
        int searchScroll = 0;         // results list scroll offset (px)
        int searchSel = 0;            // keyboard-selected result index
        string[] rName; int[] rPage; int[] rCat; int rN = 0;   // result cache
        int sCount = 0;               // index builder counter
        static string[] C_NAME; static int[] C_LETTER; static uint[] C_COLOR;
        static string[] S_NAME, S_KEYS; static int[] S_PAGE, S_CAT; static int S_N;

        public ControlPanelApp()
        {
            rName = new string[64]; rPage = new int[64]; rCat = new int[64];
            InitSearch();
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

            Home(pad, w);
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

            // Whole-window transparency toggle.  When on, the kernel drops
            // every window's glassy chrome to zero opacity so the desktop
            // shows straight through it (the floating content/text remain).
            int ty = ay + 92;
            Gfx.Text(pad, ty, "Window transparency", C.Text);
            W.Button(pad + 200, ty + 16, 240, 36,
                     winTransparent != 0 ? "Transparent: On" : "Transparent: Off");
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

            // ---- home page: advanced search + tile grid --------------
            if (page == -1)
            {
                int sbx = pad, sby = pad + 38, sbw = w - 2 * pad, sbh = 34;
                int cx = sbx + sbw - 22, cy = sby + sbh / 2;
                if (U.In(mx, my, cx - 10, cy - 10, 20, 20))      // clear button
                { searchBuf = ""; searchSel = 0; searchScroll = 0; searchFocus = 1; return; }
                if (U.In(mx, my, sbx, sby, sbw, sbh))             // focus search box
                { searchFocus = 1; return; }

                if (searchFocus != 0 || NexOS.Sys.StrLen(searchBuf) > 0)
                {
                    int top = sby + sbh + 12;
                    int listTop = top + 24, listBot = Gfx.Height() - pad;
                    int rowH = 44, gap = 6;
                    int visRows = (listBot - listTop) / (rowH + gap); if (visRows < 1) visRows = 1;
                    int n = BuildSearch(searchBuf);
                    int first = searchScroll / (rowH + gap);
                    int yy = listTop - searchScroll;
                    for (int i = first; i < n && yy < listBot; i++)
                    {
                        if (U.In(mx, my, sbx, yy, sbw, rowH))
                        { searchSel = i; GoToPage(rPage[i]); return; }
                        yy += rowH + gap;
                    }
                    return;
                }

                // no query: tile grid
                int gy = sby + sbh + 14, gx = pad, cols = 3;
                int cw = (w - 2 * pad - (cols - 1) * 12) / cols, chh = 84;
                for (int i = 0; i < 8; i++)
                {
                    int r = i / cols, c = i % cols;
                    int x = gx + c * (cw + 12), y = gy + r * (chh + 12);
                    if (U.In(mx, my, x, y, cw, chh))
                    {
                        if (i == 0) page = 0; else if (i == 1) page = 2; else if (i == 2) page = 3;
                        else if (i == 3) page = 4; else if (i == 4) page = 5; else if (i == 5) page = 1;
                        else if (i == 6) { Host.Exec("plugin persist"); page = 8; }
                        else if (i == 7) page = 9;
                        return;
                    }
                }
                return;
            }

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
                    // Whole-window transparency toggle.
                    if (U.In(mx, my, pad + 200, ay + 92 + 16, 240, 36)) {
                        winTransparent = winTransparent != 0 ? 0 : 1;
                        Host.SetWindowTransparent(winTransparent);
                        Host.Repaint();
                        return;
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
                for (int i = 0; i < Desktop.KindCount; i++)
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

            // (home-page tile grid / search handled at the top of OnClick)
        }

        // ---- "Apps & features" page (page 9) ---------------------------
        static void AppsPage(int pad, int w)
        {
            int top = pad + 60, rowH = 44, btnW = 110, btnH = 30;
            int btnX = w - pad - btnW;
            int listW = w - 2 * pad;
            Gfx.Text(pad, top - 36, "Apps & features", C.Text);
            Gfx.Text(pad, top - 16, "Install or uninstall the built-in applications.", C.TextSub);
            for (int i = 0; i < Desktop.KindCount; i++)
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

        // ---- advanced search -----------------------------------------
        void GoToPage(int p) { page = p; }

        void Home(int pad, int w)
        {
            W.Header(pad, pad, "All Control Panel Items");
            int sbx = pad, sby = pad + 38, sbw = w - 2 * pad, sbh = 34;
            DrawSearchBox(sbx, sby, sbw, sbh);

            if (searchFocus != 0 || NexOS.Sys.StrLen(searchBuf) > 0)
            {
                int top = sby + sbh + 12;
                int n = BuildSearch(searchBuf);
                int hint = (NexOS.Sys.StrLen(searchBuf) == 0 && searchFocus != 0) ? 1 : 0;
                string label2;
                if (hint != 0) label2 = "Type to search settings...";
                else if (n == 0) label2 = "No results";
                else label2 = U.Cat(U.I(n), " result", n == 1 ? "" : "s");
                Gfx.Text(pad, top, label2, C.TextSub);
                int listTop = top + 24, listBot = Gfx.Height() - pad;
                int rowH = 44, gap = 6;
                int visRows = (listBot - listTop) / (rowH + gap); if (visRows < 1) visRows = 1;
                int maxScroll = (n - visRows) * (rowH + gap); if (maxScroll < 0) maxScroll = 0;
                if (searchScroll > maxScroll) searchScroll = maxScroll;
                if (searchScroll < 0) searchScroll = 0;
                int first = searchScroll / (rowH + gap);
                int yy = listTop - searchScroll;
                for (int i = first; i < n && yy < listBot; i++)
                {
                    bool sel = (i == searchSel);
                    uint bg = sel ? C.Sel : (W.Hot(sbx, yy, sbw, rowH) ? C.Hover : C.Card);
                    Gfx.FillRound(sbx, yy, sbw, rowH, 8, bg);
                    Gfx.DrawRound(sbx, yy, sbw, rowH, 8, C.Border);
                    int cat = rCat[i];
                    Gfx.Icon(sbx + 12, yy + (rowH - 30) / 2, 30, C_COLOR[cat], C_LETTER[cat], 0xFFFFFF);
                    Gfx.Text(sbx + 54, yy + 8, rName[i], C.Text);
                    Gfx.Text(sbx + 54, yy + 26, C_NAME[cat], C.TextSub);
                    yy += rowH + gap;
                }
                if (n > visRows)
                    DrawVScroll(sbx + sbw - 6, listTop, 6, listBot - listTop, first, visRows, n);
            }
            else
            {
                int gy = sby + sbh + 14, gx = pad, cols = 3;
                int cw = (w - 2 * pad - (cols - 1) * 12) / cols, chh = 84;
                for (int i = 0; i < 8; i++)
                {
                    int r = i / cols, c = i % cols;
                    int x = gx + c * (cw + 12), y = gy + r * (chh + 12);
                    Tile(x, y, cw, chh, TileLetter(i), TileName(i), TileColor(i));
                }
            }
        }

        void DrawSearchBox(int x, int y, int w, int h)
        {
            uint border = searchFocus != 0 ? C.Accent : C.BorderMid;
            Gfx.FillRound(x, y, w, h, h / 2, C.Card);
            Gfx.DrawRound(x, y, w, h, h / 2, border);
            int six = x + 16, siy = y + h / 2;
            Gfx.DrawCircle(six, siy, 6, C.TextSub);
            Gfx.DrawLine(six + 5, siy + 5, six + 11, siy + 11, C.TextSub);
            int tx = x + 34;
            if (NexOS.Sys.StrLen(searchBuf) > 0)
            {
                string shown = searchBuf;
                if (searchFocus != 0 && (Host.Ticks() / 30) % 2 == 0) shown = U.Cat(shown, "|");
                Gfx.Text(tx, y + (h - 16) / 2, shown, C.Text);
                int cx = x + w - 22, cy = y + h / 2;
                if (W.Hot(cx - 8, cy - 8, 16, 16)) Gfx.FillCircle(cx, cy, 9, C.Hover);
                Gfx.Text(cx - 4, cy - 8, "x", C.TextSub);
            }
            else
                Gfx.Text(tx, y + (h - 16) / 2, "Search settings...", C.TextSub);
        }

        void DrawVScroll(int x, int y, int w, int h, int first, int vis, int total)
        {
            Gfx.FillRound(x, y, w, h, 3, C.Border);
            int th = (h * vis) / total; if (th < 12) th = 12;
            int ty = y + ((h - th) * first) / (total - vis);
            Gfx.FillRound(x, ty, w, th, 3, C.BorderMid);
        }

        // Build the filtered result set for query `q` into the r* cache.
        int BuildSearch(string q)
        {
            rN = 0;
            if (NexOS.Sys.StrLen(q) == 0) return 0;
            string ql = Lower(q);
            for (int i = 0; i < S_N; i++)
            {
                string hay = U.Cat(S_NAME[i], " ", S_KEYS[i]);
                if (ContainsCI(hay, ql) != 0 && rN < 64)
                {
                    rName[rN] = S_NAME[i]; rPage[rN] = S_PAGE[i];
                    rCat[rN] = S_CAT[i]; rN++;
                }
            }
            if (searchSel >= rN) searchSel = rN - 1;
            if (searchSel < 0) searchSel = 0;
            return rN;
        }

        void InitSearch()
        {
            C_NAME = new string[10]; C_LETTER = new int[10]; C_COLOR = new uint[10];
            C_NAME[0]="System";      C_LETTER[0]=(int)'S'; C_COLOR[0]=0x0078D4u;
            C_NAME[1]="Display";     C_LETTER[1]=(int)'D'; C_COLOR[1]=0x8B5CF6u;
            C_NAME[2]="Network";     C_LETTER[2]=(int)'N'; C_COLOR[2]=0x10B981u;
            C_NAME[3]="Storage";     C_LETTER[3]=(int)'H'; C_COLOR[3]=0xF59E0Bu;
            C_NAME[4]="Devices";     C_LETTER[4]=(int)'V'; C_COLOR[4]=0x06B6D4u;
            C_NAME[5]="Power";       C_LETTER[5]=(int)'P'; C_COLOR[5]=0xEF4444u;
            C_NAME[6]="Personalize"; C_LETTER[6]=(int)'W'; C_COLOR[6]=0x6D28D9u;
            C_NAME[7]="Taskbar";     C_LETTER[7]=(int)'T'; C_COLOR[7]=0x0EA5E9u;
            C_NAME[8]="Plugins";     C_LETTER[8]=(int)'G'; C_COLOR[8]=0x9333EAu;
            C_NAME[9]="Apps";        C_LETTER[9]=(int)'A'; C_COLOR[9]=0x0EA5E9u;

            S_NAME = new string[64]; S_KEYS = new string[64];
            S_PAGE = new int[64]; S_CAT = new int[64];
            sCount = 0;
            // categories
            AddS("System",           "system about os info cpu memory ram arch 系统 信息 关于", 0, 0);
            AddS("Display",          "display screen 显示 屏幕", 2, 1);
            AddS("Network & Internet","network internet ethernet wifi wireless 网络 以太网 无线", 3, 2);
            AddS("Storage",          "storage disk drive capacity 存储 硬盘 容量", 4, 3);
            AddS("Devices",          "devices hardware processor memory display audio network 设备 硬件 处理器 显示器 音频 网卡", 5, 4);
            AddS("Power",            "power shutdown restart 电源 关机 重启", 1, 5);
            AddS("Personalization",  "personalize wallpaper background theme accent 个性化 壁纸 背景 主题 强调色", 6, 6);
            AddS("Taskbar",          "taskbar alignment labels 任务栏 对齐 标签", 7, 7);
            AddS("Plugins",          "plugins extensions 插件 扩展", 8, 8);
            AddS("Apps & features",  "apps applications install uninstall features 应用 安装 卸载 功能", 9, 9);
            // leaf settings
            AddS("System information","system about os name architecture memory ram 系统信息 关于 名称", 0, 0);
            AddS("Resolution",       "resolution screen 分辨率 屏幕 显示", 2, 1);
            AddS("Display scaling",  "scaling dpi zoom 缩放 显示", 2, 1);
            AddS("Dark mode",        "theme dark light mode 主题 深色 浅色 模式 暗色", 2, 1);
            AddS("Refresh rate",     "refresh rate hz 刷新率", 2, 1);
            AddS("Accent colour",    "accent color colour 强调色 颜色", 2, 1);
            AddS("Pixel / CRT mode", "pixel crt monitor scanline retro 像素 扫描线 复古 显示器", 2, 1);
            AddS("Terminal font",    "terminal font size 终端 字体 字号", 2, 1);
            AddS("Ethernet",         "ethernet lan wired 以太网 有线 网络", 3, 2);
            AddS("Wi-Fi",            "wifi wireless 无线 网络 局域网", 3, 2);
            AddS("Storage usage",    "storage usage disk capacity 存储 使用情况 容量", 4, 3);
            AddS("Local Disk (MKFS)","local disk mkfs volume 本地磁盘 卷", 4, 3);
            AddS("System volume (SFS)","system sfs volume 系统卷", 4, 3);
            AddS("Device manager",   "devices processor memory disk audio network 设备管理器 硬件", 5, 4);
            AddS("Shut down",        "shutdown power off 关机 关闭 电源", 1, 5);
            AddS("Restart",          "restart reboot 重启 重新启动", 1, 5);
            AddS("Desktop background","wallpaper background 桌面背景 壁纸", 6, 6);
            AddS("Window transparency","transparency transparent window 透明 窗口 半透明", 6, 6);
            AddS("Taskbar alignment","taskbar alignment center left 任务栏 对齐 居中 左侧", 7, 7);
            AddS("Taskbar labels",   "taskbar labels show 任务栏 标签 显示文字", 7, 7);
            AddS("Plugin manager",   "plugins enable disable 插件 管理 启用", 8, 8);
            AddS("Apps & features",  "apps install uninstall 应用 安装 卸载 功能", 9, 9);
            S_N = sCount;
        }

        void AddS(string n, string kw, int p, int c)
        {
            if (sCount >= 64) return;
            S_NAME[sCount] = n; S_KEYS[sCount] = kw; S_PAGE[sCount] = p; S_CAT[sCount] = c; sCount++;
        }

        static string Lower(string s)
        {
            int n = NexOS.Sys.StrLen(s); string r = "";
            for (int i = 0; i < n; i++)
            {
                int c = (int)NexOS.Sys.StrCharAt(s, i);
                if (c >= 'A' && c <= 'Z') c = c + 32;
                r = U.Cat(r, Host.CharStr(c));
            }
            return r;
        }

        static int ContainsCI(string hay, string needle)
        {
            string hl = Lower(hay);
            int hn = NexOS.Sys.StrLen(hl), nn = NexOS.Sys.StrLen(needle);
            if (nn == 0) return 1;
            if (nn > hn) return 0;
            for (int i = 0; i + nn <= hn; i++)
            {
                int ok = 1;
                for (int j = 0; j < nn; j++)
                    if (NexOS.Sys.StrCharAt(hl, i + j) != NexOS.Sys.StrCharAt(needle, j)) { ok = 0; break; }
                if (ok != 0) return 1;
            }
            return 0;
        }

        public override void OnKey(int ch)
        {
            if (searchFocus == 0 || page != -1) return;
            if (ch == 27)   // Esc: clear query, or unfocus when empty
            {
                if (NexOS.Sys.StrLen(searchBuf) > 0) { searchBuf = ""; searchSel = 0; searchScroll = 0; }
                else searchFocus = 0;
                return;
            }
            if (ch == -2 || ch == 10 || ch == 13)   // Enter: open selected result
            {
                int n = BuildSearch(searchBuf);
                if (n > 0 && searchSel >= 0 && searchSel < n) GoToPage(rPage[searchSel]);
                return;
            }
            if (ch == -1 || ch == 8)   // Backspace
            {
                int m = NexOS.Sys.StrLen(searchBuf);
                if (m > 0) { string r = ""; for (int i = 0; i < m - 1; i++) r = U.Cat(r, Host.CharStr((int)searchBuf[i])); searchBuf = r; searchSel = 0; searchScroll = 0; }
                return;
            }
            if (ch == -3 || ch == -18) { if (searchSel > 0) searchSel--; return; }   // up
            if (ch == -4 || ch == -19) { if (searchSel < rN - 1) searchSel++; return; } // down
            if ((ch >= 0x20 && ch < 0x7F) || (ch >= 0x80 && ch <= 0xFFFF))
            { searchBuf = U.Cat(searchBuf, Host.CharStr(ch)); searchSel = 0; searchScroll = 0; return; }
        }

        public override void OnWheel(int dy)
        {
            if (page != -1) return;
            if (!(searchFocus != 0 || NexOS.Sys.StrLen(searchBuf) > 0)) return;
            int step = (dy > 0) ? -(44 + 6) : (44 + 6);
            searchScroll += step;
            if (searchScroll < 0) searchScroll = 0;
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
        bool binary;   // file is not decodable text -> show a hex view
        string hex;    // raw bytes as hex (only meaningful when binary)

        public NotepadApp()
        {
            t = new TBox();
            string pending = Shell.TakeNotepadFile();
            if (pending != null && pending != "")
            {
                name = pending;
                // Probe the RAW bytes first: if the file is not verifiable
                // text (NULs / control bytes) fall back to a binary view
                // instead of showing mangled characters.
                int pfs = 1;
                string hx = Host.ReadHex(1, pending);
                if (hx == "") { pfs = 0; hx = Host.ReadHex(0, pending); }
                if (IsBinaryHex(hx))
                {
                    binary = true; hex = hx; t.text = "";
                }
                else
                {
                    binary = false; hex = "";
                    t.text = Host.ReadText(pfs, pending);
                    if (t.text == "") t.text = "Untitled - type below, or click Open to read a file.";
                }
            }
            else
            {
                t.text = "Untitled - type below, or click Open to read a file.";
                name = "Untitled";
                binary = false; hex = "";
            }
            picking = false;
            saved = false;
        }

        // ---- binary detection / hex formatting --------------------------
        static int HxNib(int c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
        static int HxByte(string h, int i)
        {
            if (i + 1 >= h.Length) return -1;
            int a = HxNib((int)h[i]), b = HxNib((int)h[i + 1]);
            if (a < 0 || b < 0) return -1;
            return (a << 4) | b;
        }
        static bool IsBinaryHex(string h)
        {
            if (h == null) return false;
            int n = h.Length / 2;
            for (int i = 0; i < n; i++)
            {
                int v = HxByte(h, i * 2);
                if (v < 0) return true;                          // malformed
                if (v == 0) return true;                         // NUL
                if (v < 0x20 && v != 9 && v != 10 && v != 13) return true; // control
                if (v == 0x7F) return true;
            }
            return false;
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
            if (binary) { DrawBinary(pad, w, h); return; }

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

        // Binary (hex) view: offset + 8 bytes hex + ASCII gutter.  Read-only.
        void DrawBinary(int pad, int w, int h)
        {
            Gfx.FillRect(pad, 48, w - 2 * pad, h - 48 - pad, C.White);
            Gfx.FillRound(pad + 6, 52, w - 2 * pad - 12, 24, 4, 0xFFF3CD);
            Gfx.Text(pad + 12, 56, "二进制模式：该文件不是可解码文本，按原始字节显示", 0x7A4F00);
            int y = 84;
            int total = hex.Length / 2;
            for (int off = 0; off < total && y < h - pad - 12; off += 8)
            {
                string line = Hx8(off);
                line = U.Cat(line, "  ");
                for (int j = 0; j < 8; j++)
                {
                    if (off + j < total) line = U.Cat(line, HxSub(hex, (off + j) * 2, 2));
                    else line = U.Cat(line, "  ");
                    line = U.Cat(line, " ");
                }
                line = U.Cat(line, " ");
                for (int j = 0; j < 8; j++)
                {
                    if (off + j < total)
                    {
                        int v = HxByte(hex, (off + j) * 2);
                        if (v >= 0x20 && v < 0x7F) line = U.Cat(line, Host.CharStr(v));
                        else line = U.Cat(line, ".");
                    }
                }
                Gfx.Text(pad + 8, y, line, 0x1F2937);
                y += 18;
            }
        }
        static string HxSub(string s, int a, int len)
        {
            string r = "";
            for (int i = a; i < a + len && i < s.Length; i++) r = U.Cat(r, Host.CharStr((int)s[i]));
            return r;
        }
        static string Hx8(int v)
        {
            string r = "";
            for (int sh = 28; sh >= 0; sh -= 4)
            {
                int d = (v >> sh) & 15;
                r = U.Cat(r, Host.CharStr(d < 10 ? ('0' + d) : ('a' + d - 10)));
            }
            return r;
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
            if (binary) { return; }                     // binary view is read-only
            if (ch == -10) { Save(); return; }          // Ctrl+S -> save
            if (ch == -2) { t.Insert("\n"); saved = false; return; }
            t.Key(ch);
            saved = false;
        }
    }
}
