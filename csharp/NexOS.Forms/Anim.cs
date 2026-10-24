namespace NexOS.Forms
{
    public static class Anim
    {
        // ---- recommended motion durations (ms) -------------------------
        // Centralised so the whole UI can share one Fluent-like rhythm.
        public const int DUR_FAST  = 120;   // hover / check / radio
        public const int DUR_SOFT  = 170;   // popup / start-menu open
        public const int DUR_MED   = 200;   // toggle / window/chrome
        public const int DUR_SLIDE = 220;   // start menu / toast slide
        public const int DUR_SLOW  = 300;   // progress / bar fill

        // ---- easing: input/output are 0..1000 -------------------------
        public static int EaseLinear(int t) { return t; }

        public static int EaseQuad(int t)
        {
            // ease-in-out quadratic on integer t (0..1000)
            if (t < 500) { int x = t * 2; return (x * x) / 1000 / 2; }
            int y = 1000 - t; int z = y * 2; return 1000 - (z * z) / 1000 / 2;
        }

        public static int EaseCubic(int t)
        {
            if (t < 500)
            {
                int x = t * 2;            // 0..1000
                int x2 = (x * x) / 1000;  // x^2
                int x3 = (x2 * x) / 1000; // x^3
                return x3 / 2;
            }
            int y = 1000 - t; int y2 = (y * y) / 1000; int y3 = (y2 * y) / 1000;
            return 1000 - y3 / 2;
        }

        // Back easing: overshoots past 1000 then settles (press bounce)
        public static int EaseBack(int t)
        {
            // a = 1.70158 scaled to ints; approximate with 1700/1000
            int s = 1700;
            int tp = t - 1000;                  // -1000..0
            int tp2 = (tp * tp) / 1000;
            int tp3 = (tp2 * tp) / 1000;
            int r = 1000 + (s + 1000) * tp3 + s * tp2;
            if (r < 0) r = 0;
            if (r > 1300) r = 1300;            // clamp overshoot
            return r;
        }

        // ---- tween pool (keyed, fixed capacity) -----------------------
        const int CAP = 256;
        static int[] K   = null;   // control key
        static int[] V   = null;   // current value 0..1000
        static int[] To  = null;   // target value 0..1000
        static int[] From = null;  // value at animation start
        static int[] Dur = null;   // duration ms
        static int[] Start = null; // start ms (Host.TickMs)
        static int[] Ez  = null;   // easing: 0=Cubic, 1=Back(overshoot)
        static int   Inited = 0;

        static void Ensure()
        {
            if (Inited == 1) return;
            K     = new int[CAP];
            V     = new int[CAP];
            To    = new int[CAP];
            From  = new int[CAP];
            Dur   = new int[CAP];
            Start = new int[CAP];
            Ez    = new int[CAP];
            for (int i = 0; i < CAP; i++) { K[i] = -1; V[i] = 0; To[i] = 0; From[i] = 0; Dur[i] = 1; Start[i] = 0; Ez[i] = 0; }
            Inited = 1;
        }

        static int Slot(int key)
        {
            Ensure();
            int free = -1;
            for (int i = 0; i < CAP; i++)
            {
                if (K[i] == key) return i;
                if (K[i] == -1 && free < 0) free = i;
            }
            if (free < 0) free = 0;   // pool full: recycle slot 0
            K[free] = key; V[free] = 0; To[free] = 0; Dur[free] = 1; Start[free] = 0;
            return free;
        }

        // Advance one slot to its eased value at time `now`.  Shared by Tick()
        // (batch) and Get() (lazy read).  Idempotent: a slot settled at 0 is
        // freed; a slot settled at a non-zero rest value is kept (so Get keeps
        // returning it) but then no longer drives the repaint loop.
        static void Advance(int i, int now)
        {
            if (K[i] < 0) return;
            int t = now - Start[i];
            if (t >= Dur[i])
            {
                V[i] = To[i];
                if (To[i] == 0) K[i] = -1;   // settled at rest -> free slot
            }
            else
            {
                int p = (t * 1000) / Dur[i];          // 0..1000
                int e = (Ez[i] == 1) ? EaseBack(p) : EaseCubic(p);
                V[i] = From[i] + ((To[i] - From[i]) * e) / 1000;
            }
        }

        // Current animated value for a key (0..1000).  Lazily advances the slot
        // to the present time so the returned value is always live regardless of
        // whether Tick() has run since Set().  This is the root-cause fix for the
        // "blank first frame / menu never animates" bug: an open transition
        // becomes visible on the very first frame after Set() fired, instead of
        // freezing at V=0 waiting for the next frame's Tick.
        public static int Get(int key)
        {
            Ensure();
            int now = Host.TickMs();
            for (int i = 0; i < CAP; i++)
            {
                if (K[i] == key) { Advance(i, now); return V[i]; }
            }
            return 0;
        }

        // Target value currently set for a key (0..1000), or -1 if no slot.
        public static int ToOf(int key)
        {
            Ensure();
            for (int i = 0; i < CAP; i++) if (K[i] == key) return To[i];
            return -1;
        }

        // Drive a key toward `target` (0..1000) over `durMs`.  Only restarts
        // when the target (or easing) actually changes, so calling Set() every
        // frame from a Paint() must NOT reset the tween clock.  ez: 0=Cubic,
        // 1=Back(overshoot).
        //
        // The guard `if (To[s] == target && Ez[s] == ez) return;` is load-bearing:
        // without it the Start menu / popup / toggle / progress-bar animations
        // never play (opening tweens snap to end, closing tweens stall because
        // their clock is re-zeroed each frame).  DO NOT REMOVE.
        public static void Set(int key, int target, int durMs, int ez)
        {
            int s = Slot(key);
            if (To[s] == target && Ez[s] == ez) return;   // already heading there
            int now = Host.TickMs();
            From[s] = V[s];                 // lerp from where we are right now
            To[s] = target;
            Dur[s] = (durMs < 1) ? 1 : durMs;
            Start[s] = now;
            Ez[s] = ez;
            // Keep the repaint loop alive.  Set() usually runs during Paint,
            // AFTER this frame's Anim.Tick() already executed (Tick sits at the
            // top of PaintDesktop, before Popup/StartMenu call Set).  Without
            // re-arming, a brand-new slot would never get ticked again and the
            // menu would freeze at V=0.  Re-arming guarantees the open/close
            // transition actually plays -- this is what removes the need for
            // Anim.Snap as a workaround.
            Host.SetAnim(1);
        }

        // Force a key to an explicit value immediately (bypasses tween clock).
        // Extreme fallback only: prefer Set()+Tick() real easing.  Retained for
        // the few callers (e.g. Popup.Open) that still need an instant state.
        public static void Snap(int key, int value)
        {
            int s = Slot(key);
            int now = Host.TickMs();
            V[s] = value;
            From[s] = value;
            To[s] = value;
            Start[s] = now;
            Host.SetAnim(1);   // keep loop consistent with Set()
        }

        public static void Set(int key, int target, int durMs)
        {
            Set(key, target, durMs, 0);
        }

        // ---- convenience controls -------------------------------------
        // Hover amount 0..1000 for a control (1000 when pointer inside).
        public static int Hover(int key, int hot)
        {
            int target = (hot != 0) ? 1000 : 0;
            if (ToOf(key) != target) Set(key, target, 150);
            return Get(key);
        }

        // Press amount 0..1000 (1000 = fully pressed/shrunk, 0 = relaxed).
        public static int Press(int key, int pressing)
        {
            int target = (pressing != 0) ? 1000 : 0;
            if (ToOf(key) != target) Set(key, target, (pressing != 0) ? 100 : 150);
            return Get(key);
        }

        // ---- frame tick: advance all, report activity ----------------
        public static void Tick()
        {
            Ensure();
            int now = Host.TickMs();
            int active = 0;
            for (int i = 0; i < CAP; i++)
            {
                if (K[i] < 0) continue;
                Advance(i, now);
                // Only keep the loop alive while the slot is still moving; a
                // settled slot (even at a non-zero rest value) stops driving
                // frames so the UI can go idle instead of repainting forever.
                if (K[i] >= 0 && (now - Start[i]) < Dur[i]) active = 1;
            }
            Host.SetAnim(active);
        }

        // Stable key from a control rect (layout-fixed controls keep the same
        // key; scrolling lists should pass a content id instead of x/y).
        public static int Key(int x, int y, int w, int h)
        {
            int k = (x * 73856093) ^ (y * 19349663) ^ (w * 83492791) ^ (h * 28199459);
            if (k == -1) k = 0;
            return k;
        }
    }
}
