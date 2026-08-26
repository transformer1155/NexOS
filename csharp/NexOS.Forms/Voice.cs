// =====================================================================
//  Voice.cs  -  decoupled, extensible voice interaction engine
// ---------------------------------------------------------------------
//  Design goals (per project request):
//   * Decoupled from the existing UI event system.  Voice is just another
//     *input source* that injects synthetic clicks -- exactly like the
//     mouse.  It never rewires a control's event handler; it calls the
//     very same App.OnClick / Desktop.Click the mouse would, at the
//     control's centre.  Any future interaction mode (gesture, eye-tracking,
//     serial keypad) can be added as another phrase *source* without
//     touching a single control.
//   * Extensible + uniform interface.  The phrase source is abstracted
//     behind Voice.Say().  Matching + dispatch live in one place.  A real
//     ASR module, the AI agent, or a test harness all feed phrases through
//     the same entry point.
//   * Opt-in per control (the "mark true" rule).  A control is only
//     voice-enabled when its author explicitly tags it via
//     W.Voice(cmd, rect) (or an app calls NexOS.Forms.Voice.Register).
//     By default NO control is voice-enabled.  Theme.VoiceOn is a master
//     microphone switch: when off, nothing registers and nothing fires.
//
//  MiniCLR heap discipline (IMPORTANT):
//   The managed shell paints every frame inside a heap *mark / reset*
//   window (see mforms_paint_desktop / mforms_paint_overlay in mforms.cpp).
//   Any object allocated during paint is freed when the frame ends, so it
//   cannot be referenced later -- in particular a voice control registered
//   during PaintDesktop would dangle by the time PaintOverlay's Drain runs,
//   and any app a handler opens during Drain would be reclaimed.  Therefore
//   this engine stores ALL of its state in primitive buffers that are
//   allocated ONCE in Init() (below the persistent heap baseline) and never
//   re-allocated per frame.  Drain() only matches + fills primitive deferred
//   arrays (no allocation); Dispatch() -- called by the native host AFTER
//   the paint heap is reset -- performs the actual synthetic clicks, so any
//   window/app it opens persists as durable state.  Flat char[] buffers
//   (index math) are used instead of jagged arrays for maximum CLR safety.
// =====================================================================
using NexOS.Forms;

namespace NexOS.Forms
{
    public static class Voice
    {
        const int MAX_REG    = 128;
        const int MAX_PEND   = 32;
        const int CMD_LEN    = 64;   // max chars of a registered command
        const int PHRASE_LEN = 128;  // max chars of an incoming phrase

        // ---- persistent (Init-allocated) registration store ------------
        static char[] cmdBuf;        // flat: reg i command at [i*CMD_LEN .. ]
        static int[] cmdLen;
        static int[] regAppId;       // 0 = desktop surface, >0 = window id
        static int[] regX, regY, regW, regH;
        static int   regN;

        // ---- persistent incoming-phrase store -------------------------
        static char[] pendBuf;       // flat: phrase p at [p*PHRASE_LEN .. ]
        static int[] pendLen;
        static int   pendN;

        // ---- primitive deferred-click queue (filled by Drain, run by
        //      Dispatch AFTER the paint heap reset) ---------------------
        static int[] deferApp;
        static int[] deferX, deferY;
        static int   deferN;

        // ---- lifecycle (static ctors do NOT run under MiniCLR) ----------
        public static void Init()
        {
            cmdBuf   = new char[MAX_REG * CMD_LEN];
            cmdLen   = new int[MAX_REG];
            regAppId = new int[MAX_REG];
            regX = new int[MAX_REG]; regY = new int[MAX_REG];
            regW = new int[MAX_REG]; regH = new int[MAX_REG];

            pendBuf  = new char[MAX_PEND * PHRASE_LEN];
            pendLen  = new int[MAX_PEND];

            deferApp = new int[MAX_PEND];
            deferX   = new int[MAX_PEND];
            deferY   = new int[MAX_PEND];

            regN = 0; pendN = 0; deferN = 0;
        }

        // ---- master switch (mirrors Theme.VoiceOn) ----------------------
        public static void SetEnabled(bool on) { Theme.VoiceOn = on ? 1 : 0; }

        // ---- per-frame registry lifecycle ------------------------------
        // Called once at the very start of each desktop paint frame, before
        // any control re-registers, so the registry stays fresh as windows
        // open/close and controls move.
        public static void BeginFrame() { regN = 0; }

        // Called by W.Voice (or an app) for a voice-enabled control.
        // appId == 0 means a desktop-surface control (screen coordinates);
        // >0 means a window app (the rect is window-local).  The command
        // text is copied into a persistent buffer so it survives the
        // per-frame heap resets.
        public static void Register(string cmd, int x, int y, int w, int h)
        {
            if (Theme.VoiceOn == 0) return;
            if (cmd == null) return;
            int cl = NexOS.Sys.StrLen(cmd);
            if (cl == 0) return;
            if (regN >= MAX_REG) return;
            int appId = (App.Current == null) ? 0 : App.Current.id;
            int base_ = regN * CMD_LEN;
            int n = cl; if (n > CMD_LEN - 1) n = CMD_LEN - 1;
            for (int i = 0; i < n; i++)
                cmdBuf[base_ + i] = NexOS.Sys.StrCharAt(cmd, i);
            cmdLen[regN] = n;
            regAppId[regN] = appId;
            regX[regN] = x; regY[regN] = y; regW[regN] = w; regH[regN] = h;
            regN++;
        }

        // ---- phrase ingestion (the extensible entry point) -------------
        // Any interaction backend calls this with a recognised phrase.  The
        // text is copied into a persistent buffer so it survives until the
        // next frame's Drain().
        public static void Say(string phrase)
        {
            if (phrase == null) return;
            if (pendN >= MAX_PEND) return;
            int pl = NexOS.Sys.StrLen(phrase);
            int base_ = pendN * PHRASE_LEN;
            int n = pl; if (n > PHRASE_LEN - 1) n = PHRASE_LEN - 1;
            for (int i = 0; i < n; i++)
                pendBuf[base_ + i] = NexOS.Sys.StrCharAt(phrase, i);
            pendLen[pendN] = n;
            pendN++;
        }

        // ---- dispatch stage 1: match (runs INSIDE the paint heap mark) --
        // Match every pending phrase against the registered controls and
        // enqueue the resolved synthetic click.  Allocates nothing -- it
        // only fills primitive deferred arrays -- so it is safe to run
        // before the paint heap is reset.
        public static void Drain()
        {
            if (pendN == 0) return;
            for (int p = 0; p < pendN; p++)
            {
                int pbase = p * PHRASE_LEN;
                int phlen = pendLen[p];
                int best = -1, bestScore = 0;
                for (int i = 0; i < regN; i++)
                {
                    int ibase = i * CMD_LEN;
                    if (LooseMatch(pendBuf, pbase, phlen, cmdBuf, ibase, cmdLen[i]))
                    {
                        // Longest command wins, so "浏览器" outranks a
                        // shorter alias that is a substring of it.
                        if (cmdLen[i] > bestScore) { bestScore = cmdLen[i]; best = i; }
                    }
                }
                if (best >= 0)
                {
                    int cx = regX[best] + regW[best] / 2;
                    int cy = regY[best] + regH[best] / 2;
                    if (deferN < MAX_PEND)
                    {
                        deferApp[deferN] = regAppId[best];
                        deferX[deferN]   = cx;
                        deferY[deferN]   = cy;
                        deferN++;
                    }
                }
            }
            pendN = 0;
        }

        // ---- dispatch stage 2: execute (runs AFTER the paint heap reset)
        // Perform the deferred synthetic clicks.  Called by the native GUI
        // host (mforms_paint_overlay in mforms.cpp) once the paint heap is
        // reset, so any window/app a handler opens becomes persistent state.
        // This reuses the exact same App.OnClick / Desktop.Click the real
        // mouse would invoke.
        public static void Dispatch()
        {
            if (deferN == 0) return;
            for (int i = 0; i < deferN; i++)
            {
                int appId = deferApp[i];
                int cx = deferX[i], cy = deferY[i];
                Gfx.SetMouse(cx, cy);   // cosmetic: move the cursor sprite
                if (appId == 0)
                {
                    // Desktop-surface control (icon / taskbar / Start menu).
                    int k = Desktop.Click(cx, cy);
                    if (k >= 0) Shell.Open(k);
                }
                else
                {
                    // Window control: reuse the real mouse dispatch path.
                    Shell.Click(appId, cx, cy);
                }
            }
            Host.Log("VOICE:ok");
            deferN = 0;
        }

        // ---- fuzzy, allocation-free, case/space/punct-insensitive match -
        static bool IsSkip(char c)
        {
            return c == ' ' || c == '\t' || c == ',' || c == '.' || c == '!' ||
                   c == '?' || c == ':'  || c == '"' || c == '\'' ||
                   c == '。' || c == '，' || c == '！' || c == '？' ||
                   c == '：' || c == '“' || c == '”' || c == '、' ||
                   c == '（' || c == '）' || c == '·';
        }
        static char Lower(char c)
        {
            if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
            return c;
        }
        // True if `needle` (loosely) appears inside `hay`, ignoring case,
        // whitespace and punctuation.  "open browser" matches "browser";
        // "打开浏览器" matches "浏览器"; "press 1" matches "1".
        // hay/needle are views into flat buffers given by (buf, off, len).
        static bool LooseMatch(char[] hay, int hoff, int hlen,
                               char[] needle, int noff, int nlen)
        {
            if (nlen == 0) return true;
            if (nlen > hlen) return false;
            int hi = 0;
            while (hi < hlen)
            {
                char hc = hay[hoff + hi];
                if (IsSkip(hc)) { hi++; continue; }
                int pp = hi, q = 0, matched = 0;
                while (pp < hlen && q < nlen)
                {
                    char a = hay[hoff + pp];
                    char b = needle[noff + q];
                    if (IsSkip(a)) { pp++; continue; }
                    if (IsSkip(b)) { q++; continue; }
                    if (Lower(a) != Lower(b)) break;
                    pp++; q++; matched++;
                }
                if (q >= nlen && matched > 0) return true;
                hi++;
            }
            return false;
        }
    }
}
