/* pal/libm_impl.c -- freestanding libm for the MiniOS Mono port.
 *
 * Everything is x87, which is the one FPU an i386 MiniOS target is guaranteed
 * to have.  Accuracy is whatever the 80-bit stack gives us -- fine for the
 * interpreter's Math.* surface, and far cheaper than dragging in a soft-float
 * library.
 *
 * Rounding-mode note: floor/ceil/trunc each need a *different* x87 rounding
 * mode, so they save the control word, patch bits 10-11, round, and restore.
 * Getting this wrong silently turns floor() into round-to-nearest, which is
 * the classic way a ported runtime starts producing off-by-one indices.
 */

#include <math.h>

/* ---- rounding-mode helper -------------------------------------------
 * x87 control word bits 10..11:  00 nearest / 01 down / 10 up / 11 truncate
 */
#define X87_RC_NEAREST 0x0000
#define X87_RC_DOWN    0x0400
#define X87_RC_UP      0x0800
#define X87_RC_TRUNC   0x0C00

static double round_with_ (double x, unsigned short rc)
{
	unsigned short cw, saved;
	double r;

	__asm__ __volatile__ ("fnstcw %0" : "=m"(saved));
	cw = (unsigned short)((saved & ~0x0C00u) | rc);
	__asm__ __volatile__ ("fldcw %0" :: "m"(cw));
	__asm__ __volatile__ ("frndint" : "=t"(r) : "0"(x));
	__asm__ __volatile__ ("fldcw %0" :: "m"(saved));
	return r;
}

double floor (double x) { return round_with_ (x, X87_RC_DOWN);  }
double ceil  (double x) { return round_with_ (x, X87_RC_UP);    }
double trunc (double x) { return round_with_ (x, X87_RC_TRUNC); }

/* C99 round(): halfway cases go away from zero, which x87 cannot do
 * directly (its "nearest" is banker's rounding), so bias then truncate. */
double round (double x)
{
	return (x >= 0.0) ? trunc (x + 0.5) : trunc (x - 0.5);
}

double sqrt (double x)
{
	double r;
	__asm__ __volatile__ ("fsqrt" : "=t"(r) : "0"(x));
	return r;
}

double fmod (double x, double y)
{
	double r;
	unsigned short sw;

	/* fprem is iterative: one execution may only reduce the exponent by 63,
	 * so loop while C2 (bit 2 of the status word's high byte) is set.
	 * The trailing fstp pops the divisor, which the "u" input constraint
	 * requires the asm to do -- leave it out and the x87 stack overflows
	 * after a few hundred calls. */
	__asm__ __volatile__ ("1:\n\t"
	                      "fprem\n\t"
	                      "fnstsw %%ax\n\t"
	                      "testb $0x04, %%ah\n\t"
	                      "jnz 1b\n\t"
	                      "fstp %%st(1)\n\t"
	                      : "=t"(r), "=a"(sw)
	                      : "0"(x), "u"(y));
	(void)sw;
	return r;
}

double copysign (double x, double y)
{
	union { double d; unsigned long long u; } a, b;
	a.d = x;
	b.d = y;
	a.u = (a.u & 0x7FFFFFFFFFFFFFFFULL) | (b.u & 0x8000000000000000ULL);
	return a.d;
}

double frexp (double value, int *exp)
{
	union { double d; unsigned long long u; } v;
	int e;

	v.d = value;
	e = (int)((v.u >> 52) & 0x7FF);
	if (e == 0) {                   /* zero or subnormal */
		if ((v.u & 0x000FFFFFFFFFFFFFULL) == 0) {
			if (exp) *exp = 0;
			return value;
		}
		/* normalise the subnormal by scaling up 2^64 then correcting */
		v.d = value * 18446744073709551616.0;
		e = (int)((v.u >> 52) & 0x7FF) - 64;
	}
	if (e == 0x7FF) {               /* inf / nan */
		if (exp) *exp = 0;
		return value;
	}
	if (exp) *exp = e - 1022;
	v.u = (v.u & ~0x7FF0000000000000ULL) | 0x3FE0000000000000ULL;
	return v.d;
}

double ldexp (double x, int exp)
{
	double r;
	double e = (double)exp;
	/* fscale does not pop, so pop the scale factor explicitly. */
	__asm__ __volatile__ ("fscale\n\tfstp %%st(1)" : "=t"(r) : "0"(x), "u"(e));
	return r;
}

double modf (double x, double *iptr)
{
	double i = trunc (x);
	if (iptr) *iptr = i;
	return x - i;
}

/* ---- transcendentals ------------------------------------------------ */

double log (double x)           /* ln x = log2(x) * ln 2 */
{
	double r;
	__asm__ __volatile__ ("fldln2\n\tfxch %%st(1)\n\tfyl2x"
	                      : "=t"(r) : "0"(x));
	return r;
}

double log2 (double x)
{
	double r;
	__asm__ __volatile__ ("fld1\n\tfxch %%st(1)\n\tfyl2x"
	                      : "=t"(r) : "0"(x));
	return r;
}

double log10 (double x)
{
	double r;
	__asm__ __volatile__ ("fldlg2\n\tfxch %%st(1)\n\tfyl2x"
	                      : "=t"(r) : "0"(x));
	return r;
}

/* 2^x for any x: f2xm1 only covers [-1,1], so split into integer and
 * fractional parts and recombine with fscale. */
static double exp2_ (double x)
{
	double i, f, r;

	i = round_with_ (x, X87_RC_TRUNC);
	f = x - i;
	__asm__ __volatile__ ("f2xm1" : "=t"(r) : "0"(f));
	r += 1.0;
	__asm__ __volatile__ ("fscale\n\tfstp %%st(1)" : "=t"(r) : "0"(r), "u"(i));
	return r;
}

double exp (double x)  { return exp2_ (x * 1.4426950408889634074); } /* x/ln2 */

double pow (double x, double y)
{
	if (y == 0.0) return 1.0;
	if (x == 0.0) return 0.0;
	if (x < 0.0) {
		/* Only integral exponents are defined for a negative base. */
		double iy = trunc (y);
		if (iy != y) return NAN;
		{
			double m = exp2_ (log2 (-x) * y);
			return (fmod (iy, 2.0) != 0.0) ? -m : m;
		}
	}
	return exp2_ (log2 (x) * y);
}

double sin (double x) { double r; __asm__ __volatile__ ("fsin" : "=t"(r) : "0"(x)); return r; }
double cos (double x) { double r; __asm__ __volatile__ ("fcos" : "=t"(r) : "0"(x)); return r; }

double tan (double x)
{
	double r, one;
	__asm__ __volatile__ ("fptan" : "=t"(one), "=u"(r) : "0"(x));
	(void)one;
	return r;
}

double atan (double x)
{
	double r;
	__asm__ __volatile__ ("fld1\n\tfpatan" : "=t"(r) : "0"(x));
	return r;
}

double atan2 (double y, double x)
{
	double r;
	/* fpatan computes atan(st(1)/st(0)) and pops, so st(0) must be x. */
	__asm__ __volatile__ ("fpatan" : "=t"(r) : "0"(x), "u"(y));
	return r;
}

double asin (double x) { return atan2 (x, sqrt (1.0 - x * x)); }
double acos (double x) { return atan2 (sqrt (1.0 - x * x), x); }

double sinh (double x) { double e = exp (x); return (e - 1.0 / e) * 0.5; }
double cosh (double x) { double e = exp (x); return (e + 1.0 / e) * 0.5; }
double tanh (double x)
{
	double e = exp (2.0 * x);
	return (e - 1.0) / (e + 1.0);
}

double cbrt (double x)
{
	if (x == 0.0) return 0.0;
	return (x > 0.0) ? exp2_ (log2 (x) / 3.0) : -exp2_ (log2 (-x) / 3.0);
}

/* ---- C99 反双曲 / 杂项 ----------------------------------------------
 * System.Math.Asinh/Acosh/Atanh 会一路打到这里，所以要挑数值上稳一点的
 * 恒等式：asinh 对 |x| 小的时候用 log1p 形式避免 sqrt(x^2+1)-|x| 的相消。
 */
double log1p (double x)
{
	/* ln(1+x)；|x| 很小时 1+x 会把有效位吃掉，用 x/((1+x)-1) 做补偿 */
	double u = 1.0 + x;
	if (u == 1.0) return x;
	return log (u) * (x / (u - 1.0));
}

double expm1 (double x)
{
	double u = exp (x);
	if (u == 1.0) return x;
	if (u - 1.0 == -1.0) return -1.0;
	return (u - 1.0) * (x / log (u));
}

double asinh (double x)
{
	double a = x < 0.0 ? -x : x;
	double r;
	if (a > 1.0e8)                 /* 大数：sqrt(x^2+1) ≈ |x| */
		r = log (a) + 0.6931471805599453094;   /* + ln2 */
	else
		r = log1p (a + a * a / (1.0 + sqrt (a * a + 1.0)));
	return x < 0.0 ? -r : r;
}

double acosh (double x)
{
	if (x < 1.0) return (x - x) / 0.0;         /* NaN，定义域外 */
	if (x > 1.0e8) return log (x) + 0.6931471805599453094;
	return log (x + sqrt (x * x - 1.0));
}

double atanh (double x)
{
	double a = x < 0.0 ? -x : x;
	double r = 0.5 * log1p (2.0 * a / (1.0 - a));
	return x < 0.0 ? -r : r;
}

double hypot (double x, double y)
{
	double a = x < 0.0 ? -x : x;
	double b = y < 0.0 ? -y : y;
	double t;
	if (a < b) { t = a; a = b; b = t; }
	if (a == 0.0) return 0.0;
	t = b / a;
	return a * sqrt (1.0 + t * t);             /* 先约分，避免 x^2 溢出 */
}

/* fma：x87 的寄存器栈本来就是 80 位（64 位尾数），把三个操作数提到
 * long double 再算，中间那次乘法就不会先被舍到 53 位。
 * 注意这不是 IEEE 意义上"只舍入一次"的真 FMA —— double×double 要 106 位
 * 尾数才精确，x87 只有 64 位。但它严格优于 (double)(x*y)+z，
 * 对 System.Math.FusedMultiplyAdd 的常规用法足够。真正需要正确舍入时
 * 要上 Dekker 拆分，留到 P2 再说。 */
double fma (double x, double y, double z)
{
	long double r = (long double)x * (long double)y + (long double)z;
	return (double)r;
}

double logb (double x)
{
	double r;
	if (x == 0.0) return -1.0 / 0.0;
	__asm__ __volatile__ ("fxtract\n\tfstp %%st(0)" : "=t"(r) : "0"(x));
	return r;
}

int ilogb (double x)
{
	if (x == 0.0)  return -2147483647 - 1;     /* FP_ILOGB0 */
	if (x != x)    return -2147483647 - 1;
	return (int)logb (x);
}

double scalbn (double x, int n)  { return ldexp (x, n); }
double scalbln (double x, long n){ return ldexp (x, (int)n); }

double rint (double x)
{
	double r;                                   /* 用当前舍入模式（默认最近偶） */
	__asm__ __volatile__ ("frndint" : "=t"(r) : "0"(x));
	return r;
}
double nearbyint (double x) { return rint (x); }

double remainder (double x, double y)
{
	double r;
	unsigned short sw;
	__asm__ __volatile__ ("1: fprem1\n\t"
	                      "fnstsw %%ax\n\t"
	                      "testb $0x04, %%ah\n\t"
	                      "jnz 1b\n\t"
	                      "fstp %%st(1)"
	                      : "=t"(r), "=a"(sw) : "0"(x), "u"(y));
	return r;
}

double nextafter (double x, double y)
{
	union { double d; unsigned long long u; } a;
	if (x != x || y != y) return x + y;
	if (x == y) return y;
	a.d = x;
	if (x == 0.0) { a.u = 1; return y > 0.0 ? a.d : -a.d; }
	if ((x < y) == (x > 0.0)) a.u++; else a.u--;
	return a.d;
}

/* ---- float wrappers ------------------------------------------------- */
float asinhf (float x)            { return (float)asinh ((double)x); }
float acoshf (float x)            { return (float)acosh ((double)x); }
float atanhf (float x)            { return (float)atanh ((double)x); }
float fmaf   (float x, float y, float z) { return (float)fma ((double)x, (double)y, (double)z); }
float scalbnf(float x, int n)     { return (float)ldexp ((double)x, n); }
float hypotf (float x, float y)   { return (float)hypot ((double)x, (double)y); }
float rintf  (float x)            { return (float)rint ((double)x); }
float expf   (float x)            { return (float)exp ((double)x); }
float logf   (float x)            { return (float)log ((double)x); }
float powf   (float x, float y)   { return (float)pow ((double)x, (double)y); }
float sinf   (float x)            { return (float)sin ((double)x); }
float cosf   (float x)            { return (float)cos ((double)x); }
float tanf   (float x)            { return (float)tan ((double)x); }
float atan2f (float y, float x)   { return (float)atan2 ((double)y, (double)x); }
float asinf  (float x)            { return (float)asin ((double)x); }
float acosf  (float x)            { return (float)acos ((double)x); }
float atanf  (float x)            { return (float)atan ((double)x); }
float sinhf  (float x)            { return (float)sinh ((double)x); }
float coshf  (float x)            { return (float)cosh ((double)x); }
float tanhf  (float x)            { return (float)tanh ((double)x); }
float log10f (float x)            { return (float)log10 ((double)x); }
float log2f  (float x)            { return (float)log2 ((double)x); }
float exp2f  (float x)            { return (float)exp2_ ((double)x); }
float cbrtf  (float x)            { return (float)cbrt ((double)x); }
float ldexpf (float x, int e)     { return (float)ldexp ((double)x, e); }
float frexpf (float x, int *e)    { return (float)frexp ((double)x, e); }
float logbf  (float x)            { return (float)logb ((double)x); }
int   ilogbf (float x)            { return ilogb ((double)x); }
float remainderf (float x, float y) { return (float)remainder ((double)x, (double)y); }
float nextafterf (float x, float y) { return (float)nextafter ((double)x, (double)y); }
float log1pf (float x)            { return (float)log1p ((double)x); }
float expm1f (float x)            { return (float)expm1 ((double)x); }
float modff  (float x, float *iptr)
{
	double ip, fp = modf ((double)x, &ip);
	if (iptr) *iptr = (float)ip;
	return (float)fp;
}
float floorf    (float x)          { return (float)floor ((double)x); }
float ceilf     (float x)          { return (float)ceil ((double)x); }
float truncf    (float x)          { return (float)trunc ((double)x); }
float roundf    (float x)          { return (float)round ((double)x); }
float sqrtf     (float x)          { return (float)sqrt ((double)x); }
float fmodf     (float x, float y) { return (float)fmod ((double)x, (double)y); }
float copysignf (float x, float y) { return (float)copysign ((double)x, (double)y); }
