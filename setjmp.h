#ifndef NexOS_SETJMP_H
#define NexOS_SETJMP_H

#include <stdint.h>

// Minimal freestanding setjmp/longjmp context (i386).
// Layout MUST match the asm in linux_compat.cpp / syscall.cpp.
struct Ctx { uint32_t ebp, ebx, esi, edi, esp, eip; };

extern "C" int  mini_setjmp(Ctx*);
extern "C" void mini_longjmp(Ctx*, int);

#endif // NexOS_SETJMP_H
