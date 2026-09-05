#ifndef NexOS_SYSCALL_H
#define NexOS_SYSCALL_H

#include <stdint.h>
#include "setjmp.h"

// Registers as pushed by sys_enter / linux_syscall_entry (asm).
// Push order is eax, ebx, ecx, edx, esi, edi, ebp so that eax lands at
// offset 0 (lowest address) — matching this struct field order.
struct SysRegs { uint32_t eax, ebx, ecx, edx, esi, edi, ebp; };

// int 0x80 trap entry (asm, defined in syscall.cpp).
extern "C" void sys_enter(void);

// C-side dispatcher. Ring-0 calls forward to linux_syscall_dispatch;
// ring-3 user processes are handled here.
extern "C" void sys_dispatch(SysRegs* r);

// Switch the CPU from ring 0 to ring 3: build an iret frame and enter
// `entry` with `stack_top`. Returns (via mini_longjmp on sys_exit) when the
// user process exits. Caller must set g_current to the user Process first.
extern "C" int  enter_user(uint32_t entry, uint32_t stack_top);

// Linux i386 syscall numbers (subset we implement).
#define SYS_EXIT   1
#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_GETPID 20
#define SYS_GETUID 24
#define SYS_BRK    45

#endif // NexOS_SYSCALL_H
