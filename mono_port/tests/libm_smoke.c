/* tests/libm_smoke.c -- verify the x87 libm in pal/libm_impl.c.
 *
 * Why this test exists: every function in libm_impl.c is hand-written x87
 * inline asm, and the two ways to get that wrong are silent.  Either the
 * rounding-mode dance in floor/ceil/trunc is mis-patched (so floor() quietly
 * becomes round-to-nearest), or an asm block forgets to pop its "u" operand
 * and the 8-deep x87 stack overflows after a few hundred calls -- at which
 * point results turn into NaN far away from the real bug.  The loop at the
 * end deliberately hammers the pop-sensitive functions to catch that.
 *
 * Built and run the same way as eglib_smoke: 32-bit, -nostdlib, int 0x80.
 */

#include <math.h>

/* eax 必须声明成输出：int 0x80 会把返回值写回 eax。只写 "a"(4) 输入的话，
 * 编译器以为 eax 还是 4，第二次调用就不重新加载系统调用号了（-O2 下必现）。 */
static void sys_write (const char *s, unsigned len)
{
	int r;
	__asm__ __volatile__ ("int $0x80"
	                      : "=a"(r)
	                      : "0"(4), "b"(1), "c"(s), "d"(len)
	                      : "memory");
	(void)r;
}

static void sys_exit (int code)
{
	__asm__ __volatile__ ("int $0x80" :: "a"(1), "b"(code));
	for (;;) { }
}

static unsigned slen (const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void out (const char *s) { sys_write (s, slen (s)); }

static int g_fail;

static void check (const char *name, int ok)
{
	out (ok ? "  [ok]   " : "  [FAIL] ");
	out (name);
	out ("\n");
	if (!ok) g_fail++;
}

/* Tolerance-based compare; the x87 80-bit path is not bit-exact with glibc. */
static int near_ (double a, double b, double eps)
{
	double d = a - b;
	if (d < 0) d = -d;
	return d <= eps;
}

int _start_c (void)
{
	int i;
	double acc;

	out ("=== pal libm smoke (x87) ===\n");

	/* --- rounding: the mode-patching is the fragile part --- */
	check ("floor(2.7)==2",    floor (2.7) == 2.0);
	check ("floor(-2.1)==-3",  floor (-2.1) == -3.0);
	check ("ceil(2.1)==3",     ceil (2.1) == 3.0);
	check ("ceil(-2.7)==-2",   ceil (-2.7) == -2.0);
	check ("trunc(2.7)==2",    trunc (2.7) == 2.0);
	check ("trunc(-2.7)==-2",  trunc (-2.7) == -2.0);
	/* round() must go away from zero at .5, unlike x87's banker's default */
	check ("round(2.5)==3",    round (2.5) == 3.0);
	check ("round(-2.5)==-3",  round (-2.5) == -3.0);
	check ("round(0.5)==1",    round (0.5) == 1.0);
	check ("round(1.5)==2",    round (1.5) == 2.0);

	/* --- basic algebra --- */
	check ("sqrt(144)==12",    sqrt (144.0) == 12.0);
	check ("fmod(10,3)==1",    near_ (fmod (10.0, 3.0), 1.0, 1e-12));
	check ("fmod(-10,3)==-1",  near_ (fmod (-10.0, 3.0), -1.0, 1e-12));
	check ("copysign",         copysign (3.0, -1.0) == -3.0);
	check ("ldexp(3,4)==48",   ldexp (3.0, 4) == 48.0);

	{
		int e = 0;
		double m = frexp (48.0, &e);
		check ("frexp mantissa",   near_ (m, 0.75, 1e-15));
		check ("frexp exponent",   e == 6);
	}
	{
		double ip = 0.0;
		double fp = modf (3.25, &ip);
		check ("modf int part",    ip == 3.0);
		check ("modf frac part",   near_ (fp, 0.25, 1e-15));
	}

	/* --- transcendentals --- */
	check ("log(e)==1",        near_ (log (2.718281828459045), 1.0, 1e-12));
	check ("log2(1024)==10",   near_ (log2 (1024.0), 10.0, 1e-12));
	check ("log10(1000)==3",   near_ (log10 (1000.0), 3.0, 1e-12));
	check ("exp(0)==1",        near_ (exp (0.0), 1.0, 1e-15));
	check ("exp(1)==e",        near_ (exp (1.0), 2.718281828459045, 1e-12));
	check ("pow(2,10)==1024",  near_ (pow (2.0, 10.0), 1024.0, 1e-9));
	check ("pow(2,0.5)",       near_ (pow (2.0, 0.5), 1.4142135623730951, 1e-12));
	check ("pow(-2,3)==-8",    near_ (pow (-2.0, 3.0), -8.0, 1e-9));
	check ("pow(x,0)==1",      pow (123.0, 0.0) == 1.0);
	check ("cbrt(27)==3",      near_ (cbrt (27.0), 3.0, 1e-9));

	check ("sin(0)==0",        near_ (sin (0.0), 0.0, 1e-15));
	check ("cos(0)==1",        near_ (cos (0.0), 1.0, 1e-15));
	check ("sin(pi/6)==0.5",   near_ (sin (0.5235987755982988), 0.5, 1e-12));
	check ("tan(pi/4)==1",     near_ (tan (0.7853981633974483), 1.0, 1e-12));
	check ("atan(1)==pi/4",    near_ (atan (1.0), 0.7853981633974483, 1e-12));
	/* atan2 argument order is the classic place to get x/y backwards */
	check ("atan2(1,1)",       near_ (atan2 (1.0, 1.0), 0.7853981633974483, 1e-12));
	check ("atan2(1,0)",       near_ (atan2 (1.0, 0.0), 1.5707963267948966, 1e-12));
	check ("atan2(0,1)==0",    near_ (atan2 (0.0, 1.0), 0.0, 1e-15));
	check ("asin(1)==pi/2",    near_ (asin (1.0), 1.5707963267948966, 1e-9));
	check ("acos(1)==0",       near_ (acos (1.0), 0.0, 1e-9));
	check ("tanh(0)==0",       near_ (tanh (0.0), 0.0, 1e-15));
	check ("cosh(0)==1",       near_ (cosh (0.0), 1.0, 1e-12));

	/* --- classification macros --- */
	check ("isnan(0/0)",       isnan (0.0 / 0.0) != 0);
	check ("isinf(1/0)",       isinf (1.0 / 0.0) != 0);
	check ("isfinite(1)",      isfinite (1.0) != 0);
	check ("isunordered",      isunordered (0.0 / 0.0, 1.0) != 0);
	check ("!isunordered",     isunordered (1.0, 2.0) == 0);

	/* --- x87 stack-leak hunt ---
	 * 2000 iterations is far more than the 8 stack slots, so a missing pop
	 * turns the accumulator into NaN long before we get here. */
	acc = 0.0;
	for (i = 1; i <= 2000; i++) {
		acc += fmod ((double)i, 7.0);
		acc += ldexp (1.0, i % 5);
		acc += atan2 (1.0, (double)i);
		acc += pow (1.000001, (double)(i % 10));
	}
	check ("no x87 stack leak", isfinite (acc) && !isnan (acc));

	/* ---- C99 additions pulled in by metadata/sysmath.c ----------------
	 * System.Math.Asinh/Acosh/Atanh/ScaleB map straight onto these, so a
	 * wrong identity here shows up as wrong managed arithmetic, not a crash.
	 */
	check ("asinh(0)==0",        asinh (0.0) == 0.0);
	check ("asinh(1)",           near_ (asinh (1.0), 0.881373587019543, 1e-12));
	check ("asinh(-1) odd",      near_ (asinh (-1.0), -0.881373587019543, 1e-12));
	check ("asinh tiny (log1p)", near_ (asinh (1e-9), 1e-9, 1e-20));
	check ("asinh(1e10) big",    near_ (asinh (1e10), 23.7189981105004, 1e-9));
	check ("acosh(1)==0",        acosh (1.0) == 0.0);
	check ("acosh(2)",           near_ (acosh (2.0), 1.3169578969248166, 1e-12));
	check ("acosh(0.5)==NaN",    isnan (acosh (0.5)));
	check ("atanh(0)==0",        atanh (0.0) == 0.0);
	check ("atanh(0.5)",         near_ (atanh (0.5), 0.5493061443340549, 1e-12));
	check ("atanh(-0.5) odd",    near_ (atanh (-0.5), -0.5493061443340549, 1e-12));
	check ("tanh(atanh(x))",     near_ (tanh (atanh (0.3)), 0.3, 1e-12));
	check ("log1p tiny",         near_ (log1p (1e-15), 1e-15, 1e-27));
	check ("log1p(1)==ln2",      near_ (log1p (1.0), 0.6931471805599453, 1e-12));
	check ("expm1 tiny",         near_ (expm1 (1e-15), 1e-15, 1e-27));
	check ("expm1(1)",           near_ (expm1 (1.0), 1.718281828459045, 1e-12));
	check ("hypot(3,4)==5",      near_ (hypot (3.0, 4.0), 5.0, 1e-12));
	check ("hypot no overflow",  near_ (hypot (3e200, 4e200), 5e200, 1e188));
	check ("hypot(0,0)==0",      hypot (0.0, 0.0) == 0.0);
	check ("fma(2,3,4)==10",     fma (2.0, 3.0, 4.0) == 10.0);
	check ("fma neg",            fma (-2.0, 3.0, 1.0) == -5.0);
	check ("scalbn(1,10)",       scalbn (1.0, 10) == 1024.0);
	check ("scalbn(3,-2)",       scalbn (3.0, -2) == 0.75);
	check ("ilogb(1)==0",        ilogb (1.0) == 0);
	check ("ilogb(1024)==10",    ilogb (1024.0) == 10);
	check ("ilogb(0.5)==-1",     ilogb (0.5) == -1);
	check ("logb(8)==3",         logb (8.0) == 3.0);
	check ("rint(2.5)==2",       rint (2.5) == 2.0);   /* 最近偶 */
	check ("rint(3.5)==4",       rint (3.5) == 4.0);
	check ("rint(-2.5)==-2",     rint (-2.5) == -2.0);
	check ("remainder(5,3)==-1", remainder (5.0, 3.0) == -1.0);
	check ("remainder(4,3)==1",  remainder (4.0, 3.0) == 1.0);
	check ("nextafter up",       nextafter (1.0, 2.0) > 1.0);
	check ("nextafter tight",    nextafter (1.0, 2.0) < 1.0000000001);
	check ("nextafter down",     nextafter (1.0, 0.0) < 1.0);
	check ("nextafter x==y",     nextafter (1.0, 1.0) == 1.0);

	/* 第二轮栈泄漏猎杀：这些新函数内部也有 fscale/fprem1/fxtract */
	acc = 0.0;
	for (i = 1; i <= 2000; i++) {
		acc += asinh ((double)(i % 97) * 0.01);
		acc += hypot ((double)i, 1.0);
		acc += remainder ((double)i, 7.0);
		acc += (double)ilogb ((double)i);
		acc += scalbn (1.0, (i % 7) - 3);
	}
	check ("no x87 stack leak (C99)", isfinite (acc) && !isnan (acc));

	if (g_fail == 0) {
		out ("=== ALL PASS ===\n");
		return 0;
	}
	out ("=== FAILURES PRESENT ===\n");
	return 1;
}

__asm__ (
	".globl _start\n"
	"_start:\n"
	"	call _start_c\n"
	"	mov  %eax, %ebx\n"
	"	mov  $1, %eax\n"
	"	int  $0x80\n"
);
