/* pal/setjmp_i386.c -- 真正的 i386 setjmp/longjmp。
 *
 * 为什么用整块裸汇编而不是 inline asm：setjmp 的语义是"这个调用会返回两次"，
 * 任何被编译器插进来的 prologue/epilogue（尤其是 -O0 下的 frame pointer
 * 处理）都会在第二次返回时对不上号。写成独立的汇编符号是唯一稳的做法。
 *
 * SysV i386 调用约定里 callee-saved 的是 ebx/esi/edi/ebp（还有 esp）。
 * 其余寄存器（eax/ecx/edx）本来就允许被调用方破坏，不需要保存 ——
 * 这也是 longjmp 之后 setjmp 返回值能放进 eax 的原因。
 */

__asm__ (
	".text\n"
	".globl setjmp\n"
	".globl _setjmp\n"
	".globl sigsetjmp\n"
	".type  setjmp, @function\n"
	"setjmp:\n"
	"_setjmp:\n"
	"sigsetjmp:\n"                    /* savemask 参数直接忽略 */
	"	movl 4(%esp), %eax\n"         /* eax = env */
	"	movl %ebx,  0(%eax)\n"
	"	movl %esi,  4(%eax)\n"
	"	movl %edi,  8(%eax)\n"
	"	movl %ebp, 12(%eax)\n"
	"	leal 4(%esp), %ecx\n"         /* 记录 ret 之后的 esp */
	"	movl %ecx, 16(%eax)\n"
	"	movl (%esp), %ecx\n"          /* 返回地址 */
	"	movl %ecx, 20(%eax)\n"
	"	movl $0, 24(%eax)\n"          /* 预留槽清零 */
	"	movl $0, 28(%eax)\n"
	"	xorl %eax, %eax\n"            /* 直接调用时返回 0 */
	"	ret\n"
	".size setjmp, .-setjmp\n"
);

__asm__ (
	".text\n"
	".globl longjmp\n"
	".globl _longjmp\n"
	".globl siglongjmp\n"
	".type  longjmp, @function\n"
	"longjmp:\n"
	"_longjmp:\n"
	"siglongjmp:\n"
	"	movl 4(%esp), %edx\n"         /* edx = env */
	"	movl 8(%esp), %eax\n"         /* eax = val */
	"	testl %eax, %eax\n"
	"	jnz  1f\n"
	"	incl %eax\n"                  /* longjmp(env,0) 必须让 setjmp 返回 1 */
	"1:\n"
	"	movl  0(%edx), %ebx\n"
	"	movl  4(%edx), %esi\n"
	"	movl  8(%edx), %edi\n"
	"	movl 12(%edx), %ebp\n"
	"	movl 16(%edx), %esp\n"        /* 换栈 —— 之后不能再碰原栈上的东西 */
	"	jmp  *20(%edx)\n"
	".size longjmp, .-longjmp\n"
);
