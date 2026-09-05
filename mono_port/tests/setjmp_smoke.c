/* tests/setjmp_smoke.c -- verify pal/setjmp_i386.c.
 *
 * mini/interp 把托管的 try/catch 直接建在 setjmp/longjmp 上：每进一个
 * 受保护块就 setjmp 一次，抛异常时 longjmp 回去。这里如果错了，症状
 * 不是"崩溃"，而是"catch 之后某个局部变量的值不对"——那种 bug 要查一周。
 *
 * 所以这个测试专门盯三件事：
 *   1. 返回值语义：直接调用返回 0；longjmp(env,v) 返回 v；v==0 时返回 1。
 *   2. callee-saved 寄存器（ebx/esi/edi/ebp）在 longjmp 之后必须原样恢复。
 *   3. 深栈 longjmp 之后 esp 要真的回退（否则循环几千次就把栈耗光）。
 *
 * 用 -O2 编译。-O0 下编译器不做寄存器分配，测不出 returns_twice 的问题。
 */

#include <setjmp.h>

/* eax 必须声明成输出：int 0x80 用返回值覆盖 eax。少写这一笔，-O2 下
 * 编译器会认为 eax 仍是 4 而跳过重新装载系统调用号，于是第三次写变成
 * 一个号码错误的 syscall —— 表现就是"某几行日志莫名其妙不见了"。 */
static void sys_write (const char *s, unsigned len)
{
	int r;
	__asm__ __volatile__ ("int $0x80"
	                      : "=a"(r)
	                      : "0"(4), "b"(1), "c"(s), "d"(len)
	                      : "memory");
	(void)r;
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

static jmp_buf g_env;

/* ---- 1. 基本返回值语义 ---------------------------------------------- */
static void throw_with (int v) { longjmp (g_env, v); }

void regs_probe_body (void)
{
	__asm__ __volatile__ ("movl $0, %%ebx\n\t"
	                      "movl $0, %%esi\n\t"
	                      "movl $0, %%edi"
	                      ::: "ebx", "esi", "edi");
	longjmp (g_env, 7);
}

/* ---- 2. 深栈：递归到 200 层再跳回来 ---------------------------------- */
static int deep (int n)
{
	volatile int pad[8];        /* 让每层实实在在吃掉栈 */
	pad[0] = n;
	if (n == 0)
		longjmp (g_env, 99);
	return deep (n - 1) + pad[0];
}

/* ---- 纯汇编的 callee-saved 探针 -------------------------------------
 * int jmp_regs_probe (jmp_buf env);
 * 语义和 setjmp 一样"返回两次"，所以必须标 returns_twice，否则编译器
 * 会把调用点之后的代码当成只执行一次来优化。
 */
unsigned long g_seen_ebx, g_seen_esi, g_seen_edi;
extern int jmp_regs_probe (jmp_buf env);

/* 探针的"抛出"回调：踩烂三个 callee-saved 寄存器再跳回去。
 * 关键是它在 jmp_regs_probe 的栈帧之内被调用 —— longjmp 只能跳回
 * 一个还活着的栈帧，跳回已经 return 的函数是未定义行为（实测直接段错误）。 */
void regs_probe_body (void);

__asm__ (
	".text\n"
	".globl jmp_regs_probe\n"
	"jmp_regs_probe:\n"
	"	pushl %ebx\n"
	"	pushl %esi\n"
	"	pushl %edi\n"
	"	pushl %ebp\n"
	"	movl 20(%esp), %eax\n"        /* env：4 个 push + 返回地址 */
	"	movl $0xB1B1B1B1, %ebx\n"
	"	movl $0x51515151, %esi\n"
	"	movl $0xD1D1D1D1, %edi\n"
	"	pushl %eax\n"
	"	call setjmp\n"                /* 第一次落 0；被 longjmp 弹回来落非 0 */
	"	addl $4, %esp\n"
	"	movl %ebx, g_seen_ebx\n"      /* 两次都抄一遍，第二次才是我们要的 */
	"	movl %esi, g_seen_esi\n"
	"	movl %edi, g_seen_edi\n"
	"	testl %eax, %eax\n"
	"	jnz 2f\n"
	"	call regs_probe_body\n"       /* 不返回：内部 longjmp 回到上面 */
	"2:\n"
	"	popl %ebp\n"
	"	popl %edi\n"
	"	popl %esi\n"
	"	popl %ebx\n"
	"	ret\n"                        /* setjmp 的返回值原样留在 eax */
);

/* ---- 3. 读当前 esp，用来验证栈真的回退了 ----------------------------- */
static unsigned long cur_esp (void)
{
	unsigned long sp;
	__asm__ __volatile__ ("movl %%esp, %0" : "=r"(sp));
	return sp;
}

int _start_c (void)
{
	int r, i;
	unsigned long sp_before, sp_after;

	out ("== setjmp/longjmp smoke ==\n");

	/* 1a. 首次调用返回 0 */
	r = setjmp (g_env);
	if (r == 0) {
		check ("setjmp first call == 0", 1);
		throw_with (42);
		check ("longjmp did not return", 0);   /* 到不了 */
	} else {
		check ("longjmp(env,42) -> 42", r == 42);
	}

	/* 1b. longjmp(env, 0) 必须被改写成 1 —— C 标准明文规定 */
	r = setjmp (g_env);
	if (r == 0)
		throw_with (0);
	else
		check ("longjmp(env,0) -> 1", r == 1);

	/* 2. callee-saved 寄存器必须原样恢复。
	 *    这一条不能用 C 的 register 变量来测：`register x __asm__("ebx")`
	 *    只保证在 inline asm 那一瞬间值在 ebx 里，编译器随时可以把它挪走，
	 *    结果测的是寄存器分配器而不是 longjmp。所以用一段纯汇编探针
	 *    jmp_regs_probe()：它自己把哨兵值塞进 ebx/esi/edi，紧接着 call
	 *    setjmp，再把 setjmp 返回后看到的三个寄存器抄进全局变量。
	 *    第一次（返回 0）抄到的是哨兵，第二次（被 longjmp 弹回来）抄到的
	 *    就是 longjmp 恢复出来的值 —— 两次必须一致。 */
	{
		r = jmp_regs_probe (g_env);
		check ("longjmp restores ebx", g_seen_ebx == 0xB1B1B1B1UL);
		check ("longjmp restores esi", g_seen_esi == 0x51515151UL);
		check ("longjmp restores edi", g_seen_edi == 0xD1D1D1D1UL);
		check ("value passed through",  r == 7);
	}

	/* 3. 深栈跳回：esp 必须回到 setjmp 那一刻的位置 */
	sp_before = cur_esp ();
	r = setjmp (g_env);
	if (r == 0) {
		deep (200);
		check ("deep longjmp taken", 0);
	}
	sp_after = cur_esp ();
	check ("deep longjmp value == 99", r == 99);
	check ("esp restored after deep jmp", sp_before == sp_after);

	/* 4. 反复跳 5000 次：如果 esp 每次少回退一点，这里会栈溢出/跑飞 */
	{
		int count = 0;
		for (i = 0; i < 5000; i++) {
			if (setjmp (g_env) == 0)
				deep (20);
			count++;
		}
		check ("5000x setjmp/longjmp stable", count == 5000);
		check ("esp still sane after 5000x", cur_esp () == sp_after);
	}

	/* 5. sigsetjmp/siglongjmp 走同一套代码，顺带确认别名符号真的存在 */
	{
		static sigjmp_buf senv;
		r = sigsetjmp (senv, 1);
		if (r == 0)
			siglongjmp (senv, 123);
		check ("sigsetjmp/siglongjmp", r == 123);
	}

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
