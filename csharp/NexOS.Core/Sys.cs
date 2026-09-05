// =====================================================================
//  Sys.cs  -  NexOS internal-call surface exposed to managed code
// ---------------------------------------------------------------------
//  Every method marked [MethodImpl(MethodImplOptions.InternalCall)] has
//  no IL body.  tools/mex_pack.py records its fully-qualified name
//  ("NexOS.Sys::Print") and the kernel binds it to a native handler in
//  clr.cpp through the g_icalls table.  Names must stay in sync.
// =====================================================================
using System.Runtime.CompilerServices;

namespace NexOS
{
#if !WINHOST
    // When WINHOST is defined the Windows harness (csharp/winhost) provides
    // a real managed implementation of NexOS.Sys with identical signatures,
    // so these bodyless internal-call declarations are skipped.
    public static class Sys
    {
        // ---- console / serial -------------------------------------
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Print(string s);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void PrintInt(int v);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void PrintChar(char c);

        // ---- string helpers (implemented natively) ----------------
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string StrConcat(string a, string b);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern char StrCharAt(string s, int index);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern int StrLen(string s);

        // Substring / control-char flattening in ONE native allocation each.
        // Building these in managed code with a per-character StrConcat loop
        // costs O(n^2) bytes on the CLR's bump heap and used to exhaust it
        // (which faulted and silently retired the whole managed shell).
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string StrSub(string s, int start, int len);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string StrFlat(string s);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern bool StrEq(string a, string b);

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string IntToStr(int v);

        // ---- misc --------------------------------------------------
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern int TickCount();
    }
#endif // !WINHOST
}
