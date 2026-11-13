// =====================================================================
//  nexos_compat_asm.cpp -- 64-bit context primitives for the AI-assisted
//                          compatibility layer.
//
//  WHY THIS FILE EXISTS
//    The repository only had a 32-bit setjmp/longjmp
//    (linux_compat.cpp: mini_setjmp/mini_longjmp, i386 register layout).
//    The 64-bit kernel had NO unwind primitive at all.  Two things need
//    one:
//
//      1. D4 -- fault_common() must be able to abandon a PE that just
//         took a #PF and return to the kernel with its state intact.
//      2. D0 -- the PE must run on its own stack and yield back to the
//         frame loop at API-call boundaries, so the AI can observe it
//         while it runs.
//
//    Both are the same primitive: save the callee-saved registers plus
//    rsp/rip, and later restore them and jump.
//
//  ABI NOTES
//    * Naked functions: gcc emits nothing but the asm block, so the
//      register layout below is exactly what the CPU sees.
//    * SysV AMD64 argument registers: rdi = arg0, rsi = arg1, rdx = arg2.
//      We pin them with the "D"/"S"/"d" constraints and address the
//      context through rdi, so no register shuffling is needed.
//    * The kernel is built with -mno-red-zone.
//    * compat_setjmp() stores rsp pointing AT the return-address slot and
//      stores that return address as the resume rip.  compat_longjmp()
//      writes the saved rip back into (%rsp) and executes `ret`, so it
//      lands exactly where setjmp() returned, with the value in eax.
//    * compat_fiber_switch() never returns in the forward direction: it
//      installs the new rsp and jumps.  The peer switches back by calling
//      it again with the saved context.
//
//  CONTEXT LAYOUT (must match CompatCtx in nexos_compat.h)
//      0 rbx    8 rbp    16 r12   24 r13   32 r14   40 r15   48 rsp   56 rip
// =====================================================================
#include <stdint.h>

extern "C" {

// ---- setjmp ----------------------------------------------------------
// Returns 0 when called normally, 1 when resumed via compat_longjmp().
__attribute__((naked)) int compat_setjmp(uint64_t* c){
    __asm__ __volatile__(
        "movq %%rbx, 0(%%rdi)\n"
        "movq %%rbp, 8(%%rdi)\n"
        "movq %%r12, 16(%%rdi)\n"
        "movq %%r13, 24(%%rdi)\n"
        "movq %%r14, 32(%%rdi)\n"
        "movq %%r15, 40(%%rdi)\n"
        "movq %%rsp, 48(%%rdi)\n"
        "movq (%%rsp), %%rax\n"       // our caller's return address
        "movq %%rax, 56(%%rdi)\n"
        "xorl %%eax, %%eax\n"         // first call -> 0
        "ret\n"
        :: "D"(c)
        : "rax", "memory");
    __builtin_unreachable();
}

// ---- longjmp ---------------------------------------------------------
// Never returns.  Restores the callee-saved registers, points rsp at the
// saved return-address slot, writes the saved rip there, and returns.
__attribute__((naked)) void compat_longjmp(uint64_t* c, int val){
    __asm__ __volatile__(
        "movq 0(%%rdi), %%rbx\n"
        "movq 8(%%rdi), %%rbp\n"
        "movq 16(%%rdi), %%r12\n"
        "movq 24(%%rdi), %%r13\n"
        "movq 32(%%rdi), %%r14\n"
        "movq 40(%%rdi), %%r15\n"
        "movq 56(%%rdi), %%rax\n"
        "movq 48(%%rdi), %%rsp\n"
        "movq %%rax, (%%rsp)\n"       // place the resume address
        "movl $1, %%eax\n"            // setjmp() will observe 1
        "ret\n"
        :: "D"(c), "S"((uint64_t)val)
        : "rax", "memory");
    __builtin_unreachable();
}

// ---- fiber switch ----------------------------------------------------
//   rdi = save : context to fill with the CURRENT state (may be null)
//   rsi = rsp  : stack pointer to switch to
//   rdx = rip  : address to jump to
//
// Forward direction never returns.  Coming back is done by the peer
// calling this again with the saved context.
__attribute__((naked)) void compat_fiber_switch(uint64_t* save,
                                                uint64_t rsp,
                                                uint64_t rip){
    __asm__ __volatile__(
        "testq %[save], %[save]\n"
        "jz 2f\n"
        "movq %%rbx, 0(%[save])\n"
        "movq %%rbp, 8(%[save])\n"
        "movq %%r12, 16(%[save])\n"
        "movq %%r13, 24(%[save])\n"
        "movq %%r14, 32(%[save])\n"
        "movq %%r15, 40(%[save])\n"
        "movq %%rsp, 48(%[save])\n"
        "movq (%%rsp), %%rax\n"
        "movq %%rax, 56(%[save])\n"
        "2:\n"
        "movq %[rsp], %%rsp\n"
        "movq %[rip], %%rax\n"
        "jmp *%%rax\n"
        : : [save]"r"(save), [rsp]"r"(rsp), [rip]"r"(rip)
        : "rax", "memory");
    __builtin_unreachable();
}

} // extern "C"
