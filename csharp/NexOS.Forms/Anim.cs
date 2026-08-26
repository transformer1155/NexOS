// =====================================================================
//  Anim.cs  -  NexOS.Forms animation framework
// ---------------------------------------------------------------------
//  Tiny integer-only tween engine.  Constraints (MiniCLR interpreter):
//    * NO floating point, NO generics, NO interfaces, NO string.Format
//    * static field initialisers do NOT run -> arrays are allocated in
//      Ensure() (called lazily on first use), never via field initialisers
//    * the managed heap is rewound every frame -> we keep all tween state
//      in static fields (the static area is NOT rewound), and never `new`
//      inside Paint()
//
//  Everything is expressed as a 0..1000 integer "amount" so callers can
//  lerp colours and scales without any float math.
// =====================================================================
using System.Runtime.CompilerServices;

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

        // Current animated value for a key (0..1000), advanced to `now`.
        public static int Get(int key)
        {
            Ensure();
            for (int i = 0; i < CAP; i++) if (K[i] == key) return V[i];
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
        // when the target actually changes, so calling Hover() every frame does
        // NOT reset the animation.  ez: 0=Cubic, 1=Back(overshoot).
        public static void Set(int key, int target, int durMs, int ez)
        {
            int s = Slot(key);
            int now = Host.TickMs();
            From[s] = V[s];                 // lerp from where we are right now
            To[s] = target;
            Dur[s] = (durMs < 1) ? 1 : durMs;
            Start[s] = now;
            Ez[s] = ez;
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
                int t = now - Start[i];
                if (t >= Dur[i])
                {
                    V[i] = To[i];
                    // settled: if at rest (0) we can free the slot to save scans
                    if (To[i] == 0) { K[i] = -1; }
                    else active = 1;
                }
                else
                {
                    int p = (t * 1000) / Dur[i];          // 0..1000
                    int e = (Ez[i] == 1) ? EaseBack(p) : EaseCubic(p);
                    V[i] = From[i] + ((To[i] - From[i]) * e) / 1000;
                    active = 1;
                }
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
