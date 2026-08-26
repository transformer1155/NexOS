/* Self-contained freestanding <math.h> for the NexOS vector font build.
 *
 * The cross ELF toolchain has no newlib sysroot, and -ffreestanding makes
 * __builtin_sqrt etc. lower to external libm calls we don't link.  So we ship
 * real (IEEE-approximate) implementations using plain arithmetic -- no
 * compiler builtins -- so they inline/copy correctly under -ffreestanding.
 *
 * Only the few routines stb_truetype.h actually references are provided.
 */
#ifndef NEXOS_VECMATH_H
#define NEXOS_VECMATH_H

#define M_PI 3.14159265358979323846

static inline double vec_m_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r = x;
    for (int i = 0; i < 24; i++) {
        double n = (r + x / r) * 0.5;
        if (n == r) break;
        r = n;
    }
    return r;
}
static inline double vec_m_fabs(double x) { return x < 0.0 ? -x : x; }
static inline double vec_m_floor(double x) {
    double i = (double)(long)x;
    return (i == x || x >= 0.0) ? i : i - 1.0;
}
static inline double vec_m_ceil(double x) {
    double i = (double)(long)x;
    return (i == x || x <= 0.0) ? i : i + 1.0;
}
static inline double vec_m_fmod(double a, double b) {
    if (b == 0.0) return 0.0;
    double q = (double)(long)(a / b);
    return a - q * b;
}
static inline double vec_m_pow(double a, double b) {
    if (b < 0.0) return 1.0 / vec_m_pow(a, -b);
    double r = 1.0;
    long e = (long)b;
    double base = a;
    while (e > 0) { if (e & 1) r *= base; base *= base; e >>= 1; }
    return r;
}
static inline double vec_m_cos(double x) {
    const double pi = M_PI;
    x = vec_m_fmod(x, 2.0 * pi);
    if (x >  pi) x -= 2.0 * pi;
    if (x < -pi) x += 2.0 * pi;
    double x2 = x * x, s = 1.0, t = 1.0;
    for (int i = 1; i <= 10; i++) {
        t *= -x2 / (double)((2 * i) * (2 * i - 1));
        s += t;
    }
    return s;
}
static inline double vec_m_asin(double y) {
    /* valid for |y| <= 1; series asin(y) = y + y^3/6 + 3y^5/40 + 5y^7/112 */
    double y2 = y * y;
    return y * (1.0 + y2 * (1.0/6.0 + y2 * (3.0/40.0 + y2 * (5.0/112.0))));
}
static inline double vec_m_acos(double x) {
    if (x >=  1.0) return 0.0;
    if (x <= -1.0) return M_PI;
    double y = vec_m_sqrt((1.0 - x) * (1.0 + x));
    double as = vec_m_asin(y);
    return (x >= 0.0) ? (M_PI / 2.0 - as) : (M_PI / 2.0 + as);
}
static inline double vec_m_sin(double x) {
    return vec_m_cos(x - M_PI / 2.0);
}

/* Map the plain C names stb uses onto our implementations. */
#define sqrt  vec_m_sqrt
#define fabs  vec_m_fabs
#define floor vec_m_floor
#define ceil  vec_m_ceil
#define fmod  vec_m_fmod
#define pow   vec_m_pow
#define cos   vec_m_cos
#define acos  vec_m_acos
#define asin  vec_m_asin
#define sin   vec_m_sin

#endif /* NEXOS_VECMATH_H */
