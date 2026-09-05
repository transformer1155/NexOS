/* PAL <setjmp.h> — MiniOS Phase 0 (i386)
 *
 * mini/interp 用 setjmp/longjmp 实现托管异常的展开（interp.c 里每个
 * try 块一个 jmp_buf）。这不是可以糊弄的桩：longjmp 走错一个寄存器，
 * 症状是"catch 之后局部变量莫名其妙变了值"，比崩溃难查得多。
 * 所以这里存的是真正的 callee-saved 集合：ebx/esi/edi/ebp/esp/eip。
 *
 * 布局（每格 4 字节）：
 *   [0]=ebx [1]=esi [2]=edi [3]=ebp [4]=esp(返回后的) [5]=返回地址
 *   [6..7] 预留（保持 32 字节对齐友好，也给以后存信号掩码留位置）
 */
#ifndef PAL_SETJMP_H
#define PAL_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

#define _JBLEN 8
typedef unsigned long jmp_buf[_JBLEN];
typedef unsigned long sigjmp_buf[_JBLEN];

/* returns_twice 必须加：否则 gcc 会把 setjmp 之后被修改的局部变量
 * 缓存在寄存器里，longjmp 回来后读到的是过期值。 */
int  setjmp  (jmp_buf env) __attribute__((returns_twice));
int  _setjmp (jmp_buf env) __attribute__((returns_twice));
void longjmp (jmp_buf env, int val) __attribute__((noreturn));
void _longjmp (jmp_buf env, int val) __attribute__((noreturn));

/* MiniOS 没有信号掩码，savemask 被忽略；保留签名以便 Mono 直接编过。 */
int  sigsetjmp  (sigjmp_buf env, int savemask) __attribute__((returns_twice));
void siglongjmp (sigjmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
#endif /* PAL_SETJMP_H */
