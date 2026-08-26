// =====================================================================
//  syscall.cpp  -  Unified int 0x80 syscall ABI (Foundation 0)
// ---------------------------------------------------------------------
//  One trap vector serves both worlds:
//    * ring-0 callers (the Wine/linux_compat shim) forward to
//      linux_syscall_dispatch() so Milestone 0 keeps working;
//    * ring-3 user processes are handled here with a real syscall table,
//      and sys_exit unwinds back into enter_user() via mini_longjmp.
// =====================================================================

#include "syscall.h"
#include "proc.h"
#include "vfs.h"
#include "mm.h"
#include <stdint.h>

// ---- serial ----
static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static void serial_puts(const char* s){ while(*s) outb(0x3F8,(uint8_t)*s++); }
static void serial_putdec(int v){
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { int t = v; if (v < 0){ b[i++]='-'; t=-t; } while(t){ b[i++]=(char)('0'+t%10); t/=10; } }
    for (int j = i-1; j >= 0; j--) outb(0x3F8,(uint8_t)b[j]);
}
static void serial_hex(uint32_t v){
    const char* H="0123456789ABCDEF"; char b[9];
    for (int i=0;i<8;i++) b[i]=H[(v>>(28-i*4))&0xF]; b[8]=0; serial_puts(b);
}

// Ring-0 path lives in linux_compat.cpp.
extern "C" void linux_syscall_dispatch(SysRegs* r);
extern "C" void linux_deliver_signals(SysRegs* r, int from_trap);

// ---------------------------------------------------------------------
//  User pointer validation
// ---------------------------------------------------------------------
//  A ring-3 process passes raw pointers in registers.  The user region is
//  identity-mapped so the kernel can dereference them directly, but only
//  after confirming they actually live inside that region - otherwise a
//  malicious guest could hand us a kernel address and have the kernel
//  read or scribble on itself for it (confused deputy).
// ---------------------------------------------------------------------
static int user_range_ok(uint32_t p, uint32_t len){
    if (p < USER_BASE || p >= USER_END) return 0;
    if (len > USER_END - p) return 0;
    return 1;
}
static int user_str_ok(uint32_t p){
    if (p < USER_BASE || p >= USER_END) return 0;
    const char* s = (const char*)p;
    for (uint32_t i = 0; i < 512; i++){
        if ((p + i) >= USER_END) return 0;
        if (!s[i]) return 1;
    }
    return 0;                       // unterminated / absurdly long
}

// Context to unwind into enter_user() when a ring-3 process exits.
static Ctx g_user_ctx;

// ---------------------------------------------------------------------
//  enter_user: switch ring 0 -> ring 3
// ---------------------------------------------------------------------
extern "C" int enter_user(uint32_t entry, uint32_t stack_top){
    if (g_current) g_current->ring = 3;

    if (mini_setjmp(&g_user_ctx) == 0){
        // Build an iret frame on the current (kernel) stack and return to
        // ring 3. eflags has IF=0 (PIC is masked; no ring-3 IRQs yet).
        __asm__ __volatile__(
            "pushl $0x23\n"          // ss   = udata (RPL 3)
            "pushl %[sp]\n"          // esp  = user stack top
            "pushl $0x2\n"           // eflags (reserved bit set, IF=0)
            "pushl $0x1B\n"          // cs   = ucode (RPL 3)
            "pushl %[ep]\n"          // eip  = entry
            "iret\n"
            :: [sp]"r"(stack_top), [ep]"r"(entry) : "memory");
        __builtin_unreachable();
    }

    // Returned here via mini_longjmp (sys_exit). Back in ring 0.
    if (g_current) g_current->ring = 0;
    return 0;
}

// ---------------------------------------------------------------------
//  sys_dispatch: C-side handler
// ---------------------------------------------------------------------
extern "C" void sys_dispatch(SysRegs* r){
    // ----- ring-3 user process -----
    if (g_current && g_current->ring == 3){
        switch (r->eax){
            case SYS_EXIT: {
                serial_puts("[usys] exit pid=");
                serial_putdec((int)g_current->pid);
                serial_puts(" uid=");
                serial_putdec((int)g_current->uid);
                serial_puts("\n");
                // Unwind straight back into enter_user() (the setjmp return).
                mini_longjmp(&g_user_ctx, 1);
                __builtin_unreachable();
            }
            case SYS_WRITE: {
                int fd = (int)r->ebx;
                const char* buf = (const char*)r->ecx;
                uint32_t count = r->edx;
                if (!user_range_ok(r->ecx, count)){ r->eax = (uint32_t)-1; return; }
                if (fd == 1 || fd == 2){
                    for (uint32_t i = 0; i < count; i++) outb(0x3F8,(uint8_t)buf[i]);
                    r->eax = count;
                } else {
                    r->eax = (uint32_t)vfs_write(fd, buf, (int)count);
                }
                return;
            }
            case SYS_GETPID:
                r->eax = g_current->pid;
                return;
            case SYS_GETUID:
                r->eax = g_current->uid;
                return;
            case SYS_OPEN: {
                if (!user_str_ok(r->ebx)){
                    serial_puts("[usys] open: bad user pointer\n");
                    r->eax = (uint32_t)-1;   // -EFAULT
                    return;
                }
                r->eax = (uint32_t)vfs_open((const char*)r->ebx, (int)r->ecx);
                return;
            }
            case SYS_READ: {
                int fd = (int)r->ebx;
                if (fd == 0){ r->eax = 0; return; }          // stdin: EOF
                if (!user_range_ok(r->ecx, r->edx)){ r->eax = (uint32_t)-1; return; }
                r->eax = (uint32_t)vfs_read(fd, (void*)r->ecx, (int)r->edx);
                return;
            }
            case SYS_CLOSE:
                r->eax = (uint32_t)vfs_close((int)r->ebx);
                return;
            case SYS_BRK:
                r->eax = g_current->brk;  // benign: return current break
                return;
            default:
                serial_puts("[usys] unknown num=");
                serial_putdec((int)r->eax);
                serial_puts("\n");
                r->eax = (uint32_t)-1;    // -ENOSYS
                return;
        }
    }

    // ----- ring-0 path (Wine / linux_compat) -----
    linux_syscall_dispatch(r);
}

// ---------------------------------------------------------------------
//  sys_enter: int 0x80 trap entry (asm)
//  Push order matches struct SysRegs: ebp first ... eax LAST, so eax
//  lands at the lowest address (offset 0). Then a SysRegs* is pushed
//  as the cdecl argument to sys_dispatch.
//
//  IMPORTANT (fixed): the epilogue POPS the register block back into the
//  CPU registers instead of discarding it with `addl $32,%esp`.  The old
//  code never wrote the syscall result back, so the guest saw the kernel
//  ESP (left in eax by `movl %esp,%eax`) as the "return value".  That went
//  unnoticed while the only guests were write()+exit() demos that ignore
//  the result; the VFS needs real fds and negative errnos.
//
//  Stack at the `iret` must point exactly at the CPU-pushed frame:
//     entry esp0-20 : EIP CS EFLAGS ESP SS      (ring change -> 5 dwords)
//     after 7 pushes: esp0-48
//     after arg push: esp0-52
//     addl $4        : esp0-48  (saved EAX)
//     7 pops         : esp0-20  -> the iret frame.  Balanced.
// ---------------------------------------------------------------------
__asm__(
    ".global sys_enter\n"
    "sys_enter:\n"
    "    pushl %ebp\n"
    "    pushl %edi\n"
    "    pushl %esi\n"
    "    pushl %edx\n"
    "    pushl %ecx\n"
    "    pushl %ebx\n"
    "    pushl %eax\n"
    "    movl %esp, %eax\n"
    "    pushl %eax\n"           // SysRegs* (first cdecl arg)
    "    call sys_dispatch\n"
    "    addl $4, %esp\n"        // drop the SysRegs* argument
    // Stage 2: deliver any pending & unblocked signal for the current thread
    // BEFORE returning to the guest.  linux_deliver_signals() may rewrite the
    // trap frame and switch into a signal handler (it does not return); if
    // there is nothing to deliver it returns here and we pop+iret as normal.
    // At this point %esp points at the saved eax slot == SysRegs* (r).
    "    movl %esp, %eax\n"
    "    pushl $1\n"             // from_trap = 1 (int 0x80 path: real user ESP)
    "    pushl %eax\n"           // SysRegs* (r)
    "    call linux_deliver_signals\n"
    "    addl $8, %esp\n"
    "    popl %eax\n"            // <- syscall return value reaches the guest
    "    popl %ebx\n"
    "    popl %ecx\n"
    "    popl %edx\n"
    "    popl %esi\n"
    "    popl %edi\n"
    "    popl %ebp\n"
    "    iret\n"
);
