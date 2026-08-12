/* Freestanding shim <math.h> for MiniOS Mono port (PAL).
 *
 * eglib/ghashtable.c includes <math.h> but never calls into it; other Mono
 * users only need a handful of classification macros.  Anything that really
 * needs libm gets added here (and implemented in pal/libc_impl.c) on demand.
 */
#ifndef PAL_MATH_H
#define PAL_MATH_H

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define fabs(x)       __builtin_fabs(x)
#define fabsf(x)      __builtin_fabsf(x)

/* C99 comparison macros - mono-math-c.c wraps these for the interpreter. */
#define isunordered(x, y)    __builtin_isunordered((x), (y))
#define isgreater(x, y)      __builtin_isgreater((x), (y))
#define isgreaterequal(x, y) __builtin_isgreaterequal((x), (y))
#define isless(x, y)         __builtin_isless((x), (y))
#define islessequal(x, y)    __builtin_islessequal((x), (y))
#define islessgreater(x, y)  __builtin_islessgreater((x), (y))

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) \
    __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x))

double frexp(double value, int *exp);
double ldexp(double x, int exp);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double sqrt(double x);
double trunc(double x);
double round(double x);
double copysign(double x, double y);
float  floorf(float x);
float  ceilf(float x);
float  fmodf(float x, float y);
float  sqrtf(float x);
float  truncf(float x);
float  roundf(float x);
float  copysignf(float x, float y);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double cbrt(double x);
double modf(double x, double *iptr);
/* C99 反双曲 / 杂项：metadata/sysmath.c 直接映射到 System.Math 的同名方法 */
double asinh(double x);
double acosh(double x);
double atanh(double x);
double fma(double x, double y, double z);
double scalbn(double x, int n);
double scalbln(double x, long n);
int    ilogb(double x);
double logb(double x);
double expm1(double x);
double log1p(double x);
double hypot(double x, double y);
double nextafter(double x, double y);
double remainder(double x, double y);
double rint(double x);
double nearbyint(double x);
float  asinhf(float x);
float  acoshf(float x);
float  atanhf(float x);
float  fmaf(float x, float y, float z);
float  scalbnf(float x, int n);
float  hypotf(float x, float y);
float  rintf(float x);
float  expf(float x);
float  logf(float x);
float  powf(float x, float y);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  atan2f(float y, float x);
float  asinf(float x);
float  acosf(float x);
float  atanf(float x);
float  sinhf(float x);
float  coshf(float x);
float  tanhf(float x);
float  log10f(float x);
float  log2f(float x);
float  exp2f(float x);
float  cbrtf(float x);
float  modff(float x, float *iptr);
float  ldexpf(float x, int e);
float  frexpf(float x, int *e);
float  logbf(float x);
int    ilogbf(float x);
float  remainderf(float x, float y);
float  nextafterf(float x, float y);
float  log1pf(float x);
float  expm1f(float x);

/* 数学常量：ISO C 没规定 <math.h> 一定要有，但 Mono 的
 * threadpool-worker-default.c 直接用 M_PI 算爬山算法的步长。 */
#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#endif /* PAL_MATH_H */
