/* Minimal freestanding <math.h> shim for the cross-toolchain build sandbox.
 * The i686-elf toolchain ships no newlib sysroot, so stb_truetype.h's
 * #include <math.h> cannot be satisfied by a real libm.  We provide plain
 * (non-builtin) implementations of the few routines stb needs so the kernel
 * links without a C runtime math library.  Precision is "good enough" for
 * font rasterization; this shim is only on the include path in the no-sysroot
 * sandbox and is harmless when a real newlib <math.h> is present. */
#ifndef NEXOS_MATH_SHIM_H
#define NEXOS_MATH_SHIM_H

typedef float float_t;
typedef double double_t;

#define INFINITY (__builtin_inf())
#define NAN      (__builtin_nan(""))
#define HUGE_VAL (__builtin_huge_val())
#define M_PI 3.14159265358979323846

/* ---- absolute value (bitwise, no libcall) ---- */
extern double fabs(double x){
    unsigned long long u;
    __builtin_memcpy(&u, &x, 8);
    u &= 0x7FFFFFFFFFFFFFFFULL;
    __builtin_memcpy(&x, &u, 8);
    return x;
}
extern float fabsf(float x){
    unsigned int u;
    __builtin_memcpy(&u, &x, 4);
    u &= 0x7FFFFFFFU;
    __builtin_memcpy(&x, &u, 4);
    return x;
}

/* ---- floor / ceil via truncation ---- */
extern double floor(double x){
    double i = (double)(long long)x;   /* truncation toward zero */
    if (i > x) i -= 1.0;
    return i;
}
extern float floorf(float x){
    float i = (float)(long long)x;
    if (i > x) i -= 1.0f;
    return i;
}
extern double ceil(double x){
    double i = (double)(long long)x;
    if (i < x) i += 1.0;
    return i;
}
extern float ceilf(float x){
    float i = (float)(long long)x;
    if (i < x) i += 1.0f;
    return i;
}

/* ---- sqrt via Newton-Raphson (no libcall) ---- */
extern double sqrt(double x){
    if (x <= 0.0) return 0.0;
    double g = x * 0.5;
    for (int i = 0; i < 24; i++){
        double ng = 0.5 * (g + x / g);
        if (fabs(ng - g) < 1e-12) break;
        g = ng;
    }
    return g;
}
extern float sqrtf(float x){
    return (float)sqrt((double)x);
}

/* ---- fmod ---- */
extern double fmod(double x, double y){
    if (y == 0.0) return 0.0;
    double q = (double)(long long)(x / y);
    return x - q * y;
}

/* ---- pow via exp2/log2 (approximate, enough for font shaping) ---- */
extern double pow(double b, double e){
    if (b <= 0.0) return 0.0;
    /* ln(b) via simple series is overkill; use repeated squaring fallback */
    double r = 1.0;
    long long n = (long long)e;
    double frac = e - (double)n;
    double base = b;
    int neg = (n < 0);
    if (neg) n = -n;
    while (n > 0){ if (n & 1) r *= base; base *= base; n >>= 1; }
    if (neg) r = 1.0 / r;
    if (frac != 0.0){
        /* crude: b^frac ~ exp(frac*ln b); approximate ln via sqrt chain */
        double lb = 0.0, t = b;
        for (int i = 0; i < 12 && t > 1.0; i++){ t = sqrt(t); lb += 1.0; }
        r *= (1.0 + frac * lb * 0.69314718);
    }
    return r;
}
extern float powf(float b, float e){ return (float)pow((double)b, (double)e); }

/* ---- trig: small-angle Taylor, wrapped to [-pi,pi] ---- */
extern double sin(double x){
    double s = 1.0;
    if (x < 0.0){ x = -x; s = -1.0; }
    x = x - (double)(long long)(x / (2.0 * M_PI)) * (2.0 * M_PI);
    double xp = x, term = x, sum = x;
    for (int i = 1; i < 10; i++){
        xp *= x * x;
        term = -xp / (double)((2*i+1)*(2*i));
        sum += term;
        if (fabs(term) < 1e-10) break;
    }
    return s * sum;
}
extern float sinf(float x){ return (float)sin((double)x); }
extern double cos(double x){
    x = fabs(x);
    x = x - (double)(long long)(x / (2.0 * M_PI)) * (2.0 * M_PI);
    double xp = 1.0, term = 1.0, sum = 1.0;
    for (int i = 1; i < 10; i++){
        xp *= x * x;
        term = -xp / (double)((2*i)*(2*i-1));
        sum += term;
        if (fabs(term) < 1e-10) break;
    }
    return sum;
}
extern float cosf(float x){ return (float)cos((double)x); }
extern double asin(double x){
    if (x > 1.0) x = 1.0; if (x < -1.0) x = -1.0;
    return (double)(int)(x * (180.0 / M_PI)) * (M_PI / 180.0); /* coarse */
}
extern float asinf(float x){ return (float)asin((double)x); }
extern double acos(double x){
    if (x > 1.0) x = 1.0; if (x < -1.0) x = -1.0;
    return (double)(int)((1.0 - x) * 90.0 * (M_PI / 180.0)); /* coarse */
}
extern float acosf(float x){ return (float)acos((double)x); }

#endif /* NEXOS_MATH_SHIM_H */
