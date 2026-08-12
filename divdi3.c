// =====================================================================
//  divdi3.c  -  minimal 64-bit integer division/modulo helpers
// ---------------------------------------------------------------------
//  The freestanding 32-bit kernel is built with -m32 -nostdlib and has
//  NO 32-bit libgcc available in this environment, so the compiler's
//  64-bit / and % operators (which lower to __udivdi3 / __umoddi3 /
//  __divdi3 / __moddi3) are unresolved at link time.  These are the
//  canonical libgcc routines, implemented with shift-and-subtract so
//  they never themselves call back into a division helper.
//
//  Only used during boot (e.g. GGUF model loading); correctness over
//  speed is fine here.
// =====================================================================
typedef unsigned int    u32;
typedef unsigned long long u64;
typedef long long       i64;

// Exported with C linkage so the names stay unmangled whether this file is
// compiled as C or (as happens when CC=g++) as C++. libgcc's __udivdi3 etc.
// are expected by the compiler as raw C symbols.
#ifdef __cplusplus
extern "C" {
#endif

u64 __udivdi3(u64 num, u64 den) {
    u64 quot = 0, rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1ULL);
        if (rem >= den) { rem -= den; quot |= (1ULL << i); }
    }
    return quot;
}

u64 __umoddi3(u64 num, u64 den) {
    u64 rem = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((num >> i) & 1ULL);
        if (rem >= den) rem -= den;
    }
    return rem;
}

i64 __divdi3(i64 a, i64 b) {
    int sign = 0;
    u64 ua = (u64)a; if (a < 0) { ua = (u64)(-a); sign ^= 1; }
    u64 ub = (u64)b; if (b < 0) { ub = (u64)(-b); sign ^= 1; }
    u64 q = __udivdi3(ua, ub);
    return sign ? -(i64)q : (i64)q;
}

i64 __moddi3(i64 a, i64 b) {
    int sign = 0;
    u64 ua = (u64)a; if (a < 0) { ua = (u64)(-a); sign = 1; }
    u64 ub = (u64)b; if (b < 0) { ub = (u64)(-b); }
    u64 r = __umoddi3(ua, ub);
    return sign ? -(i64)r : (i64)r;
}

#ifdef __cplusplus
}
#endif
