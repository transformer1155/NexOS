
// =====================================================================
//  Notepad -- a REAL application, not a shell stub.
// ---------------------------------------------------------------------
//  This file compiles into Notepad.mex and is loaded into its OWN CLR slot.
//  It implements the entry points mforms calls for a window it owns:
//
//      App.Main::Init()         once, when the slot is loaded
//      App.Main::Paint(id,w,h)  paint the client area
//      App.Main::Click(id,x,y)  mouse
//      App.Main::Key(id,ch)     keyboard
//      App.Main::Title(id)      window caption
//
//  mforms.cpp maps "NexOS.Forms.Shell::<M>" to "App.Main::<M>" for any
//  window whose owner slot is not 0 (see slot_symbol), so the desktop shell
//  and an application expose the SAME contract and neither needs a private
//  protocol.
//
//  The only dependency is NexOS.Forms (C/Gfx/Host/U/W/TBox) -- deliberately
//  no reference to Apps.cs or Desktop.cs, so this assembly has no link to the
//  shell and can be loaded, replaced or closed on its own.
// =====================================================================
using NexOS.Forms;

namespace App
{
    // Helpers replacing the two shell calls the original class used.  A
    // standalone instance has no "open with" argument, so these return the
    // empty values the shell would have produced anyway.
    static class Pending
    {
        public static string TakePendingFile() { return ""; }
        public static int    TakePendingFs()   { return 0; }
    }

    public static class Main
    {
        static NotepadApp app;

        public static void Init()
        {
            if (app == null) app = new NotepadApp();
            // Create the tween pool in the PERSISTENT part of this slot's
            // managed heap.  Paint() resets the heap to its frame-start mark;
            // a pool lazily created there would dangle on the next frame and
            // eventually unload the whole .mex image (blank window).
            NexOS.Forms.Anim.Init();
        }

        public static int Paint(int id, int w, int h)
        {
            Init();
            app.OnPaint();
            return 0;
        }

        public static int Click(int id, int x, int y)
        {
            Init();
            app.OnClick(x, y);
            return 0;
        }

        public static int Key(int id, int ch)
        {
            Init();
            app.OnKey(ch);
            return 0;
        }

        public static string Title(int id)
        {
            Init();
            return app.GetTitle();
        }
    }

    public class NotepadApp : NexOS.Forms.App
    {
        TBox t;        // editor model (caret + selection + undo)
        string name;   // currently loaded file
        bool picking;   // showing the open list
        bool saved;    // last action was a successful Ctrl+S (title hint)
        bool binary;   // file is not decodable text -> show a hex view

        // ---- text metrics: the single source of truth ------------------
        // The font is an 8x16 BIOS cell (gui_legacy.cpp: g_font_w=8, g_font_h=16).
        // Caret placement, line advance and click hit-testing all derive from
        // these, so they can never disagree the way the old literals (8 / 20 /
        // 18) did -- the caret drifted one line per 4 lines of text.
        const int CH_W   = 8;    // glyph advance
        const int LINE_H = 16;   // glyph cell height == line advance
        const int TEXT_X = 18;   // body left edge: pad(10) + 8
        const int TEXT_Y = 54;   // body top edge, below the two header rows
        int  firstLine;      // top line shown in the body (scroll offset)
        bool   askClose;       // close prompt is on screen
        const int SB_H   = 22;   // status bar height

        // ---- document state -------------------------------------------
        bool   confirmSave;      // overwrite prompt is on screen
        string confirmName;      // the name the prompt refers to
        bool   mdMode;           // document detected as Markdown
        static bool mdPaint;     // mirror of mdMode for the static drawer
        static int  mdFirstLine;   // mirror of firstLine for the static drawer
        string hex;    // raw bytes as hex (only meaningful when binary)
        int    fs;     // volume this document lives on (0=MKFS root, 3=Desktop)

        public NotepadApp()
        {
            t = new TBox();
            string pending = Pending.TakePendingFile();
            if (pending != null && pending != "")
                LoadFile(Pending.TakePendingFs(), pending);
            else
            {
                t.text = "Untitled - type below, or click Open to read a file.";
                name = "Untitled";
                fs = 0;
                binary = false; hex = "";
            }
            picking = false;
            saved = false;
        }

        // ---- the ONE file-open path ------------------------------------
        // Both the shell's pending-file handoff and the in-app Open picker
        // land here.  Read the RAW bytes first: Host.ReadText() scrubs every
        // NUL / control byte to a space or a dot, so decoding a .mex (a
        // binary "MEX1" image) as text yields a page of blanks that looks
        // exactly like "the file could not be opened".  Binary payloads get
        // the existing read-only hex view; text files keep the old path.
        void LoadFile(int pfs, string fname)
        {
            name = fname;
            fs = pfs;
            string hx = Host.ReadHex(pfs, fname);
            if (hx == "")
            {
                int alt = (pfs == 1) ? 0 : 1;
                string altHex = Host.ReadHex(alt, fname);
                if (altHex != "") { hx = altHex; fs = alt; }
            }
            if (IsBinaryHex(hx))
            {
                binary = true; hex = hx; t.text = "";
            }
            else
            {
                binary = false; hex = "";
                t.text = Host.ReadText(fs, fname);
                if (t.text == "") t.text = "Untitled - type below, or click Open to read a file.";
            }
            firstLine = 0;
            t.cursor = 0;
            t.selA = t.selB = 0;
            saved = false;
            Host.Log(U.Cat(U.Cat("[EDITOR] ", U.I(binary ? 1 : 0)), fname));
        }

        // ---- context menu (right click in the text body) ------------------
        // Drawn and hit-tested here rather than through the shell Popup class:
        // an application must not depend on shell-side UI.
        bool menuOpen;
        int  menuX, menuY;
        const int MENU_W = 210, MENU_ROW = 22, MENU_ROWS = 8;

        void DrawMenu(int w, int h)
        {
            int bw = MENU_W, bh = MENU_ROWS * MENU_ROW + 8;
            int bx = menuX, by = menuY;
            if (bx + bw > w) bx = w - bw - 4;
            if (bx < 4) bx = 4;
            if (by + bh > h) by = h - bh - 4;
            if (by < 4) by = 4;
            Gfx.FillRound(bx, by, bw, bh, 8, 0xFFFFFF);
            Gfx.DrawRound(bx, by, bw, bh, 8, C.Border);
            string[] labels = new string[] {
                "Undo", "Redo", "Cut", "Copy", "Paste", "Delete", "Select All", "Save" };
            int hot = MenuIndexAt(Gfx.MouseX(), Gfx.MouseY());
            for (int i = 0; i < MENU_ROWS; i++)
            {
                int ry = by + 4 + i * MENU_ROW;
                if (i == hot) Gfx.FillRound(bx + 2, ry, bw - 4, MENU_ROW - 1, 4, C.Hover);
                Gfx.Text(bx + 12, ry + 3, labels[i], C.Text);
            }
        }

        // Which menu row is under the pointer, or -1.
        int MenuIndexAt(int mx, int my)
        {
            int bw = MENU_W, bh = MENU_ROWS * MENU_ROW + 8;
            int w = Gfx.Width(), h = Gfx.Height();
            int bx = menuX, by = menuY;
            if (bx + bw > w) bx = w - bw - 4;
            if (bx < 4) bx = 4;
            if (by + bh > h) by = h - bh - 4;
            if (by < 4) by = 4;
            if (mx < bx || mx >= bx + bw) return -1;
            int rel = my - (by + 4);
            if (rel < 0 || rel >= MENU_ROWS * MENU_ROW) return -1;
            return rel / MENU_ROW;
        }

        // Run one menu row.
        void MenuAction(int i)
        {
            menuOpen = false;
            if (i == 0) { t.Undo(); saved = false; }
            else if (i == 1) { t.Redo(); saved = false; }
            else if (i == 2) { t.Key(-18); saved = false; }
            else if (i == 3) { t.Key(-3); }
            else if (i == 4) { t.Key(-4); saved = false; }
            else if (i == 5) { t.Key(-17); saved = false; }
            else if (i == 6) { t.SelectAll(); }
            else if (i == 7) { Save(); }
        }

        // ---- overwrite confirmation overlay ------------------------------
        // Modal: the pending Save does not commit until Overwrite is pressed.
        // ---- close-with-unsaved-changes prompt ---------------------------
        // Closing a modified document must not silently discard the edits.
        void DrawAskClose(int w, int h)
        {
            Gfx.FillRect(0, 44, w, h - 44, 0xF7F7F7);
            int bx = 40, by = 120, bw = w - 80, bh = 130;
            Gfx.FillRound(bx, by, bw, bh, 10, 0xFFFFFF);
            Gfx.DrawRound(bx, by, bw, bh, 10, C.Border);
            Gfx.Text(bx + 16, by + 14, "Unsaved changes", C.Text);
            Gfx.Text(bx + 16, by + 40, name == null ? "" : name, C.TextSub);
            Gfx.Text(bx + 16, by + 62, "Close and discard the edits?", C.Danger);
            W.Button(bx + 16, by + bh - 44, 120, 30, "Discard");
            W.Button(bx + 152, by + bh - 44, 100, 30, "Keep editing");
        }

        void DrawConfirm(int w, int h)
        {
            Gfx.FillRect(0, 44, w, h - 44, 0xF7F7F7);
            int bx = 40, by = 120, bw = w - 80, bh = 130;
            Gfx.FillRound(bx, by, bw, bh, 10, 0xFFFFFF);
            Gfx.DrawRound(bx, by, bw, bh, 10, C.Border);
            Gfx.Text(bx + 16, by + 14, "Overwrite existing file?", C.Text);
            Gfx.Text(bx + 16, by + 40, confirmName == null ? "" : confirmName, C.TextSub);
            Gfx.Text(bx + 16, by + 62, "This replaces the file on disk.", C.Danger);
            W.Button(bx + 16, by + bh - 44, 120, 30, "Overwrite");
            W.Button(bx + 152, by + bh - 44, 100, 30, "Cancel");
        }
        // ---- scroll bar -------------------------------------------------
        // A draggable thumb plus wheel support.  The body shows its own line
        // range, so the thumb is sized from the visible fraction and positioned
        // from firstLine.
        void DrawScrollBar(int w, int h, int pad)
        {
            int lines = t.LineCount();
            int vis = (h - SB_H - TEXT_Y) / LINE_H;
            if (vis < 1) vis = 1;
            if (lines <= vis) return;
            int bx = w - pad - 10;
            int by = TEXT_Y;
            int bh = h - SB_H - TEXT_Y;
            Gfx.FillRound(bx, by, 8, bh, 4, 0xE5E7EB);
            int th = bh * vis / lines;
            if (th < 24) th = 24;
            int maxFirst = lines - vis;
            if (maxFirst < 1) maxFirst = 1;
            int ty = by + (bh - th) * firstLine / maxFirst;
            Gfx.FillRound(bx, ty, 8, th, 4, 0x9CA3AF);
        }


        // ---- status bar ------------------------------------------------
        // The spec asks for the program basics, so this shows the four facts
        // a writer needs while typing: caret position, document size, save
        // target, and the detected format.
        void DrawStatusBar(int w, int h, int pad)
        {
            int y = h - SB_H;
            Gfx.FillRect(0, y, w, SB_H, 0xF3F4F6);
            Gfx.DrawLine(0, y, w, y, C.Border);

            int cl = t.LineOf(t.cursor) + 1;
            int cc = t.ColOf(t.cursor) + 1;
            string pos = U.Cat(U.Cat("Ln ", U.I(cl)), U.Cat(", Col ", U.I(cc)));
            Gfx.Text(pad, y + 4, pos, C.TextSub);

            string cnt = U.Cat(U.I(t.text.Length), " chars");
            Gfx.Text(pad + 100, y + 4, cnt, C.TextSub);

            string vol = (fs == 3) ? "Desktop" : (fs == 1 ? "SFS" : "MKFS");
            string tgt = U.Cat(U.Cat(vol, ":"), name == null ? "" : name);
            Gfx.Text(pad + 210, y + 4, tgt, C.TextFaint);

            string fmt = DetectLabel();
            int fw = Gfx.Measure(fmt);
            Gfx.Text(w - pad - fw, y + 4, fmt, C.TextFaint);
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

        // ---- save targets ------------------------------------------------
        // Two ways to save:
        //   * Ctrl+S  -> overwrite the document in place (old behaviour, kept
        //               because it is the reflex every user has).
        //   * Save As -> choose the volume FOLDER and a file name first.
        //
        // The folder picker exists because the old editor could only rewrite
        // the exact file it had opened: there was no way to put a new document
        // anywhere, so "save" meant "you must have opened something first".
        static readonly string[] VolNames = { "MKFS root", "Desktop folder" };
        int  saveVol;        // 0 = MKFS root (fs 0), 1 = Desktop (fs 3)
        bool pickingSave;    // showing the Save-As name/folder dialog
        string saveNameBuf;  // file name being typed in the dialog
        int   saveDirWarn;   // 1 = last save failed, show a red hint

        int SaveFs() { return saveVol == 1 ? 3 : 0; }

        void Save()
        {
            // Write back to the volume the document came from.  Always using fs 0
            // silently saved desktop / SFS files into the MKFS root, so the file
            // you opened never changed -- which looked exactly like "data lost".
            int wr = Host.WriteText(fs, name, t.text);
            Host.Log(U.Cat(wr >= 0 ? "[EDITOR] saved " : "[EDITOR] SAVE FAILED ", name));
            saveDirWarn = wr < 0 ? 1 : 0;
            saved = true;
        }

        // Commit a Save-As: the name may carry no extension; append one that
        // matches the detected content so the file is recognisable later.
        void SaveAs()
        {
            // Two-step save: the first call asks, the second commits.  A conflict
            // must be confirmed explicitly: silently overwriting is data loss.
            string target = (saveNameBuf == null || saveNameBuf == "") ? "Untitled.txt" : saveNameBuf;
            int dstFs = SaveFs();
            if (!confirmSave)
            {
                if (Host.FileSize(dstFs, target) >= 0)
                {
                    confirmSave = true;
                    confirmName = target;
                    return;
                }
            }
            confirmSave = false;
            name = target;
            fs = dstFs;
            string nm = saveNameBuf;
            if (nm == null || nm == "") nm = "Untitled.txt";
            // Look for a '.' in the LAST path component only, so a folder name
            // containing a dot cannot be mistaken for an extension.
            int dot = -1;
            for (int i = nm.Length - 1; i >= 0; i--) { if (nm[i] == '.') { dot = i; break; } }
            if (dot <= 0) nm = U.Cat(nm, "." + DetectExt());
            name = nm;
            fs   = SaveFs();
            int wr = Host.WriteText(fs, name, t.text);
            Host.Log(U.Cat(wr >= 0 ? "[EDITOR] saved-as " : "[EDITOR] SAVE-AS FAILED ", name));
            saveDirWarn = wr < 0 ? 1 : 0;
            saved = true;
            pickingSave = false;
        }

        // ---- content sniffing --------------------------------------------
        // The editor used to treat everything as opaque text.  These cheap
        // probes recognise the plain formats that are actually parseable here
        // (there is no PDF/ZIP engine in NexOS), and the title bar reports what
        // was found so "it opened but looks like noise" is explainable.
        // MiniCLR has no string.StartsWith, and the copies in Browser.cs /
        // TerminalApp.cs are private to those classes, so Notepad needs its own.
        static bool StartsWith(string s, string p)
        {
            if (s == null || p == null) return false;
            if (p.Length > s.Length) return false;
            for (int i = 0; i < p.Length; i++) if (s[i] != p[i]) return false;
            return true;
        }

        static string DetectExtStatic(string body)
        {
            if (body == null || body.Length == 0) return "txt";
            if (body.Length >= 12 && StartsWith(body, "{\\rtf")) return "rtf";   // \rtf is RTF's opening
            if (StartsWith(body, "<?xml")) return "xml";
            if (StartsWith(body, "<!DOCTYPE html") || StartsWith(body, "<!doctype html")
                || StartsWith(body, "<html")) return "html";
            if (StartsWith(body, "{") ) return "json";         // no parser: best effort
            if (StartsWith(body, ";;") || StartsWith(body, "; ")) return "ini";
            if (StartsWith(body, "---")) return "md";          // YAML front matter
            // Markdown heuristics: a leading heading is the strongest single cue.
            for (int i = 0; i < body.Length && i < 40; i++)
                if (body[i] == '#') return "md";
            return "txt";
        }
        string DetectExt() { return DetectExtStatic(t.text); }

        // Human-readable description shown next to the file name.
        string DetectLabel()
        {
            string e = DetectExt();
            if (e == "rtf")  return "Rich Text (RTF source)";
            if (e == "xml")  return "XML markup";
            if (e == "html") return "HTML document";
            if (e == "json") return "JSON data";
            if (e == "ini")  return "INI / config";
            if (e == "md")   return "Markdown";
            return "Plain text";
        }

        public override string GetTitle() { return U.Cat("Notepad - ", name); }

        public override void OnPaint()
        {
            W.Clear();
            int w = Gfx.Width(), h = Gfx.Height();

        // A pending overwrite prompt is modal: nothing else may react.
            int pad = 10;

            // Toolbar.
            Gfx.FillRect(0, 0, w, 40, 0xFAFAFA);
            Gfx.DrawLine(0, 40, w, 40, C.Border);
            W.Button(pad, 6, 70, 28, "Open");
            W.Button(pad + 80, 6, 70, 28, "New");
            W.Button(pad + 160, 6, 84, 28, "Save As");
            Gfx.Text(pad + 254, 12, name, C.TextSub);
            if (saved) { int sw = Gfx.Measure(name); Gfx.Text(pad + 262 + sw, 12, "(saved)", 0x107C10); }
            // What the editor thinks the document is.  Shown always (not just
            // for recognised formats) so "plain text" is a positive statement
            // rather than an unexplained absence.
            Gfx.Text(pad + 254, 30, DetectLabel(), C.TextFaint);
            if (saveDirWarn != 0)
                Gfx.Text(pad + 400, 12, "save failed - choose another folder", C.Danger);

            if (confirmSave) { DrawConfirm(w, h); return; }
            if (askClose) { DrawAskClose(w, h); return; }
            if (pickingSave) { DrawSaveAs(w, h); return; }
            if (picking) { DrawPicker(w, h); return; }
            if (binary) { DrawBinary(pad, w, h); return; }

            // Text body.
            Gfx.FillRect(pad, 48, w - 2 * pad, h - 48 - pad, C.White);
            // selection highlight (behind text) - visible for select-all
            if (t.selA == 0 && t.selB == t.text.Length && t.text.Length > 0)
                Gfx.FillRect(pad + 8, 54, w - 2 * pad - 16, h - 48 - pad, 0xCDE6FF);
            mdFirstLine = firstLine;
            DrawLines(TEXT_X, TEXT_Y, t.text, C.Text, h - pad - SB_H, false);
            DrawStatusBar(w, h, pad);
            if (menuOpen) DrawMenu(w, h);
            DrawScrollBar(w, h, pad);

            // caret (on top)
            int cl = t.LineOf(t.cursor);
            int lineStart = OffsetFor(cl, 0);
            int colInLine = t.cursor - lineStart;
            if ((Host.Ticks() / 30) % 2 == 0) {
                int cxp = TEXT_X + TextWidth(t.text, lineStart, colInLine);
                int cyp = TEXT_Y + cl * LINE_H;
                if (cyp < h - pad - SB_H) Gfx.FillRect(cxp, cyp, 2, LINE_H, 0x1A1A1A);
            }
            // Publish the caret origin so the native pinyin candidate /
            // pre-edit popup sits just above this line, aligned with it.
            int anchorY = TEXT_Y + cl * LINE_H;
            if (anchorY > h - SB_H - LINE_H) anchorY = h - SB_H - LINE_H;
            if (anchorY < TEXT_Y) anchorY = TEXT_Y;
#if !WINHOST
            Host.SetImeAnchor(TEXT_X + TextWidth(t.text, lineStart, colInLine), anchorY);
#endif
        }

        // ---- Save As dialog ---------------------------------------------
        // Volume + folder chooser.  Host.WriteText(fs, name, text) addresses a
        // file by (volume, name) with the volume's CURRENT directory implicitly
        // applied, so "custom save location" here means picking the volume and
        // navigating its folders -- that is what the kernel API can actually
        // express.  The current directory is read back through Host.CwdPath so
        // the dialog shows where the write will really land.
        void DrawSaveAs(int w, int h)
        {
            int pad = 10;
            // Dim the editor behind the dialog so the mode is unmistakable.
            Gfx.FillRect(0, 44, w, h - 44, 0xF7F7F7);
            Gfx.Text(pad + 8, 56, "Save as - choose a location and a file name", C.Text);
            int y = 84;

            // Volume selector (two rows, radio-style).
            Gfx.Text(pad + 8, y, "Volume:", C.TextSub);
            y += 22;
            for (int i = 0; i < 2; i++)
            {
                int ry = y + i * W.RowH;
                bool hot = W.Hot(pad, ry, w - 2 * pad, W.RowH - 2);
                if (saveVol == i) Gfx.FillRound(pad, ry, w - 2 * pad, W.RowH - 2, 6, C.Sel);
                else if (hot)     Gfx.FillRound(pad, ry, w - 2 * pad, W.RowH - 2, 6, C.Hover);
                Gfx.Text(pad + 12, ry + 7, saveVol == i ? "(o)" : "( )", C.Text);
                Gfx.Text(pad + 44, ry + 7, VolNames[i], C.Text);
            }
            y += 2 * W.RowH + 10;

            // Current directory of the chosen volume.
            int vfs = SaveFs();
            Gfx.Text(pad + 8, y, U.Cat("Folder: ", Host.CwdPath(vfs)), C.TextSub);
            y += 22;
            Gfx.Text(pad + 8, y, "File name:", C.TextSub);
            y += 22;
            // Name field (plain box; typing is routed from OnKey while visible).
            Gfx.FillRect(pad + 8, y, w - 2 * pad - 16, 30, C.White);
            Gfx.DrawRect(pad + 8, y, w - 2 * pad - 16, 30, C.BorderMid);
            Gfx.Text(pad + 14, y + 7, saveNameBuf == null || saveNameBuf == "" ? "Untitled.txt" : saveNameBuf, C.OnLight);
            // Caret.
            if ((Host.Ticks() / 30) % 2 == 0)
            {
                string shown = saveNameBuf == null || saveNameBuf == "" ? "Untitled.txt" : saveNameBuf;
                Gfx.FillRect(pad + 15 + Gfx.Measure(shown), y + 8, 2, 16, 0x1A1A1A);
            }
#if !WINHOST
            // The Save-As name field is an input box too: anchor the IME
            // popup above it while the dialog owns the keyboard.
            Host.SetImeAnchor(pad + 14, y + 7);
#endif
            y += 44;

            // Probed format so the user sees what will be written.
            Gfx.Text(pad + 8, y, U.Cat("Detected content: ", DetectLabel()), C.TextFaint);
            y += 26;

            W.Button(pad + 8, y, 100, 30, "Save");
            W.Button(pad + 116, y, 100, 30, "Cancel");
            if (saveDirWarn != 0)
                Gfx.Text(pad + 230, y + 8, "last attempt failed (read-only or no such folder)", C.Danger);
        }

        // ---- file picker: MKFS (writable) + SFS (read-only) --------------
        // SFS is a read-only volume, so it is shown for OPENING only: the
        // picker lists it, but saving into it is refused (the Save-As dialog
        // only offers writable volumes).
        int  pickVol;      // 0 = MKFS root, 1 = SFS

        int PickFs() { return pickVol == 1 ? 1 : 0; }

        void DrawPicker(int w, int h)
        {
            int pad = 10, y = 48;
            Gfx.Text(pad + 8, y, "Open a file", C.Text);
            y += 26;

            // Volume switch: MKFS is writable, SFS is read-only.
            W.Button(pad + 8, y, 110, 26, "MKFS root");
            W.Button(pad + 126, y, 90, 26, "SFS (read-only)");
            y += 34;
            Gfx.Text(pad + 8, y, U.Cat("Folder: ", Host.CwdPath(PickFs())), C.TextSub);
            y += 24;

            int n = Host.FileCount(PickFs());
            int shown = 0;
            for (int i = 0; i < n && shown < 12; i++)
            {
                int ry = y + shown * W.RowH;
                bool hot = W.Hot(pad, ry, w - 2 * pad, W.RowH - 2);
                if (hot) Gfx.FillRound(pad, ry, w - 2 * pad, W.RowH - 2, 6, C.Hover);
                int isDir = Host.FileIsDir(PickFs(), i);
                Gfx.Icon(pad + 8, ry + 4, 22, isDir != 0 ? (uint)0xF0B400 : (uint)0x60A5FA,
                         isDir != 0 ? 68 : 70, 0xFFFFFF);   // 68=D 70=F
                Gfx.Text(pad + 40, ry + 8, Host.FileName(PickFs(), i), C.Text);
                shown++;
            }
            if (shown == 0)
                Gfx.Text(pad + 8, y, "(empty)", C.TextFaint);
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
            int n = s.Length, start = 0, lineNo = 0;
            for (int i = 0; i <= n; i++)
            {
                        lineNo++;
                        if (lineNo <= mdFirstLine) { start = i + 1; continue; }
                if (i == n || s[i] == '\n')
                {
                    // A MiniCLR string is a UTF-8 BYTE array, so s[start..i) is
                    // already the correct UTF-8 bytes for this line.  Pass the
                    // substring straight to Gfx.Text: draw_text_utf8 decodes it
                    // and renders CJK properly.  The old per-byte
                    // Host.CharStr(s[j]) re-encoded each UTF-8 byte as its own
                    // codepoint (E4 -> C3 A4 ...), which draw_text_utf8 then
                    // hit its "2-byte -> skip" branch and dropped -> Chinese
                    // silently vanished from the editor.
                    string line = U.Sub(s, start, i - start);
                    if (i == n && caret && (Host.Ticks() / 30) % 2 == 0)
                        line = U.Cat(line, "|");   // ASCII, safe to append
                    if (y < maxY) Gfx.Text(x, y, line, col);
                    y += LINE_H; start = i + 1;
                }
            }
        }

        // Map (line, column) to an absolute offset in the document.
        // A column past the end of a line clamps to that line end: clicking
        // to the right of short text must not jump to the next line.
        int OffsetFor(int line, int col)
        {
            if (line < 0) line = 0;
            if (col  < 0) col  = 0;
            int off = 0, cur = 0;
            while (cur < line && off < t.text.Length)
            {
                if (t.text[off] == (char)10) cur++;
                off++;
            }
            int taken = 0;
            while (taken < col && off < t.text.Length && t.text[off] != (char)10)
            {
                off++; taken++;
            }
            return off;
        }

        // ---- glyph advance model (must match the kernel's mh_measure) ----
        // The body is painted with draw_text_utf8, whose per-glyph advances are
        // 8px for ASCII and ~16px for a CJK codepoint.  A MiniCLR string is a
        // UTF-8 BYTE array, so these helpers decode the sequence starting at
        // index i: one 3-byte CJK char is ONE 16px glyph, not three 16px
        // bytes.  Counting per byte made the caret and the IME popup drift
        // 3x right past every Chinese character.
        static int GlyphAdvOf(string s, int i)
        {
            if ((int)s[i] < 0x80) return 8;   // ASCII / control
            return 16;                        // any multi-byte UTF-8 glyph
        }
        // Bytes consumed by the UTF-8 sequence starting at i.
        static int Utf8Consume(string s, int i)
        {
            int b = (int)s[i];
            if (b < 0x80) return 1;
            if ((b & 0xE0) == 0xC0) return 2;
            if ((b & 0xF0) == 0xE0) return 3;
            if ((b & 0xF8) == 0xF0) return 4;
            return 1;                          // lone continuation byte
        }
        // Pixel width of text[start .. start+n)  (n = BYTE count, e.g. a
        // caret offset within the line; always lands on a codepoint boundary).
        static int TextWidth(string s, int start, int n)
        {
            if (s == null) return 0;
            if (start < 0) start = 0;
            if (n > s.Length - start) n = s.Length - start;
            int w = 0, i = start, end = start + n;
            while (i < end)
            {
                w += GlyphAdvOf(s, i);
                i += Utf8Consume(s, i);
            }
            return w;
        }

        public override void OnWheel(int dy)
        {
            int vis = (Gfx.Height() - SB_H - TEXT_Y) / LINE_H;
            if (vis < 1) vis = 1;
            firstLine -= dy;
            if (firstLine < 0) firstLine = 0;
            int maxFirst = t.LineCount() - vis;
            if (maxFirst < 0) maxFirst = 0;
            if (firstLine > maxFirst) firstLine = maxFirst;
        }

        public override void OnRightClick(int mx, int my, int ox, int oy)
        {
            int pad = 10;
            int w = Gfx.Width(), h = Gfx.Height();
            // Only inside the text body; the toolbar keeps its own behaviour.
            if (U.In(mx, my, TEXT_X, TEXT_Y, w - 2 * pad, h - SB_H - TEXT_Y))
            {
                menuOpen = true;
                menuX = mx;
                menuY = my;
                return;
            }
        }

        public override void OnClick(int mx, int my)
        {

            int w = Gfx.Width(), h = Gfx.Height();
            int pad = 10;
            // A pending overwrite prompt is modal: nothing else may react.
            if (confirmSave)
            {
                int bx = 40, by = 120, bw = w - 80, bh = 130;
                if (U.In(mx, my, bx + 16, by + bh - 44, 120, 30)) { SaveAs(); return; }
                if (U.In(mx, my, bx + 152, by + bh - 44, 100, 30)) { confirmSave = false; return; }
                return;
            }
            // An open context menu swallows the click: a row runs, a miss closes.
            if (menuOpen)
            {
                int row = MenuIndexAt(mx, my);
                if (row >= 0) MenuAction(row);
                else menuOpen = false;
                return;
            }
            if (U.In(mx, my, pad, 6, 70, 28)) { picking = !picking; pickingSave = false; return; }
            if (U.In(mx, my, pad + 80, 6, 70, 28))
            { t.text = ""; name = "Untitled"; picking = false; pickingSave = false; return; }
            if (U.In(mx, my, pad + 160, 6, 84, 28))
            {
                // Open the Save-As dialog, pre-filled with the current name and
                // the volume this document already belongs to.
                pickingSave = !pickingSave;
                picking = false;
                if (saveNameBuf == null || saveNameBuf == "")
                    saveNameBuf = (name == null || name == "Untitled") ? "Untitled.txt" : name;
                saveVol = (fs == 3) ? 1 : 0;
                return;
            }

            if (pickingSave)
            {
                int y0 = 84 + 22;
                // Volume rows.
                for (int i = 0; i < 2; i++)
                {
                    int ry = y0 + i * W.RowH;
                    if (U.In(mx, my, pad, ry, w - 2 * pad, W.RowH - 2)) { saveVol = i; return; }
                }
                int yb = y0 + 2 * W.RowH + 10 + 22 + 22 + 44 + 26;
                if (U.In(mx, my, pad + 8, yb, 100, 30)) { SaveAs(); return; }
                if (U.In(mx, my, pad + 116, yb, 100, 30)) { pickingSave = false; return; }
            }

            if (picking)
            {
                int y = 48 + 26;
                // Volume switch buttons.
                if (U.In(mx, my, pad + 8, y, 110, 26)) { pickVol = 0; return; }
                if (U.In(mx, my, pad + 126, y, 90, 26)) { pickVol = 1; return; }
                y += 34 + 24;              // skip the folder line
                int fsx = PickFs();
                int n = Host.FileCount(fsx);
                int shown = 0;
                for (int i = 0; i < n && shown < 12; i++)
                {
                    int ry = y + shown * W.RowH;
                    if (U.In(mx, my, pad, ry, w - 2 * pad, W.RowH - 2))
                    {
                        // A directory navigates instead of opening.
                        if (Host.FileIsDir(fsx, i) != 0)
                        {
                            Host.EnterDir(fsx, i);
                            return;
                        }
                        // Same loader as the shell handoff: a .mex is binary
                        // and must open in the hex view, not as scrubbed text.
                        LoadFile(fsx, Host.FileName(fsx, i));
                        picking = false;
                        return;
                    }
                    shown++;
                }
            }
            // Click in the text body places the caret.  Without this a click
            // did NOTHING: the caret stayed where typing had left it, so the
            // insertion point and the pointer visibly disagreed.  The mapping
            // inverts the exact metrics DrawLines uses (8px ASCII, 16px CJK),
            // so a click inside or just after a Chinese character lands on the
            // right offset instead of being off by half a character per CJK.
            if (U.In(mx, my, TEXT_X, TEXT_Y, w - 2 * pad, h - SB_H - TEXT_Y))
            {
                int line = (my - TEXT_Y) / LINE_H;
                if (line < 0) line = 0;
                int pos = OffsetFor(line, 0);
                int x = mx - TEXT_X; if (x < 0) x = 0;
                int acc = 0;
                while (pos < t.text.Length && t.text[pos] != (char)10)
                {
                    int adv = GlyphAdvOf(t.text, pos);
                    if (acc + adv / 2 >= x) break;   // nearest glyph wins
                    acc += adv; pos += Utf8Consume(t.text, pos);
                }
                t.cursor = pos;
                t.selA = t.selB = t.cursor;
                return;
            }
        }

        public override void OnKey(int ch)
        {
            // Save-As dialog: this consumes typing, so the document body must
            // not also receive the keystrokes (that would edit the file behind
            // the dialog while the user thinks they are naming it).
            if (pickingSave)
            {
                if (ch == -9) { pickingSave = false; return; }            // Esc
                if (ch == -2 || ch == -3) { SaveAs(); return; }           // Enter / Ctrl+Enter
                if (ch == -4)                                             // Backspace
                {
                    if (saveNameBuf != null && saveNameBuf.Length > 0)
                    {
                        // Drop the last CODEPOINT, not the last byte: a CJK
                        // file name character is 3 UTF-8 bytes and chopping a
                        // single byte would leave a broken sequence on screen.
                        int k = saveNameBuf.Length;
                        while (k > 0 && ((int)saveNameBuf[k - 1] & 0xC0) == 0x80) k--;
                        if (k > 0) k--;
                        saveNameBuf = k > 0 ? U.Sub(saveNameBuf, 0, k) : "";
                    }
                    return;
                }
                if (ch > 0) saveNameBuf = U.Cat(saveNameBuf == null ? "" : saveNameBuf, Host.CharStr(ch));
                return;
            }
            if (picking) { return; }
            if (binary) { return; }                     // binary view is read-only
            if (ch == -10) { Save(); return; }          // Ctrl+S -> save in place
            if (ch == -2) { t.Insert("\n"); saved = false; return; }
                            // Tab inserts 4 spaces.  The shell keeps Tab for window cycling,
                // so an editor must place the indent itself instead of expecting
                // a literal tab character in the key stream.
                if (ch == 9) { t.Insert("    "); saved = false; return; }
                t.Key(ch);
#if !WINHOST
            if (ch >= 0x80)
                Host.Log(U.Cat(U.Cat(U.Cat("[NPK] cjk=", U.I(ch)), U.Cat(" len=", U.I(t.text.Length))), U.Cat(" cur=", U.I(t.cursor))));
#endif
            saved = false;
        }
    }
}
