// linux_compat.cpp — Milestone 0 of Wine-on-NexOS.
//
// A minimal Linux binary-compatibility shim:
//   * int 0x80 syscall dispatcher (Linux i386 ABI: eax=sysno, ebx..ebp=args)
//   * flat ELF32 loader (PT_LOAD only, identity-mapped address space)
//   * runs the guest in RING 0 (de-risked; ring-3 isolation is a later milestone)
//
// This is the host ABI Wine's Linux backend requires. Once enough syscalls
// exist and ring-3 + per-process paging land, a cross-built Wine can sit on
// top and, in turn, run Win32 binaries (including, long-term, Windows Chrome).

#include "linux_compat.h"
#include "setjmp.h"      // shared Ctx + mini_setjmp/mini_longjmp
#include "syscall.h"     // shared SysRegs
#include "gdt.h"         // gdt_set_tls (set_thread_area)
#include <stdint.h>
#include "kernel/vmm.h"  // vmm_map_page (real 4 KiB page mapping; auto-splits PSE)
#include "kernel/pmm.h"  // pmm_alloc_page / pmm_free_page
#include "remote_desktop.h"  // remote-desktop ABI (nexos_fb_query / nexos_input_*)

// Guest socket bridge: the kernel's NE2000 stack exposes a single shared
// socket to Linux guests via these extern "C" entry points (net.cpp).
extern "C" int  net_guest_connect(uint32_t ip, uint16_t port);
extern "C" int  net_guest_send(const void* data, int len);
extern "C" int  net_guest_recv(void* buf, int len);
extern "C" void net_guest_close(void);

// Forward declaration: minimal in-kernel ELF dynamic linker (Stage 6).  The
// definition lives just before linux_run(); sys_execve() calls it.
//   returns 1 if the image is STATIC (caller runs it via the normal path),
//   returns -1 on error, and DOES NOT RETURN when it links + runs a dynamic
//   image (it transfers control to the guest).
static int linux_dynload_and_exec(const char* name, int argc, const char** argv,
                                  int envc, const char** envp, uint32_t stack_top);

// Page-table flags for vmm_map_page().  The 32-bit non-PAE layout has NO NX
// bit, so PROT_EXEC is recorded semantically but every user page is physically
// executable; PROT_WRITE toggles the RW bit; PROT_NONE keeps the page present
// but read-only (a write still #PFs -- full SIGSEGV injection is a later
// milestone because guest and kernel both run in RING 0 and the #PF handler
// cannot yet distinguish them).
#define LX_PG_PRESENT 0x001
#define LX_PG_RW      0x002
#define LX_PG_USER    0x004

// VFS hooks defined in vfs.cpp (C++ linkage; linux_compat.o is linked into
// kernel.elf together with vfs.o, so these resolve at link time).
int  vfs_open(const char* path, int flags);
int  vfs_read(int fd, void* buf, int count);
int  vfs_write(int fd, const void* buf, int count);
int  vfs_close(int fd);

// ---- serial output (self-contained; mirrors kernel's 0x3F8 port) ----
static inline void outb(uint16_t port, uint8_t val){
    __asm__ __volatile__("outb %0, %1" :: "a"(val), "Nd"(port));
}
static void serial_puts(const char* s){ while(*s) outb(0x3F8, (uint8_t)*s++); }
static void serial_puthex(uint32_t v){
    const char* h = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) outb(0x3F8, (uint8_t)h[(v >> i) & 0xF]);
}
static void dbg_hex(const char* label, uint32_t v){
    serial_puts(label);
    serial_puthex(v);
    outb(0x3F8, (uint8_t)'\n');
}

// ---- registered file reader (SFS) ----
static int (*g_reader)(const char*, unsigned char*, int) = 0;

// setjmp/longjmp-style resume context: lets a guest sys_exit unwind straight
// back into linux_run() and return cleanly to its caller (cmd_linux), without
// any fragile in-function label / inline-asm ESP tricks.
// (Ctx is defined in setjmp.h; mini_setjmp/mini_longjmp are the naked asm
//  functions implemented further below.)
static Ctx     g_ctx = {};
static int      g_linux_exit_code = 0;

// Guest handoff slots (absolute-addressed from the entry asm).
static uint32_t g_guest_stack = 0;
static uint32_t g_guest_entry = 0;

// =========================================================================
//  Linux thread control block (TCB) + cooperative scheduler (Stage 1)
// -------------------------------------------------------------------------
//  The guest runs in RING 0, so a thread's trap frame lives on its OWN
//  stack and there is no separate kernel stack.  Context-switching a thread
//  therefore only requires saving the address of its trap frame's
//  SysRegs* slot (resume_esp = r - 4) and `iret`-ing into the next thread.
//  Scheduling is cooperative: a thread only yields at a syscall
//  (sched_yield / futex / clone / exit).
// =========================================================================
#define LINUX_MAX_THREADS 16
#define THREAD_UNUSED   0
#define THREAD_RUNNING  1
#define THREAD_RUNNABLE 2
#define THREAD_EXITED   3
#define THREAD_WAITING  4

// ---- Stage 2 signal delivery ----
// Linux i386 signal numbers we implement (subset of the first 32).
#define LINUX_SIG_MIN        1
#define LINUX_SIG_MAX        31
#define LINUX_SIGUSR1        10
#define LINUX_SIGUSR2        12
#define LINUX_SIGTERM        15
#define LINUX_SIGCHLD        17
#define LINUX_SIGSEGV        11
#define LINUX_SIGILL         4
#define LINUX_SIGBUS         7
#define LINUX_SIGFPE         8
#define LINUX_SIGKILL        9
#define LINUX_SIGINT         2
#define LINUX_SIGQUIT        3
// sigaction sa_flags bits (subset).
#define LINUX_SA_RESTORER    0x04000000   // glibc always sets this on i386
#define LINUX_SA_RESTART     0x10000000
#define LINUX_SA_SIGINFO     0x00000004
#define LINUX_SA_NOCLDSTOP   0x00000001
#define LINUX_SA_NOCLDWAIT   0x00000002
#define LINUX_SA_NODEFER     0x08000000

// Per-thread sigaction record.  handler=0 means SIG_DFL, handler=1 means
// SIG_IGN (we treat 1 as ignore; real Linux uses SIG_IGN=1 too).
struct linux_sigaction {
    uint32_t handler;   // sa_handler (0=DFL, 1=IGN, else addr)
    uint32_t flags;     // sa_flags
    uint32_t restorer;  // sa_restorer (used if SA_RESTORER set)
    uint32_t mask;      // sa_mask
};

// Live signal frame that rt_sigreturn restores.  Mirrors the fields we
// save/restore around a handler invocation.  Stored on the thread's stack
// (below the handler's return address) so rt_sigreturn can recover it.
// The virtual iret frame lives at the TOP of t->sig_slot, leaving a large
// scratch region BELOW it.  linux_switch_asm sets esp = frame base, pops the
// 7 registers, then iret -> handler with esp = frame+44.  Because the handler
// runs in kernel memory and pushes DOWN from frame+44, the scratch region
// (SIG_SLOT_GAP words) absorbs all its stack use without underflowing the
// buffer.  88 words = 352 bytes of scratch below the frame.
#define SIG_SLOT_GAP 88

struct linux_sigframe {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp; // gp regs at pre-signal point
    uint32_t eip;        // return EIP (where the thread was interrupted)
    uint32_t eflags;     // return EFLAGS
    uint32_t esp;        // return ESP (user stack pointer at pre-signal point)
    uint32_t oldmask;    // signal mask to restore on sigreturn
    uint32_t sig;        // signal number that triggered this frame
    uint32_t retaddr;    // -> rt_sigreturn trampoline (handler's "return addr")
};

struct linux_thread {
    uint32_t resume_esp;   // addr of this thread's trap-frame SysRegs* slot
    uint32_t gs_sel;       // TLS selector for this thread (0 = none)
    uint32_t stack_top;    // top of this thread's stack (debug)
    uint32_t tls_base;     // TLS block base (debug)
    int      tid;
    int      state;
    // ---- Stage 2 signal state ----
    linux_sigaction sa[LINUX_SIG_MAX + 1]; // per-signal disposition (1..31)
    uint32_t blocked;       // blocked signal mask (bit n = sig n)
    uint32_t pend;          // pending signal mask (bit n = sig n)
    linux_sigframe sigframe; // saved context while a handler runs (kernel-side;
                            // NOT on the guest stack, to avoid corrupting it)
    uint32_t sigframe_active; // 1 while a handler is running
    uint32_t sig_slot[160];  // virtual iret slot (kernel-side; esp is forced
                            // here by linux_switch_asm so the handler runs in
                            // kernel memory and the guest stack is untouched).
                            // The iret frame lives near the TOP (offset
                            // SIG_SLOT_GAP words) so the handler, which runs
                            // with esp just above the frame and pushes DOWN,
                            // has a large scratch region below it and never
                            // underflows the buffer.
    uint32_t saved_resume;  // resume_esp to restore on rt_sigreturn
};
static linux_thread g_threads[LINUX_MAX_THREADS];
static int g_cur = 0;            // index of the currently running thread
static int g_live_threads = 0;   // number of non-exited threads
static int g_next_tid = 1;

// futex wait bookkeeping: a small fixed array of (uaddr, thread) waiters.
#define LINUX_FUTEX_MAX_WAIT 32
struct linux_futex_q { uint32_t uaddr; int tid; } g_futex_wait[LINUX_FUTEX_MAX_WAIT];

// Forward decls (defined after the dispatch switch).
static void sys_linux_exit_thread(uint32_t code);
static void sys_linux_exit_group(uint32_t code);
// Stage 2: deliver any pending & unblocked signals for the current thread
// before returning to (or iret-ing into) the guest.  May rewrite *r so the
// thread resumes in a signal handler instead of its normal return point.
// Declared extern "C" (not static) so the int 0x80 entry asm can call it.
extern "C" void linux_deliver_signals(SysRegs* r, int from_trap);

// forward decls (defined after the dispatch switch)
static void sys_linux_exit_thread(uint32_t code);
static void sys_linux_exit_group(uint32_t code);

// =========================================================================
//  Syscall dispatch (Linux i386 numbers)
// =========================================================================
static void serial_putdec(int v){
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { if (v < 0){ b[i++]='-'; v=-v; } while(v){ b[i++] = (char)('0' + v%10); v/=10; } }
    for (int j = i-1; j >= 0; j--) outb(0x3F8, (uint8_t)b[j]);
}

// Minimal freestanding setjmp/longjmp (i386), written as NAKED asm functions
// so we control the prologue/epilogue and can read the true return address.
// Ctx layout: { ebp, ebx, esi, edi, esp, eip } at offsets 0,4,8,12,16,20.

// ---- optional syscall trace (off by default; flip to 1 while debugging) ----
#define LINUX_TRACE_SYSCALLS 0
#if LINUX_TRACE_SYSCALLS
static void trace_sys(uint32_t num, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3){
    serial_puts("[sc ");
    serial_putdec((int)num);
    serial_puts(" ");
    serial_puts("0x");
    { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
      uint32_t v=a0; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
    serial_puts(" 0x");
    { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
      uint32_t v=a1; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
    serial_puts(" 0x");
    { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
      uint32_t v=a2; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
    serial_puts(" f=0x");
    { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
      uint32_t v=a3; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
    serial_puts("\n");
}
#endif

extern "C" int __attribute__((naked)) mini_setjmp(Ctx*){
    __asm__ __volatile__(
        "movl 4(%esp), %eax\n"       // eax = c (first cdecl arg)
        "movl %ebp, 0(%eax)\n"
        "movl %ebx, 4(%eax)\n"
        "movl %esi, 8(%eax)\n"
        "movl %edi, 12(%eax)\n"
        "movl %esp, 16(%eax)\n"      // esp points at this fn's return address
        "movl 0(%esp), %ecx\n"       // return address into caller (linux_run)
        "movl %ecx, 20(%eax)\n"
        "xorl %eax, %eax\n"          // setjmp returns 0 on first call
        "ret\n"
    );
    __builtin_unreachable();
}

extern "C" void __attribute__((naked)) mini_longjmp(Ctx*, int){
    __asm__ __volatile__(
        "movl 4(%esp), %eax\n"       // eax = c
        "movl 0(%eax), %ebp\n"
        "movl 4(%eax), %ebx\n"
        "movl 8(%eax), %esi\n"
        "movl 12(%eax), %edi\n"
        "movl 16(%eax), %esp\n"      // restore esp (points at ret-addr slot)
        "movl 20(%eax), %ecx\n"      // return address into linux_run
        "movl $1, %eax\n"            // setjmp returns nonzero on longjmp
        "movl %ecx, (%esp)\n"        // place ret addr on the restored stack
        "ret\n"                      // return into linux_run after setjmp
    );
    __builtin_unreachable();
}

// ---- Stage 1 scheduler -------------------------------------------------
// Naked asm that switches to the next thread's trap frame and `iret`s into
// it.  Called from C with the next thread's resume_esp (address of its
// SysRegs* slot) and gs selector.  It does NOT return: it performs the
// `iret` that resumes the target thread.  All 7 general registers are
// reloaded from the target's trap frame so the thread resumes exactly where
// it left off (the `int 0x80` return point with the correct registers).
extern "C" void __attribute__((naked)) linux_switch_asm(uint32_t next_esp,
                                                        uint32_t next_gs){
    (void)next_esp; (void)next_gs;   // consumed inside the inline asm below
    __asm__ __volatile__(
        "movl 8(%esp), %eax\n"    // next_gs
        "movw %ax, %gs\n"
        "movl 4(%esp), %esp\n"    // ESP = next_esp (next thread's SysRegs* slot)
        "movl 4(%esp), %eax\n"    // reload next's eax  (r->eax)
        "movl 8(%esp), %ebx\n"    //        ebx
        "movl 12(%esp), %ecx\n"   //        ecx
        "movl 16(%esp), %edx\n"   //        edx
        "movl 20(%esp), %esi\n"   //        esi
        "movl 24(%esp), %edi\n"   //        edi
        "movl 28(%esp), %ebp\n"   //        ebp
        "addl $32, %esp\n"        // drop SysRegs* slot + 7 saved registers
        "iret\n"
    );
    __builtin_unreachable();
}

// Save the current thread's resume point, pick the next RUNNABLE thread, and
// switch to it.  Called from inside linux_syscall_dispatch (r = current trap
// frame).  If no other thread is runnable, returns and the caller keeps
// running (cooperative; no preemption).
static void linux_do_switch(SysRegs* r){
    int cur = g_cur;
    g_threads[cur].resume_esp = (uint32_t)r - 4;   // r = S-40; slot = S-44
    int nx = -1;
    for (int i = 1; i <= LINUX_MAX_THREADS; i++){
        int idx = (cur + i) % LINUX_MAX_THREADS;
        if (g_threads[idx].state == THREAD_RUNNABLE){ nx = idx; break; }
    }
    if (nx < 0) return;                            // nobody else to run
    g_threads[cur].state = THREAD_RUNNABLE;        // we yielded
    g_threads[nx].state  = THREAD_RUNNING;
    g_cur = nx;
    linux_switch_asm(g_threads[nx].resume_esp, g_threads[nx].gs_sel);
    __builtin_unreachable();
}

// Park the current thread on futex uaddr (FUTEX_WAIT).  We record the waiter
// and switch away; when re-scheduled we resume at the syscall return point
// (the value changed).  r->eax must already be set to the value to return.
static void linux_futex_park(uint32_t uaddr, SysRegs* r){
    int cur = g_cur;
    int slot = -1;
    for (int i = 0; i < LINUX_FUTEX_MAX_WAIT; i++)
        if (g_futex_wait[i].tid < 0){ slot = i; break; }
    if (slot >= 0){ g_futex_wait[slot].uaddr = uaddr; g_futex_wait[slot].tid = cur; }
    g_threads[cur].state = THREAD_WAITING;
    g_threads[cur].resume_esp = (uint32_t)r - 4;
    int nx = -1;
    for (int i = 1; i <= LINUX_MAX_THREADS; i++){
        int idx = (cur + i) % LINUX_MAX_THREADS;
        if (g_threads[idx].state == THREAD_RUNNABLE){ nx = idx; break; }
    }
    if (nx < 0){                                   // deadlock guard
        g_threads[cur].state = THREAD_RUNNABLE;
        if (slot >= 0) g_futex_wait[slot].tid = -1;
        return;
    }
    // Before switching to nx, deliver any pending signal it has so it does
    // not resume in the middle of a stale context.  linux_do_switch and
    // this park both funnel through the same logic.
    linux_deliver_signals((SysRegs*)(g_threads[nx].resume_esp + 4), 0);
    g_threads[nx].state = THREAD_RUNNING;
    g_cur = nx;
    linux_switch_asm(g_threads[nx].resume_esp, g_threads[nx].gs_sel);
    __builtin_unreachable();
}

// =========================================================================
//  Stage 2 signal delivery engine
// -------------------------------------------------------------------------
//  Linux delivers signals at the kernel→user boundary (here: the iret path
//  of int 0x80, and whenever a thread is switched back in).  Because the
//  guest runs in RING 0, `iret` does NOT change esp, so we cannot simply
//  rewrite a trap frame's esp.  Instead we build a *virtual* SysRegs slot on
//  the thread's own stack, point resume_esp at it, and switch into it via
//  linux_switch_asm (which forces esp = resume_esp, pops the 6 registers,
//  and iret-s into EIP).  The virtual slot's EIP = the handler; its +44
//  word = the rt_sigreturn trampoline address (the handler's "return addr").
//  When the handler returns it `ret`-s into rt_sigreturn, which restores the
//  saved original context from the signal frame and switches back.
// =========================================================================

// Build a signal frame for `sig` on the current thread's stack and rewrite
// its trap frame so the next iret enters the handler.  Must be called with
// r = the current thread's live SysRegs* (the frame that will be iret-ed).
extern "C" void linux_deliver_signals(SysRegs* r, int from_trap){
    int cur = g_cur;
    linux_thread* t = &g_threads[cur];
    uint32_t deliverable = t->pend & ~t->blocked;
    if (!deliverable) return;

    // lowest-numbered pending & unblocked signal wins
    int sig = -1;
    for (int s = 1; s <= LINUX_SIG_MAX; s++)
        if (deliverable & (1u << s)) { sig = s; break; }
    if (sig < 0) return;

    t->pend &= ~(1u << sig);
    linux_sigaction* sa = &t->sa[sig];

    if (sa->handler == 1u){            // SIG_IGN
        return;
    }
    if (sa->handler == 0u){            // SIG_DFL
        // Default actions.  Ignored by default:
        if (sig == LINUX_SIGCHLD || sig == LINUX_SIGUSR1 ||
            sig == LINUX_SIGUSR2 || sig == 13 /*SIGPIPE*/){
            return;
        }
        // Everything else terminates the thread (or the whole process if it
        // is the main thread / last live thread).
        if (cur == 0 || g_live_threads <= 1)
            sys_linux_exit_group((uint32_t)(128 + sig));
        else
            sys_linux_exit_thread((uint32_t)(128 + sig));
        __builtin_unreachable();
    }

    // ---- real handler: build the frame ----
    // r is the SysRegs* at the top of a frame (resume_esp = r-4).  Because the
    // guest runs in RING 0, `int 0x80` does NOT push ESP/SS, so the guest's
    // ESP at the trap is simply the address r+40 (the 7 pushed regs + the 3
    // CPU-pushed EFLAGS/CS/EIP sit 40 bytes below it):
    //   r+0=eax .. r+28=EIP r+32=CS r+36=EFLAGS, and r+40 == guest ESP.
    volatile uint32_t* rr = (volatile uint32_t*)r;
    uint32_t ret_eip   = rr[7];
    uint32_t ret_cs    = rr[8] ? rr[8] : 0x08;
    uint32_t ret_efl   = rr[9];
    uint32_t user_esp  = (uint32_t)r + 40;

    // Virtual iret slot: we force esp to a KERNEL-SIDE buffer (t->sig_slot)
    // via linux_switch_asm, so the handler runs entirely in kernel memory and
    // the guest's own stack is never touched (no corruption of guest locals).
    // After the handler returns + rt_sigreturn, esp is restored to user_esp.
    // The frame lives at offset SIG_SLOT_GAP so the handler (esp=frame+44,
    // pushing down) has a large scratch region below it and never underflows.
    uint32_t* vp = t->sig_slot + SIG_SLOT_GAP;

    // save original context into the kernel-side signal frame
    t->sigframe.eax     = r->eax;
    t->sigframe.ebx     = r->ebx;
    t->sigframe.ecx     = r->ecx;
    t->sigframe.edx     = r->edx;
    t->sigframe.esi     = r->esi;
    t->sigframe.edi     = r->edi;
    t->sigframe.ebp     = r->ebp;
    t->sigframe.eip     = ret_eip;
    t->sigframe.eflags  = ret_efl;
    t->sigframe.esp     = user_esp;
    t->sigframe.oldmask = t->blocked;
    t->sigframe.sig     = (uint32_t)sig;
    t->sigframe.retaddr = 0;

    // virtual iret slot (matches linux_switch_asm expectation):
    //   slot[0]=eax slot[1]=pad slot[2..7]=ebx..ebp
    //   slot[8]=EIP slot[9]=CS slot[10]=EFLAGS  (iret frame at slot+32)
    // linux_switch_asm sets esp = slot, pops regs, iret -> handler.
    // After iret esp = slot+44.  The handler reads its first arg `sig` from
    // [esp+4]; [esp+0] is the return address (restorer -> rt_sigreturn):
    //   slot+44 = restorer (return addr; handler `ret`-s into rt_sigreturn)
    //   slot+48 = sig number (handler(int sig) first argument)
    vp[0] = r->eax;
    vp[1] = (uint32_t)vp;            // self-pointer placeholder
    vp[2] = r->ebx;
    vp[3] = r->ecx;
    vp[4] = r->edx;
    vp[5] = r->esi;
    vp[6] = r->edi;
    vp[7] = r->ebp;
    vp[8] = sa->handler;             // EIP -> handler
    vp[9] = ret_cs;                  // CS
    vp[10]= ret_efl;                 // EFLAGS
    vp[11] = sa->restorer ? sa->restorer : 0; // [esp+0] -> return addr
    vp[12] = (uint32_t)sig;          // [esp+4] -> handler(int sig)

    // block this signal while the handler runs (unless SA_NODEFER)
    if (!(sa->flags & LINUX_SA_NODEFER))
        t->blocked |= (1u << sig);
    t->blocked |= sa->mask;          // also block sa_mask

    // mark a handler as active; rt_sigreturn clears it
    t->sigframe_active = 1;
    t->saved_resume  = (uint32_t)r - 4;

    // Switch into the handler.  ring-0 iret does NOT reload esp, so we force
    // esp = sig_slot via linux_switch_asm (movl next_esp, %esp then iret).
    g_threads[cur].resume_esp = (uint32_t)vp;
    linux_switch_asm((uint32_t)vp, t->gs_sel);
    __builtin_unreachable();
}

// rt_sigreturn: restore the original context saved in the signal frame.
static void sys_linux_rt_sigreturn(void){
    int cur = g_cur;
    linux_thread* t = &g_threads[cur];
    if (!t->sigframe_active){ return; }   // nothing to restore
    linux_sigframe* sfp = &t->sigframe;    // kernel-side saved context

    // Rebuild the virtual iret slot in kernel memory (t->sig_slot) so the
    // guest stack is never touched.  After iret (esp = slot+44) the guest
    // resumes with its original user ESP (sfp->esp).  Use the same high offset
    // (SIG_SLOT_GAP) as linux_deliver_signals so the geometry matches.
    uint32_t* rp = t->sig_slot + SIG_SLOT_GAP;
    rp[0] = sfp->eax;
    rp[1] = (uint32_t)rp;
    rp[2] = sfp->ebx;
    rp[3] = sfp->ecx;
    rp[4] = sfp->edx;
    rp[5] = sfp->esi;
    rp[6] = sfp->edi;
    rp[7] = sfp->ebp;
    rp[8] = sfp->eip;                // EIP (return point)
    rp[9] = 0x08;                    // CS
    rp[10]= sfp->eflags;             // EFLAGS
    // restore the signal mask
    t->blocked = sfp->oldmask;
    t->sigframe_active = 0;
    t->resume_esp = (uint32_t)rp;
    linux_switch_asm((uint32_t)rp, t->gs_sel);
    __builtin_unreachable();
}

// =========================================================================
//  Linux i386 stat structure (glibc layout, 88 bytes).
struct linux_stat32 {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime; uint32_t st_atime_nsec;
    uint32_t st_mtime; uint32_t st_mtime_nsec;
    uint32_t st_ctime; uint32_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
} __attribute__((packed));

// Linux utsname (i386, 65-byte fields).
struct linux_utsname {
    char sysname[65];    // "Linux"
    char nodename[65];   // hostname
    char release[65];    // kernel release
    char version[65];    // kernel version
    char machine[65];    // "i686"
    char domainname[65];
} __attribute__((packed));

// Minimal per-guest file descriptor table (fd 0/1/2 = stdin/out/err).
static int g_linux_fd[8];
static bool g_linux_fd_open[8];
static bool g_linux_initialized_fds = false;

static void linux_fd_init(){
    if (g_linux_initialized_fds) return;
    for (int i = 0; i < 8; i++){ g_linux_fd_open[i] = false; g_linux_fd[i] = -1; }
    // fd 0/1/2 -> serial (handled specially in read/write)
    g_linux_fd_open[0] = g_linux_fd_open[1] = g_linux_fd_open[2] = true;
    g_linux_initialized_fds = true;
}

// ---- Stage 3: real mmap arena with per-region tracking ----
//
// mmap hands out real, physically-backed 4 KiB pages (vmm_map_page +
// pmm_alloc_page) from a bump cursor that grows DOWNWARD inside the PG_USER
// region.  Each allocation is recorded in a region table so munmap/mprotect
// can find and re-map it.  The arena sits ABOVE the guest stack (which now
// lives at 192 MiB) and below the top of the 256 MiB identity map, giving a
// much larger, collision-free playground than the old 80 MiB overlap.
#define LINUX_MMAP_MAX 24
struct linux_mmap_region {
    uint32_t addr;   // page-aligned base VA
    uint32_t alen;  // page-aligned length
    uint32_t prot;  // PROT_READ/WRITE/EXEC/NONE (Linux constants)
    uint32_t flags; // MAP_* (Linux constants)
    bool     used;
};
static linux_mmap_region g_mmap_regions[LINUX_MMAP_MAX];

// Arena bounds (virtual addresses inside the 64-256 MiB PG_USER map).
//   guest stack top  = 0x0C000000 (192 MiB), grows DOWN toward strings
//   mmap arena       = 0x0C100000 (just above stack) .. 0x0FFFF000 (below 256M)
static uint32_t g_mmap_cursor = 0x0FFFF000u;  // 255.9 MiB: start near map top
static const  uint32_t MMAP_FLOOR = 0x0C100000u; // 193 MiB: above guest stack

// Apply `prot` to every 4 KiB page in [addr, addr+alen).  Allocates fresh
// physical pages (zero-filled) on first mapping.  prot bits (Linux):
//   PROT_READ=0x1  PROT_WRITE=0x2  PROT_EXEC=0x4  PROT_NONE=0x0
static void linux_mmap_apply_prot(uint32_t addr, uint32_t alen, uint32_t prot){
    uint32_t want_flags = LX_PG_PRESENT | LX_PG_USER;
    if (prot & 0x2) want_flags |= LX_PG_RW;   // PROT_WRITE -> RW bit
    // PROT_EXEC has no hardware bit on 32-bit non-PAE; page stays executable.
    // PROT_NONE -> present + read-only (writes #PF).  Full no-access needs
    // SIGSEGV injection, deferred.
    bool is_none = (prot == 0);
    for (uint32_t p = addr; p < addr + alen; p += 0x1000){
        uint32_t phys = pmm_alloc_page();
        if (!phys) break;                     // OOM: best-effort
        uint32_t f = want_flags;
        if (is_none) f &= ~LX_PG_RW;          // PROT_NONE: read-only
        vmm_map_page(phys, p, f);
        // Linux mmap contract: fresh anonymous pages are zero-filled.
        for (uint32_t j = 0; j < 0x1000; j += 4)
            *(volatile uint32_t*)(p + j) = 0;
    }
}

static linux_mmap_region* linux_mmap_find(uint32_t addr, uint32_t len){
    for (int i = 0; i < LINUX_MMAP_MAX; i++){
        linux_mmap_region* r = &g_mmap_regions[i];
        if (r->used && r->addr <= addr && addr + len <= r->addr + r->alen)
            return r;
    }
    return 0;
}

static void linux_mmap_reset(void){
    for (int i = 0; i < LINUX_MMAP_MAX; i++) g_mmap_regions[i].used = false;
    g_mmap_cursor = 0x0FFFF000u;
}

// Per-guest brk state.  g_linux_brk_base is set by linux_run() to the
// (page-aligned) end of the loaded data segment; the guest grows the break
// upward from there.  g_linux_brk tracks the current break.
static uint32_t g_linux_brk_base = 0x04000000u;
static uint32_t g_linux_brk      = 0x04000000u;
static uint32_t g_linux_stack_top = 0;   // set by linux_run(); brk cap

// Forward decls for execve (defined after the dispatch switch).
static int linux_load_image(const char* name, uint32_t* out_entry,
                            uint32_t* out_max_end, uint32_t* out_load_base,
                            uint32_t* out_phoff, uint16_t* out_phentsz,
                            uint16_t* out_phnum);
static uint32_t linux_build_stack(int argc, const char** argv, int envc_arg,
                                  const char** envp, uint32_t stack_top,
                                  uint32_t max_end, uint32_t load_base,
                                  uint32_t phoff, uint16_t phentsz, uint16_t phnum,
                                  uint32_t entry, uint32_t* out_str_base,
                                  uint32_t unused_tail);

extern "C" void linux_syscall_dispatch(SysRegs* r){
    uint32_t num = r->eax;
    linux_fd_init();
#if LINUX_TRACE_SYSCALLS
    trace_sys(num, r->ebx, r->ecx, r->edx, r->esi);
#endif
    switch (num){
        case 1: { // sys_exit: exit the CALLING thread
            sys_linux_exit_thread((uint32_t)r->ebx);
            __builtin_unreachable();
        }
        case 3: { // sys_read (fd 0 -> EOF; fd>=3 -> VFS)
            int fd = (int)r->ebx;
            if (fd == 0){ r->eax = 0; return; }              // stdin EOF
            if (fd >= 1 && fd <= 2){ r->eax = 0; return; }   // serial has no input
            if (fd < 8 && g_linux_fd_open[fd]){
                r->eax = (uint32_t)vfs_read(g_linux_fd[fd], (void*)r->ecx, (int)r->edx);
            } else {
                r->eax = (uint32_t)-9; // -EBADF
            }
            return;
        }
        case 4: { // sys_write (fd 1/2 -> serial; fd>=3 -> VFS)
            int fd = (int)r->ebx;
            const char* buf = (const char*)r->ecx;
            uint32_t count = r->edx;
            if (fd == 1 || fd == 2){
                for (uint32_t i = 0; i < count; i++) outb(0x3F8, (uint8_t)buf[i]);
                r->eax = count;
            } else if (fd < 8 && g_linux_fd_open[fd]){
                r->eax = (uint32_t)vfs_write(g_linux_fd[fd], buf, (int)count);
            } else {
                r->eax = (uint32_t)-9; // -EBADF
            }
            return;
        }
        case 5: { // sys_open (path in ebx, flags in ecx)
            const char* path = (const char*)r->ebx;
            if (!path){ r->eax = (uint32_t)-14; return; }    // -EFAULT
            for (int i = 3; i < 8; i++){
                if (!g_linux_fd_open[i]){
                    int vfd = vfs_open(path, 0);
                    if (vfd < 0){ r->eax = (uint32_t)-2; return; } // -ENOENT
                    g_linux_fd_open[i] = true;
                    g_linux_fd[i] = vfd;
                    r->eax = (uint32_t)i;
                    return;
                }
            }
            r->eax = (uint32_t)-24; // -EMFILE
            return;
        }
        case 6: // sys_close
            r->eax = 0;
            return;
        case 400: { // sys_socket (guest TCP: alloc shared socket)
            // Single shared guest socket; "create" just returns fd 0.
            r->eax = 0;
            return;
        }
        case 401: { // sys_connect (ebx=ip host-order, ecx=port host-order)
            uint32_t ip   = (uint32_t)r->ebx;
            uint16_t port = (uint16_t)r->ecx;
            int rc = net_guest_connect(ip, port);
            r->eax = (rc < 0) ? (uint32_t)-1 : 0;
            return;
        }
        case 402: { // sys_send (ebx=buf, ecx=len)
            const void* buf = (const void*)r->ebx;
            int len = (int)r->ecx;
            int rc = net_guest_send(buf, len);
            r->eax = (rc < 0) ? (uint32_t)-1 : (uint32_t)rc;
            return;
        }
        case 403: { // sys_recv (ebx=buf, ecx=len)
            void* buf = (void*)r->ebx;
            int len = (int)r->ecx;
            int rc = net_guest_recv(buf, len);
            r->eax = (rc < 0) ? (uint32_t)-1 : (uint32_t)rc;
            return;
        }
        case 404: { // sys_close (guest socket)
            net_guest_close();
            r->eax = 0;
            return;
        }
        case 11: { // sys_execve (path in ebx, argv in ecx, envp in edx)
            // Replace the running guest image with a new ELF from SFS.
            // argv/envp are guest pointers; copy argv strings into kernel
            // memory, then load and jump to the new entry WITHOUT re-arming
            // the setjmp resume point (g_ctx already captures the original
            // cmd_linux frame, so a later sys_exit still unwinds cleanly).
            const char* path = (const char*)r->ebx;
            if (!path){ r->eax = (uint32_t)-14; return; } // -EFAULT
            while (*path == '/') path++;                  // strip leading /
            // Copy guest argv[] (NULL-terminated pointer array in ecx).
            static char  argv_buf[33][96];
            static const char* av[33];
            const volatile uint32_t* gargv = (const volatile uint32_t*)r->ecx;
            int ac = 0;
            if (gargv){
                for (; ac < 32; ac++){
                    uint32_t p = gargv[ac];
                    if (!p) break;
                    const volatile char* s = (const volatile char*)p;
                    int w = 0;
                    while (s[w] && w < 95){ argv_buf[ac][w] = (char)s[w]; w++; }
                    argv_buf[ac][w] = 0;
                    av[ac] = argv_buf[ac];
                }
            }
            if (ac == 0){ av[0] = path; ac = 1; }  // at least argv[0]
            av[ac] = 0;
            // Copy guest envp[] (NULL-terminated pointer array in edx).
            static char  envp_buf[64][256];
            static const char* ev[64];
            const volatile uint32_t* genvp = (const volatile uint32_t*)r->edx;
            int ec = 0;
            if (genvp){
                for (; ec < 63; ec++){
                    uint32_t p = genvp[ec];
                    if (!p) break;
                    const volatile char* s = (const volatile char*)p;
                    int w = 0;
                    while (s[w] && w < 255){ envp_buf[ec][w] = (char)s[w]; w++; }
                    envp_buf[ec][w] = 0;
                    ev[ec] = envp_buf[ec];
                }
            }
            const char** envp_to_pass = (ec > 0) ? ev : (const char**)0;
            // Load the new image; on success we never return here.
            uint32_t entry = 0, max_end = 0, load_base = 0, phoff = 0;
            uint16_t phentsz = 0, phnum = 0;
            const char* load_name = path;
            if (linux_load_image(path, &entry, &max_end, &load_base, &phoff,
                                 &phentsz, &phnum) < 0){
                // Busybox multi-call fallback: applets (id/ls/echo...) are
                // dispatched by argv[0], not by separate files.  When the
                // requested path is missing, exec busybox itself -- its
                // argv[0] is still the applet name, so busybox picks the
                // right applet.  This mirrors Linux's busybox symlinks.
                if (linux_load_image("busybox", &entry, &max_end, &load_base,
                                     &phoff, &phentsz, &phnum) < 0){
                    r->eax = (uint32_t)-2; // -ENOENT
                    return;
                }
                load_name = "busybox";
            }
            (void)load_name;
            // Re-arm heap/mmap for the new image.
            uint32_t stack_top = 0x0C000000u;
            g_linux_stack_top = stack_top;
            g_linux_brk_base  = (max_end + 0xFFFu) & ~0xFFFu;
            if (g_linux_brk_base > 0x09000000u) g_linux_brk_base = 0x09000000u;
            g_linux_brk       = g_linux_brk_base;
            linux_mmap_reset();
            // If the new image is dynamically linked, link it now (maps libs,
            // applies relocations, transfers control; returns 1 if static).
            int dr = linux_dynload_and_exec(path, ac, av, ec, envp_to_pass, stack_top);
            if (dr < 0){ r->eax = (uint32_t)-2; return; }
            if (dr == 1) {
                // Static image: build the new startup stack and jump.
                uint32_t str_base_unused = 0;
                uint32_t stack = linux_build_stack(ac, av, ec, envp_to_pass, stack_top,
                                                   max_end, load_base, phoff, phentsz,
                                                   phnum, entry, &str_base_unused, 0);
                if (!stack){ r->eax = (uint32_t)-2; return; }
                g_guest_stack = stack;
                g_guest_entry = entry;
                __asm__ __volatile__(
                    "mov %0, %%edx\n"
                    "mov %1, %%eax\n"
                    "mov %%edx, %%esp\n"
                    "jmp *%%eax\n"
                    :: "m"(g_guest_stack), "m"(g_guest_entry)
                    : "eax", "edx", "memory");
                __builtin_unreachable();
            }
            return;  // dr == 0 unreachable (dynamic path jumped)
        }
        case 19: // sys_lseek (return 0: all files are streams)
            r->eax = 0;
            return;
        case 20: // sys_getpid
            r->eax = 1;
            return;
        case 24: // sys_getuid
        case 47: // sys_getgid
        case 49: // sys_geteuid
            r->eax = 0;
            return;
        case 63: { // sys_dup2 (oldfd in ebx, newfd in ecx)
            int oldfd = (int)r->ebx;
            int newfd = (int)r->ecx;
            if (newfd < 0 || newfd >= 8 || oldfd < 0 || oldfd >= 8){
                r->eax = (uint32_t)-9; return; // -EBADF
            }
            if (!g_linux_fd_open[oldfd]){ r->eax = (uint32_t)-9; return; }
            g_linux_fd_open[newfd] = true;
            g_linux_fd[newfd] = g_linux_fd[oldfd];
            // Note: fd 0/1/2 keep their "serial" semantics by guest fd number
            // in read/write, so copying the VFS slot is all dup2 needs.
            r->eax = (uint32_t)newfd;
            return;
        }
        case 64: { // sys_dup (oldfd in ebx): pick the lowest free fd
            int oldfd = (int)r->ebx;
            if (oldfd < 0 || oldfd >= 8 || !g_linux_fd_open[oldfd]){
                r->eax = (uint32_t)-9; return; // -EBADF
            }
            for (int i = 0; i < 8; i++){
                if (!g_linux_fd_open[i]){
                    g_linux_fd_open[i] = true;
                    g_linux_fd[i] = g_linux_fd[oldfd];
                    r->eax = (uint32_t)i;
                    return;
                }
            }
            r->eax = (uint32_t)-24; // -EMFILE
            return;
        }
        case 45: { // sys_brk: real static binaries probe with brk(0) then grow
            // Initial break must be the END OF THE DATA SEGMENT (like Linux's
            // _end), NOT a fixed low address: busybox's data sits at
            // 0x0814xxxx and a low brk would grow over the kernel heap
            // (0x00500000+).  linux_run() sets g_linux_brk_base to
            // max_end aligned; we grow upward from there.  The break must
            // NEVER reach the argv-string region (0x09000000) -- that lives
            // above the heap -- so the cap is BRK_CAP, NOT the stack top.
            uint32_t want = r->ebx;
            uint32_t top = 0x09000000u;       // below argv strings
            if (want == 0){
                r->eax = g_linux_brk;         // query current break
            } else if (want >= g_linux_brk && want < top){
                // Zero-fill the freshly extended heap region (Linux contract).
                uint32_t old = g_linux_brk;
                g_linux_brk = want;           // grow break
                for (uint32_t j = old; j < want && j - old < 0x1000000u; j++)
                    *(volatile unsigned char*)j = 0;
                r->eax = g_linux_brk;
            } else {
                r->eax = g_linux_brk;         // can't shrink below start; report current
            }
            return;
        }
        case 54: // sys_ioctl: report ENOTTY (not a terminal) so isatty() fails
            r->eax = (uint32_t)-25; // -ENOTTY
            return;
        case 78: { // sys_gettimeofday: zero timeval (guests rarely check)
            if (r->ebx){ // struct timeval* in ebx
                volatile uint32_t* tv = (volatile uint32_t*)r->ebx;
                tv[0] = 0;  // tv_sec
                tv[1] = 0;  // tv_usec
            }
            r->eax = 0;
            return;
        }
        case 85: // sys_readlink: not implemented
            r->eax = (uint32_t)-2; // -ENOENT
            return;
        case 90: { // sys_mmap (old_mmap: arg pointer in ebx)
            // struct old_mmap { addr, len, prot, flags, fd, offset }
            const volatile uint32_t* a = (const volatile uint32_t*)r->ebx;
            uint32_t len = a[1];
            uint32_t prot = a[2];
            uint32_t flags = a[3];
            if ((flags & 0x20) == 0){ r->eax = (uint32_t)-1; return; } // MAP_ANONYMOUS only
            uint32_t alloc = (len + 0xFFFu) & ~0xFFFu;
            if (flags & 0x10){ // MAP_FIXED: honour requested VA
                uint32_t want = a[0] & ~0xFFFu;
                if (want < 0x10000u || want >= 0x10000000u){ r->eax = (uint32_t)-1; return; }
                // Register / (re)map the fixed region with real pages.
                linux_mmap_apply_prot(want, alloc, prot);
                // Record it (overwrite any prior region at this VA).
                linux_mmap_region* slot = 0;
                for (int i = 0; i < LINUX_MMAP_MAX; i++)
                    if (!g_mmap_regions[i].used){ slot = &g_mmap_regions[i]; break; }
                if (slot){ slot->addr = want; slot->alen = alloc; slot->prot = prot; slot->flags = flags; slot->used = true; }
                r->eax = want;
                return;
            }
            if (alloc > g_mmap_cursor - MMAP_FLOOR){ r->eax = (uint32_t)-1; return; } // ENOMEM
            g_mmap_cursor -= alloc;
            uint32_t base = g_mmap_cursor;
            linux_mmap_apply_prot(base, alloc, prot);
            linux_mmap_region* slot = 0;
            for (int i = 0; i < LINUX_MMAP_MAX; i++)
                if (!g_mmap_regions[i].used){ slot = &g_mmap_regions[i]; break; }
            if (slot){ slot->addr = base; slot->alen = alloc; slot->prot = prot; slot->flags = flags; slot->used = true; }
            r->eax = base;
            return;
        }
        case 91: { // sys_munmap(addr in ebx, len in ecx)
            uint32_t addr = r->ebx & ~0xFFFu;
            uint32_t len  = (r->ecx + 0xFFFu) & ~0xFFFu;
            linux_mmap_region* reg = linux_mmap_find(addr, len);
            if (reg){
                // Unmap each 4 KiB page (drop PRESENT) and free the phys page.
                for (uint32_t p = reg->addr; p < reg->addr + reg->alen; p += 0x1000){
                    uint32_t phys = vmm_get_phys(p);
                    if (phys) pmm_free_page(phys);
                    vmm_map_page(0, p, 0);   // clears PTE present bit
                }
                reg->used = false;
            }
            r->eax = 0;
            return;
        }
        case 125: { // sys_mprotect(addr in ebx, len in ecx, prot in edx)
            uint32_t addr = r->ebx & ~0xFFFu;
            uint32_t len  = (r->ecx + 0xFFFu) & ~0xFFFu;
            uint32_t prot = r->edx;
            linux_mmap_region* reg = linux_mmap_find(addr, len);
            if (!reg){ r->eax = (uint32_t)-1; return; } // EINVAL: no such mapping
            reg->prot = prot;
            linux_mmap_apply_prot(reg->addr, reg->alen, prot);
            r->eax = 0;
            return;
        }
        case 108: { // sys_fstat (fd in ebx, struct stat* in ecx)
            int fd = (int)r->ebx;
            volatile struct linux_stat32* st = (volatile struct linux_stat32*)r->ecx;
            if (!st){ r->eax = (uint32_t)-14; return; }
            // Fill a plausible regular-file/char-device stat.
            for (int i = 0; i < (int)(sizeof(struct linux_stat32)/4); i++)
                ((volatile uint32_t*)st)[i] = 0;
            st->st_dev   = 0x0800;
            st->st_ino   = 1 + (uint32_t)fd;
            st->st_mode  = (fd <= 2) ? 0x2000 : 0x8000;  // char dev / regular
            st->st_nlink = 1;
            st->st_uid   = 0;
            st->st_gid   = 0;
            st->st_size  = 0;
            r->eax = 0;
            return;
        }
        case 122: { // sys_uname (struct utsname* in ebx)
            volatile struct linux_utsname* u = (volatile struct linux_utsname*)r->ebx;
            if (!u){ r->eax = (uint32_t)-14; return; }
            const char* fields[5] = { "Linux", "nexos", "6.1.0-nexos",
                                      "#1 SMP NexOS", "i686" };
            for (int f = 0; f < 5; f++){
                const char* src = fields[f];
                volatile char* dst = (volatile char*)u + f * 65;
                int i = 0;
                while (src[i] && i < 64){ dst[i] = src[i]; i++; }
                dst[i] = 0;
            }
            r->eax = 0;
            return;
        }
        case 123: // sys_setrlimit: accept and ignore (return 0)
            r->eax = 0;
            return;
        case 243: { // sys_set_thread_area: point the GDT TLS descriptor at
                    // the guest's TLS block.  Linux passes struct user_desc*
                    // in ebx: { entry_number, base_addr, limit, seg_32bit,
                    //   contents, read_exec_only, limit_in_pages,
                    //   seg_not_present, useable }
            const volatile uint32_t* ud = (const volatile uint32_t*)r->ebx;
            if (!ud){ r->eax = (uint32_t)-14; return; }
            uint32_t base  = ud[1];
            uint32_t limit = ud[2];
            // Allocate a per-thread TLS descriptor from the pool so every
            // thread can own an independent TLS block.  Return the GS selector
            // directly to the guest (it loads `gs` with this value).
            int sel = gdt_alloc_tls(base, limit);
            if (sel < 0){ r->eax = (uint32_t)-1; return; }
            ((volatile uint32_t*)ud)[0] = (uint32_t)sel;  // selector for guest gs
            // Track it on the current thread so exit can free it.
            g_threads[g_cur].gs_sel  = (uint32_t)sel;
            g_threads[g_cur].tls_base = base;
            r->eax = 0;
            return;
        }
        case 140: // sys_llseek (return 0)
        case 141: // sys_getdents (no dir support yet)
            r->eax = 0;
            return;
        case 195: { // sys_stat64 (path in ebx, struct stat64* in ecx)
            volatile uint8_t* st = (volatile uint8_t*)r->ecx;
            if (!st){ r->eax = (uint32_t)-14; return; }
            for (int i = 0; i < 112; i++) st[i] = 0;
            *(volatile uint64_t*)(st + 8)  = 0;
            *(volatile uint32_t*)(st + 16) = 0x81A4;
            *(volatile uint32_t*)(st + 20) = 1;
            *(volatile uint64_t*)(st + 56) = 0;
            r->eax = 0;
            return;
        }
        case 196: { // sys_lstat64 (path in ebx, struct stat64* in ecx)
            volatile uint8_t* st = (volatile uint8_t*)r->ecx;
            if (!st){ r->eax = (uint32_t)-14; return; }
            for (int i = 0; i < 112; i++) st[i] = 0;
            *(volatile uint32_t*)(st + 16) = 0x81A4;
            *(volatile uint32_t*)(st + 20) = 1;
            r->eax = 0;
            return;
        }
        case 197: { // sys_fstat64 (fd in ebx, struct stat64* in ecx)
            volatile uint8_t* st = (volatile uint8_t*)r->ecx;
            if (!st){ r->eax = (uint32_t)-14; return; }
            for (int i = 0; i < 108; i++) st[i] = 0;
            *(volatile uint32_t*)(st + 16) = 0x81A4;
            *(volatile uint32_t*)(st + 20) = 1;
            r->eax = 0;
            return;
        }
        case 300: { // sys_fstatat64 (dirfd in ebx, path in ecx, stat in edx,
                    // flags in esi).  Report the same regular-file stat.
            volatile uint8_t* st = (volatile uint8_t*)r->edx;
            if (!st){ r->eax = (uint32_t)-14; return; }
            for (int i = 0; i < 112; i++) st[i] = 0;
            *(volatile uint32_t*)(st + 16) = 0x81A4;
            *(volatile uint32_t*)(st + 20) = 1;
            r->eax = 0;
            return;
        }
        case 405: { // sys_clock_gettime64 (clockid in ebx, timespec64* in ecx)
                // NOTE: renumbered 403 -> 405 to avoid colliding with the guest
                // TCP socket bridge's sys_recv (case 403). The 400-404 range is
                // reserved by usr/libc.h for nex_socket/connect/send/recv/close.
            volatile uint8_t* ts = (volatile uint8_t*)r->ecx;
            if (ts){
                for (int i = 0; i < 16; i++) ts[i] = 0;
                *(volatile uint64_t*)(ts + 0) = 1780000000ull;
                *(volatile uint64_t*)(ts + 8) = 0;
            }
            r->eax = 0;
            return;
        }
        case 146: { // sys_writev (fd in ebx, iovec* in ecx, count in edx)
            int fd = (int)r->ebx;
            const volatile uint32_t* iov = (const volatile uint32_t*)r->ecx;
            uint32_t cnt = r->edx;
            uint32_t total = 0;
            for (uint32_t i = 0; i < cnt && i < 64; i++){
                const char* base = (const char*)iov[i*2];
                uint32_t len = iov[i*2+1];
                if (fd == 1 || fd == 2){
                    for (uint32_t j = 0; j < len; j++) outb(0x3F8, (uint8_t)base[j]);
                }
                total += len;
            }
            r->eax = total;
            return;
        }
        // ---- Stage 2 signal syscalls ----
        case 173: { // sys_rt_sigreturn: restore context saved by a handler
            sys_linux_rt_sigreturn();
            __builtin_unreachable();
        }
        case 174: { // sys_rt_sigaction (ebx=signum, ecx=act*, edx=oldact*)
            int          sig = (int)r->ebx;
            const void*  act = (const void*)r->ecx;
            void*        old = (void*)r->edx;
            if (sig < LINUX_SIG_MIN || sig > LINUX_SIG_MAX){ r->eax = (uint32_t)-22; return; } // EINVAL
            if (old){
                volatile uint32_t* o = (volatile uint32_t*)old;
                o[0] = g_threads[g_cur].sa[sig].handler;
                o[1] = g_threads[g_cur].sa[sig].flags;
                o[2] = g_threads[g_cur].sa[sig].restorer;
                o[3] = g_threads[g_cur].sa[sig].mask;
            }
            if (act){
                volatile const uint32_t* a = (volatile const uint32_t*)act;
                g_threads[g_cur].sa[sig].handler  = a[0];
                g_threads[g_cur].sa[sig].flags   = a[1];
                g_threads[g_cur].sa[sig].restorer = a[2];
                g_threads[g_cur].sa[sig].mask    = a[3];
                // installing a handler clears any pending SIG_IGN'd signal
            }
            r->eax = 0;
            return;
        }
        case 175: { // sys_rt_sigprocmask (ebx=how, ecx=set*, edx=oldset*)
            int    how = (int)r->ebx;
            const uint32_t* set  = (const uint32_t*)r->ecx;
            uint32_t*       oldset = (uint32_t*)r->edx;
            if (oldset) *oldset = g_threads[g_cur].blocked;
            if (set){
                uint32_t nm = *set;
                if (how == 0)       g_threads[g_cur].blocked = nm;       // SIG_SETMASK
                else if (how == 1)  g_threads[g_cur].blocked |= nm;     // SIG_BLOCK
                else if (how == 2)  g_threads[g_cur].blocked &= ~nm;    // SIG_UNBLOCK
                // signal 9 (KILL) and 19 (STOP) cannot be blocked
                g_threads[g_cur].blocked &= ~(1u << 9);
                g_threads[g_cur].blocked &= ~(1u << 19);
            }
            r->eax = 0;
            // After changing the mask, attempt delivery of any now-unblocked
            // pending signal (e.g. SIGUSR2 blocked then unblocked).  If a
            // signal is delivered this does not return (switches to handler).
            linux_deliver_signals(r, 1);
            return;
        }
        case 238: { // sys_tkill (ebx=tid, ecx=signal)
            int tid = (int)r->ebx;
            int sig = (int)r->ecx;
            if (sig == 0){ r->eax = 0; return; } // poll
            if (sig < LINUX_SIG_MIN || sig > LINUX_SIG_MAX){ r->eax = (uint32_t)-22; return; }
            int idx = -1;
            for (int i = 0; i < LINUX_MAX_THREADS; i++)
                if (g_threads[i].state != THREAD_UNUSED && g_threads[i].tid == tid){ idx = i; break; }
            if (idx < 0){ r->eax = (uint32_t)-3; return; } // ESRCH
            g_threads[idx].pend |= (1u << sig);
            r->eax = 0;
            return;
        }
        case 270: { // sys_tgkill (ebx=tgid, ecx=tid, edx=signal)
            int tid = (int)r->ecx;
            int sig = (int)r->edx;
            if (sig == 0){ r->eax = 0; return; }
            if (sig < LINUX_SIG_MIN || sig > LINUX_SIG_MAX){ r->eax = (uint32_t)-22; return; }
            int idx = -1;
            for (int i = 0; i < LINUX_MAX_THREADS; i++)
                if (g_threads[i].state != THREAD_UNUSED && g_threads[i].tid == tid){ idx = i; break; }
            if (idx < 0){ r->eax = (uint32_t)-3; return; }
            g_threads[idx].pend |= (1u << sig);
            r->eax = 0;
            return;
        }
        case 192: // sys_mmap2 (mmap with page-offset; treat like old_mmap via regs)
            if (num == 192){
                // mmap2: ebx=addr ecx=len edx=prot esi=flags edi=fd ebp=off
                uint32_t len = r->ecx;
                uint32_t flags = r->esi;
#if LINUX_TRACE_SYSCALLS
                serial_puts("[mmap2 len=0x");
                { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
                  uint32_t v=len; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
                serial_puts(" fl=0x");
                { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
                  uint32_t v=flags; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
                serial_puts(" anon=");
                serial_putdec((int)((flags & 0x20) != 0));
                serial_puts("]\n");
#endif
                if ((flags & 0x20) == 0){ r->eax = (uint32_t)-1; return; }
                uint32_t prot = r->edx;
                uint32_t alloc = (len + 0xFFFu) & ~0xFFFu;
                if (flags & 0x10){
                    // MAP_FIXED: honour the requested address (page-aligned).
                    // busybox/glibc map TLS + initial heap at a fixed VA.
                    uint32_t want = r->ebx & ~0xFFFu;
                    if (want < 0x10000u || want >= 0x10000000u){ r->eax = (uint32_t)-1; return; }
                    linux_mmap_apply_prot(want, alloc, prot);
                    linux_mmap_region* slot = 0;
                    for (int i = 0; i < LINUX_MMAP_MAX; i++)
                        if (!g_mmap_regions[i].used){ slot = &g_mmap_regions[i]; break; }
                    if (slot){ slot->addr = want; slot->alen = alloc; slot->prot = prot; slot->flags = flags; slot->used = true; }
                    r->eax = want;
                } else {
                    if (alloc > g_mmap_cursor - MMAP_FLOOR){ r->eax = (uint32_t)-1; return; }
                    g_mmap_cursor -= alloc;
                    uint32_t base = g_mmap_cursor;
                    linux_mmap_apply_prot(base, alloc, prot);
                    linux_mmap_region* slot = 0;
                    for (int i = 0; i < LINUX_MMAP_MAX; i++)
                        if (!g_mmap_regions[i].used){ slot = &g_mmap_regions[i]; break; }
                    if (slot){ slot->addr = base; slot->alen = alloc; slot->prot = prot; slot->flags = flags; slot->used = true; }
                    r->eax = base;
                }
#if LINUX_TRACE_SYSCALLS
                serial_puts("[mmap2 -> 0x");
                { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
                  uint32_t v=r->eax; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
                serial_puts("]\n");
#endif
                return;
            }
            r->eax = 0;
            return;
        case 199: // sys_getuid32
            r->eax = 0;
            return;
        case 200: // sys_getgid32
        case 201: // sys_geteuid32
        case 202: // sys_getegid32
            r->eax = 0;
            return;
        case 205: { // sys_getgroups32 (size in ebx, grouplist in ecx)
            // Root has no supplementary groups; report 0.
            (void)r->ebx; (void)r->ecx;
            r->eax = 0;
            return;
        }
        case 258: // sys_set_tid_address: musl needs a valid return (0 ok)
            r->eax = 0;
            return;
        case 221: // sys_fdatasync (fd in ebx): no-op success
            r->eax = 0;
            return;
        case 383: // sys_statx: report ENOSYS (glibc/musl fall back to fstat)
            r->eax = (uint32_t)-38; // -ENOSYS
            return;
        case 183: { // sys_getcwd (buf in ebx, size in ecx)
            const char* cwd = "/";
            char* dst = (char*)r->ebx;
            uint32_t sz = r->ecx;
            if (dst && sz > 1){ dst[0] = '/'; dst[1] = 0; r->eax = 1; }
            else r->eax = (uint32_t)-1;
            (void)cwd;
            return;
        }
        case 220: // sys_getdents64 (no dir support)
            r->eax = 0;
            return;
        case 265: // sys_faccessat2 (dirfd in ebx, path in ecx, mode in edx,
                  // flags in esi): report success (root can access anything)
            r->eax = 0;
            return;
        case 116: { // sys_sysinfo (struct sysinfo* in ebx)
            // i386 struct sysinfo (64 bytes): uptime, loads[3], totalram,
            // freeram, sharedram, bufferram, totalswap, freeswap, procs,
            // totalhigh, freehigh, mem_unit.  Report a small plausible box.
            volatile uint32_t* si = (volatile uint32_t*)r->ebx;
            if (!si){ r->eax = (uint32_t)-14; return; }
            for (int i = 0; i < 16; i++) si[i] = 0;
            si[0]  = 60;                  // uptime (s)
            si[4]  = 128 * 1024;          // totalram (KiB)
            si[5]  = 96 * 1024;           // freeram
            si[6]  = 0;                   // sharedram
            si[7]  = 0;                   // bufferram
            si[8]  = 0;                   // totalswap
            si[9]  = 0;                   // freeswap
            si[10] = 1;                   // procs (16-bit at byte 40)
            si[13] = 1024;                // mem_unit (bytes)
            r->eax = 0;
            return;
        }
        case 239: { // sys_openat2 (dirfd in ebx, path in ecx, open_how* in edx)
            // open_how { flags, mode, resolve } in edx; treat like openat.
            const char* path = (const char*)r->ecx;
            if (!path){ r->eax = (uint32_t)-14; return; }
            if (path[0] == '/') path++;
            for (int i = 3; i < 8; i++){
                if (!g_linux_fd_open[i]){
                    int vfd = vfs_open(path, 0);
                    if (vfd < 0){ r->eax = (uint32_t)-2; return; }
                    g_linux_fd_open[i] = true;
                    g_linux_fd[i] = vfd;
                    r->eax = (uint32_t)i;
                    return;
                }
            }
            r->eax = (uint32_t)-24;
            return;
        }
        case 252: { // sys_exit_group: kill every thread, exit process
            sys_linux_exit_group((uint32_t)r->ebx);
            __builtin_unreachable();
        }
        case 295: { // sys_openat (dirfd in ebx, path in ecx, flags in edx)
            const char* path = (const char*)r->ecx;
            if (!path){ r->eax = (uint32_t)-14; return; }
            if (path[0] == '/') path++;
            for (int i = 3; i < 8; i++){
                if (!g_linux_fd_open[i]){
                    int vfd = vfs_open(path, 0);
                    if (vfd < 0){ r->eax = (uint32_t)-2; return; }
                    g_linux_fd_open[i] = true;
                    g_linux_fd[i] = vfd;
                    r->eax = (uint32_t)i;
                    return;
                }
            }
            r->eax = (uint32_t)-24;
            return;
        }
        case 410: { // sys_nexos_fb: query framebuffer info (guest writes pixels)
                // NOTE: renumbered from 400 -> 410 to avoid colliding with the
                // guest TCP socket bridge (usr/libc.h documents 400/401/402/403
                // as nex_socket/nex_connect/send/recv). Keep remote_desktop.h and
                // usr/mc_launcher.c's NEXOS_SYS_FB/INPUT in sync with this.
            struct NexosFBInfo* p = (struct NexosFBInfo*)(uintptr_t)r->ebx;
            if (p) nexos_fb_query(p);
            r->eax = 0;
            return;
        }
        case 411: { // sys_nexos_input: block until an input event arrives (renumbered 401 -> 411)
            struct NexosInput* p = (struct NexosInput*)(uintptr_t)r->ebx;
            nexos_input_wait(p);
            r->eax = 0;
            return;
        }
        // ---- Stage 1 threading syscalls ----
        case 120: { // sys_clone: create a new thread
            // i386-ish ABI (NexOS flavour):
            //   ebx = child_entry (void(*)(int))
            //   ecx = child_stack (TOP of the child's stack)
            //   edx = flags
            //   esi = arg
            //   ebp = tls_base  (only used when flags & CLONE_SETTLS)
            uint32_t child_entry = r->ebx;
            uint32_t child_stack = r->ecx;
            uint32_t flags       = r->edx;
            uint32_t tls_base    = r->ebp;
            if (!child_stack || !child_entry){ r->eax = (uint32_t)-22; return; } // -EINVAL
            int t = -1;
            for (int i = 1; i < LINUX_MAX_THREADS; i++)
                if (g_threads[i].state == THREAD_UNUSED){ t = i; break; }
            if (t < 0){ r->eax = (uint32_t)-11; return; } // -EAGAIN
            // Optional per-thread TLS.
            uint32_t gs = 0;
            if (flags & 0x20000u){ // CLONE_SETTLS
                int sel = gdt_alloc_tls(tls_base, 0xFFFFF);
                if (sel < 0){ r->eax = (uint32_t)-1; return; }
                gs = (uint32_t)sel;
            }
            // Build the child's initial trap frame just below child_stack.
            // On `iret` the child starts at child_entry with esp = child_stack
            // and eax = 0 (child tid).  The clone *arg* (esi) is delivered by
            // the guest, which places it at child_stack[4] before calling
            // clone; child_entry reads it as its cdecl argument.  The guest
            // must also place child_stack[0] = child_exit_trampoline so the
            // child returns into sys_exit(0) when child_entry returns.
            volatile uint32_t* f = (volatile uint32_t*)(child_stack - 44);
            f[0]  = child_stack - 40;   // SysRegs* slot -> points at eax slot
            f[1]  = 0;                  // eax (child tid)
            f[2]  = 0;                  // ebx
            f[3]  = 0;                  // ecx
            f[4]  = 0;                  // edx
            f[5]  = 0;                  // esi
            f[6]  = 0;                  // edi
            f[7]  = 0;                  // ebp
            f[8]  = child_entry;        // EIP
            f[9]  = 0x08;               // CS (guest runs ring 0)
            f[10] = ((volatile uint32_t*)r)[9]; // EFLAGS (copy parent's saved flags)
            g_threads[t].resume_esp = (uint32_t)(child_stack - 44);
            g_threads[t].gs_sel     = gs;
            g_threads[t].stack_top  = child_stack;
            g_threads[t].tls_base   = tls_base;
            // Signal dispositions (sa[]) and the blocked mask are process-wide
            // in Linux: a handler installed by any thread is visible to all
            // threads in the same thread group.  Copy them from the caller so
            // the child can receive signals with the same dispositions.
            for (int s = 0; s <= LINUX_SIG_MAX; s++)
                g_threads[t].sa[s] = g_threads[g_cur].sa[s];
            g_threads[t].blocked   = g_threads[g_cur].blocked;
            g_threads[t].tid        = ++g_next_tid;
            g_threads[t].state      = THREAD_RUNNABLE;
            g_live_threads++;
            r->eax = (uint32_t)g_threads[t].tid;   // parent returns child tid
            return;
        }
        case 158: { // sys_sched_yield: yield to the next runnable thread
            r->eax = 0;
            linux_do_switch(r);
            return;
        }
        case 224: { // sys_gettid
            r->eax = (uint32_t)g_threads[g_cur].tid;
            return;
        }
        case 240: { // sys_futex
            volatile uint32_t* uaddr = (volatile uint32_t*)r->ebx;
            uint32_t op  = r->ecx;
            uint32_t val = r->edx;
            int      n   = (int)r->edi;
            if (!uaddr){ r->eax = (uint32_t)-14; return; } // -EFAULT
            uint32_t cmd = op & 0xF;
            if (cmd == 0){ // FUTEX_WAIT
                if (*uaddr == val){
                    r->eax = 0;                 // returned value on wake
                    linux_futex_park((uint32_t)uaddr, r);
                } else {
                    r->eax = (uint32_t)-11;     // -EAGAIN (value changed)
                }
                return;
            } else if (cmd == 1){ // FUTEX_WAKE
                int w = 0;
                for (int i = 0; i < LINUX_FUTEX_MAX_WAIT; i++){
                    if (g_futex_wait[i].tid >= 0 && g_futex_wait[i].uaddr == (uint32_t)uaddr){
                        int tt = g_futex_wait[i].tid;
                        g_futex_wait[i].tid = -1;
                        if (g_threads[tt].state == THREAD_WAITING){
                            g_threads[tt].state = THREAD_RUNNABLE;
                            w++;
                            if (w >= n) break;
                        }
                    }
                }
                r->eax = (uint32_t)w;
                return;
            }
            r->eax = (uint32_t)-1; // -ENOSYS
            return;
        }
        default:
            // [DIAG] log unsupported syscalls so we can grow the set.
            serial_puts("linux: unsupported syscall ");
            serial_putdec((int)num);
            serial_puts("\n");
            r->eax = (uint32_t)-1; // -ENOSYS
            return;
    }
}

// ---- Stage 1 thread exit ------------------------------------------------
// Exit the calling thread.  If it is the last live thread (or the initial
// main thread), tear the whole process down via mini_longjmp back into
// linux_run(); otherwise mark it EXITED, free its TLS, and switch to the
// next runnable thread.
static void sys_linux_exit_thread(uint32_t code){
    int cur = g_cur;
    if (g_threads[cur].gs_sel){
        gdt_free_tls((int)g_threads[cur].gs_sel);
        g_threads[cur].gs_sel = 0;
    }
    g_threads[cur].state = THREAD_EXITED;
    g_live_threads--;
    if (cur == 0 || g_live_threads <= 0){
        g_linux_exit_code = (int)code;
        mini_longjmp(&g_ctx, 1);
    }
    // switch to another runnable thread
    int nx = -1;
    for (int i = 1; i <= LINUX_MAX_THREADS; i++){
        int idx = (cur + i) % LINUX_MAX_THREADS;
        if (g_threads[idx].state == THREAD_RUNNABLE){ nx = idx; break; }
    }
    if (nx < 0){ // deadlock guard: nothing to run but we're alive
        g_linux_exit_code = (int)code;
        mini_longjmp(&g_ctx, 1);
    }
    // deliver any pending signal to the thread we're about to resume
    linux_deliver_signals((SysRegs*)(g_threads[nx].resume_esp + 4), 0);
    g_threads[nx].state = THREAD_RUNNING;
    g_cur = nx;
    linux_switch_asm(g_threads[nx].resume_esp, g_threads[nx].gs_sel);
    __builtin_unreachable();
}

// Exit the whole process: mark every thread EXITED and unwind to linux_run().
static void sys_linux_exit_group(uint32_t code){
    for (int i = 0; i < LINUX_MAX_THREADS; i++){
        if (g_threads[i].gs_sel){
            gdt_free_tls((int)g_threads[i].gs_sel);
            g_threads[i].gs_sel = 0;
        }
        g_threads[i].state = THREAD_EXITED;
    }
    g_live_threads = 0;
    g_linux_exit_code = (int)code;
    mini_longjmp(&g_ctx, 1);
}

// =========================================================================
//  int 0x80 trap entry (C stub wraps the asm handler)
// =========================================================================
__asm__(
    ".global linux_syscall_entry\n"
    "linux_syscall_entry:\n"
    "    pushl %ebp\n"   // arg5  (pushed first -> highest address)
    "    pushl %edi\n"   // arg4
    "    pushl %esi\n"   // arg3
    "    pushl %edx\n"   // arg2
    "    pushl %ecx\n"   // arg1
    "    pushl %ebx\n"   // arg0
    "    pushl %eax\n"   // sysno  (pushed last -> lowest address == SysRegs.eax)
    "    movl %esp, %eax\n"
    "    pushl %eax\n"   // cdecl: pass SysRegs* as the first stack argument
    "    call linux_syscall_dispatch\n"
    // Stage 2: deliver any pending & unblocked signal for the current thread
    // BEFORE returning to the guest.  linux_deliver_signals() may rewrite the
    // trap frame and switch into a signal handler (it does not return); if
    // there is nothing to deliver it returns here and we iret as normal.
    "    movl 4(%esp), %eax\n"   // eax = SysRegs* (current trap frame)
    "    pushl $0\n"             // from_trap = 0 (dead-code path)
    "    pushl %eax\n"
    "    call linux_deliver_signals\n"
    "    addl $8, %esp\n"
    // Reload eax from the saved trap frame: dispatch wrote the syscall
    // return value into r->eax (the slot at [esp+4] == SysRegs* points at
    // it).  This is required so callers (e.g. clone's parent) receive the
    // kernel return value instead of whatever the dispatcher left in eax.
    "    movl 4(%esp), %eax\n"   // eax = SysRegs* (addr of saved eax slot)
    "    movl 0(%eax), %eax\n"   // eax = r->eax (syscall return value)
    "    addl $32, %esp\n"   // pop 7 registers + 1 argument
    "    iret\n"
);

// =========================================================================
//  ELF32 loader + execution
// =========================================================================

// Load an ELF32 image from SFS and map its PT_LOAD segments into the guest
// identity region.  Shared by linux_run() (first exec) and sys_execve.
// Returns 0 on success and fills *out_entry; -1 on any failure.
static int linux_load_image(const char* name, uint32_t* out_entry,
                            uint32_t* out_max_end, uint32_t* out_load_base,
                            uint32_t* out_phoff, uint16_t* out_phentsz,
                            uint16_t* out_phnum){
    if (!g_reader){ serial_puts("linux: no file reader registered\n"); return -1; }
    // Image buffer: real static Linux binaries (e.g. busybox) reach ~450 KiB.
    // Keep it out of the guest identity region (heap is 16 MiB at 0x200000;
    // this static buffer lives in the kernel .bss, fine).
    static unsigned char elf[2 * 1024 * 1024];
    int sz = g_reader(name, elf, (int)sizeof(elf));
    if (sz <= 0){ serial_puts("linux: file not found: "); serial_puts(name); serial_puts("\n"); return -1; }

    // ELF magic + class check
    if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F'){
        serial_puts("linux: not an ELF image\n"); return -1;
    }
    if (elf[4] != 1){ serial_puts("linux: not ELF32\n"); return -1; } // 1 = 32-bit

    uint32_t  entry    = *(uint32_t*)(elf + 24);          // e_entry
    uint32_t  phoff    = *(uint32_t*)(elf + 28);          // e_phoff
    uint16_t  phentsz  = *(uint16_t*)(elf + 42);          // e_phentsize
    uint16_t  phnum    = *(uint16_t*)(elf + 44);          // e_phnum
    if (phnum == 0 || phnum > 64){ serial_puts("linux: bad e_phnum, abort\n"); return -1; }

    serial_puts("linux: loading "); serial_puts(name);
    serial_puts(" entry=0x");
    {
        static const char* hexd = "0123456789ABCDEF";
        char hb[9]; int hi = 0;
        for (int s = 24; s >= 0; s -= 8){
            uint8_t b = (uint8_t)(entry >> s);
            hb[hi++] = hexd[(b >> 4) & 0xF]; hb[hi++] = hexd[b & 0xF];
        }
        hb[hi] = 0; serial_puts(hb);
    }
    serial_puts("\n");

    // Map PT_LOAD segments into the identity-mapped address space (< 256 MiB).
    uint32_t max_end = 0;   // highest vaddr+memsz across PT_LOAD segments
    uint32_t load_base = 0; // base of the first PT_LOAD (for AT_PHDR/auxv)
    for (int i = 0; i < phnum; i++){
        unsigned char* ph = elf + phoff + (uint32_t)i * phentsz;
        uint32_t p_type = *(uint32_t*)(ph + 0);
        if (p_type != 1) continue; // PT_LOAD == 1
        uint32_t p_offset = *(uint32_t*)(ph + 4);
        uint32_t p_vaddr  = *(uint32_t*)(ph + 8);
        uint32_t p_filesz = *(uint32_t*)(ph + 16);
        uint32_t p_memsz  = *(uint32_t*)(ph + 20);
        serial_puts("  PH"); serial_putdec(i); serial_puts(" vaddr=0x");
        { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
          uint32_t v=p_vaddr; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
        serial_puts(" off=0x");
        { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
          uint32_t v=p_offset; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
        serial_puts("\n");
        if (p_vaddr + p_memsz > 0x10000000){
            serial_puts("linux: segment above 256 MiB identity map\n"); return -1;
        }
        if (load_base == 0) load_base = p_vaddr - p_offset;
        for (uint32_t j = 0; j < p_filesz; j++)
            *(unsigned char*)(p_vaddr + j) = elf[p_offset + j];
        for (uint32_t j = p_filesz; j < p_memsz; j++)
            *(unsigned char*)(p_vaddr + j) = 0; // zero .bss
        if (p_vaddr + p_memsz > max_end) max_end = p_vaddr + p_memsz;
    }
    serial_puts("  entry bytes: ");
    for (int k = 0; k < 8; k++){
        uint8_t v = *(unsigned char*)(entry + k);
        static const char* h = "0123456789ABCDEF"; char b2[3];
        b2[0] = h[v >> 4]; b2[1] = h[v & 0xF]; b2[2] = 0; serial_puts(b2);
    }
    serial_puts("\n");

    *out_entry    = entry;
    *out_max_end  = max_end;
    *out_load_base= load_base;
    *out_phoff    = phoff;
    *out_phentsz  = phentsz;
    *out_phnum    = phnum;
    return 0;
}

// Build the i386 SysV startup stack below stack_top for argc/argv (envp is
// the fixed minimal set).  Returns the new esp (ptr_base) on success, 0 on
// failure.  Fills *out_str_base with the string region base.
static uint32_t linux_build_stack(int argc, const char** argv, int envc_arg,
                                  const char** envp, uint32_t stack_top,
                                  uint32_t max_end, uint32_t load_base,
                                  uint32_t phoff, uint16_t phentsz, uint16_t phnum,
                                  uint32_t entry, uint32_t* out_str_base,
                                  uint32_t at_base){
    (void)max_end;
    // ---- Build the i386 SysV startup stack below stack_top ----
    // Layout (Linux): [esp]=argc, then argv[0..argc-1] pointers, then a NULL
    // argv terminator, then a NULL envp.  Pointers are stored in guest memory
    // just under the top; strings are copied above the pointer array.
    if (argc < 0) argc = 0;
    if (argc > 32) argc = 32;
    if (!argv) argc = 0;
    // Total string bytes + NULs.
    uint32_t str_bytes = 0;
    for (int i = 0; i < argc; i++)
        if (argv[i]){ uint32_t n = 0; while (argv[i][n]) n++; str_bytes += n + 1; }

    // Choose the environment source:
    //   - execve() supplies its own envp (via envp != NULL) -> use it verbatim;
    //   - cmd_linux() has no envp concept -> fall back to a minimal default set
    //     so getenv("PATH")-style probes in busybox/glibc still work.
    const char** use_envp = envp;
    int envc = 0;
    if (use_envp){
        while (use_envp[envc]) envc++;
    } else {
        static const char* envp0[] = {
            "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
            "HOME=/root",
            "TERM=linux",
            "USER=root",
            "LOGNAME=root",
            "PWD=/",
            "SHELL=/bin/sh",
            0
        };
        use_envp = envp0;
        while (use_envp[envc]) envc++;
    }
    if (envc > 63) envc = 63;
    for (int e = 0; e < envc; e++){
        const char* s = use_envp[e]; uint32_t n = 0;
        while (s[n]) n++;
        str_bytes += n + 1;
    }
    if (str_bytes > 0x10000u){
        serial_puts("linux: argv too large\n"); return 0;
    }

    // Startup-stack layout (Linux i386 ABI):
    //   [esp] = argc
    //          argv[0..argc-1]
    //          NULL argv terminator
    //          envp[0..envc-1]
    //          NULL envp terminator
    //          auxv pairs {type,value}..., AT_NULL (0,0)
    // Strings live in a SEPARATE region well above the guest stack so the
    // guest's push/pop (which grows down from ptr_base) can never overwrite
    // them.  Use the 0x09000000 area (144 MiB, inside the 64-256 MiB PG_USER
    // identity map, far from both the image and the stack).
    const uint32_t STR_BASE  = 0x09000000u;
    const uint32_t STR_LIMIT = 0x0A000000u;  // 160 MiB top for strings
    // auxv vector (Linux constants):
    //   AT_NULL=0 AT_PHDR=3 AT_PHENT=4 AT_PHNUM=5 AT_PAGESZ=6 AT_BASE=7
    //   AT_FLAGS=8 AT_ENTRY=9 AT_UID=11 AT_EUID=12 AT_GID=13 AT_EGID=14
    //   AT_HWCAP=16 AT_CLKTCK=17 AT_SECURE=23 AT_RANDOM=25 AT_EXECFN=31
    static const int auxv_k[] = { 3,4,5,6,7,8,9,11,12,13,14,17,23,25,31 };
    const int NAUXV = (int)(sizeof(auxv_k)/sizeof(auxv_k[0]));
    // auxv values filled below (some need addresses from the string region).
    uint32_t auxv_v[16];

    uint32_t str_base = STR_BASE;
    uint32_t str_cur  = str_base;
    if (str_base + str_bytes > STR_LIMIT){
        serial_puts("linux: argv string region overflow\n"); return 0;
    }

    // Copy argv strings (record each guest pointer as we go).
    uint32_t argv_ptr[33];
    uint32_t argv0_str = str_base;                 // first argv string == exec name
    for (int i = 0; i < argc; i++){
        if (argv[i]){
            uint32_t n = 0; while (argv[i][n]) n++;
            for (uint32_t j = 0; j <= n; j++)
                *(unsigned char*)(str_cur + j) = (unsigned char)argv[i][j];
            argv_ptr[i] = str_cur;
            str_cur += n + 1;
        } else {
            argv_ptr[i] = 0;
        }
    }
    // Copy envp strings.
    uint32_t envp_ptr[64];
    for (int e = 0; e < envc; e++){
        const char* s = use_envp[e];
        uint32_t n = 0; while (s[n]) n++;
        for (uint32_t j = 0; j <= n; j++)
            *(unsigned char*)(str_cur + j) = (unsigned char)s[j];
        envp_ptr[e] = str_cur;
        str_cur += n + 1;
    }
    // 16 bytes of "random" data for AT_RANDOM (stack canary seed).
    uint32_t rand_addr = str_cur;
    for (int k = 0; k < 16; k++)
        *(unsigned char*)(str_cur + k) = (unsigned char)(0xA5 + k * 13);
    str_cur += 16;

    // auxv values.
    auxv_v[0] = load_base + phoff;      // AT_PHDR
    auxv_v[1] = phentsz;                // AT_PHENT
    auxv_v[2] = (uint32_t)phnum;        // AT_PHNUM
    auxv_v[3] = 4096;                   // AT_PAGESZ
    auxv_v[4] = at_base;                // AT_BASE (interpreter/libc.so load addr)
    auxv_v[5] = 0;                      // AT_FLAGS
    auxv_v[6] = entry;                  // AT_ENTRY
    auxv_v[7] = 0;                      // AT_UID
    auxv_v[8] = 0;                      // AT_EUID
    auxv_v[9] = 0;                      // AT_GID
    auxv_v[10] = 0;                     // AT_EGID
    auxv_v[11] = 100;                   // AT_CLKTCK
    auxv_v[12] = 0;                     // AT_SECURE
    auxv_v[13] = rand_addr;             // AT_RANDOM
    auxv_v[14] = argv0_str;             // AT_EXECFN

    // Stack pointer layout: argc + argv[] + NULL + envp[] + NULL + auxv[]*2 + NULL.
    uint32_t total_words = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1
                           + (uint32_t)NAUXV * 2 + 1;
    uint32_t ptr_base = (stack_top - total_words * 4u) & ~0xFu;  // 16-byte align
    uint32_t* ptr = (uint32_t*)ptr_base;
    uint32_t w = 0;
    ptr[w++] = (uint32_t)argc;           // argc at [esp]
    for (int i = 0; i < argc; i++)       // argv[]
        ptr[w++] = argv_ptr[i];
    ptr[w++] = 0;                        // argv NULL terminator
    for (int e = 0; e < envc; e++)       // envp[]
        ptr[w++] = envp_ptr[e];
    ptr[w++] = 0;                        // envp NULL terminator
    for (int i = 0; i < NAUXV; i++){     // auxv pairs
        ptr[w++] = (uint32_t)auxv_k[i];
        ptr[w++] = auxv_v[i];
    }
    ptr[w++] = 0;                        // AT_NULL type
    ptr[w++] = 0;                        // AT_NULL value

    if (out_str_base) *out_str_base = str_base;
    return ptr_base;
}

// =========================================================================
//  Minimal in-kernel ELF dynamic linker (Stage 6)
// -------------------------------------------------------------------------
//  When a guest ELF carries DT_NEEDED entries (a shared library such as
//  libc.so), the kernel itself acts as the dynamic linker: it maps each
//  needed .so into the guest address space, applies the R_386_* relocations
//  (RELATIVE for each .so's own GOT, GLOB_DAT/JMP_SLOT/32/PC32 for the
//  main's cross-object references), fills in AT_BASE, and transfers control.
//  No PT_INTERP / ld-linux is used -- we ARE the linker.
// =========================================================================
typedef struct {
    uint32_t p_type; uint32_t p_offset; uint32_t p_vaddr; uint32_t p_paddr;
    uint32_t p_filesz; uint32_t p_memsz; uint32_t p_flags; uint32_t p_align;
} Elf32_Phdr;
typedef struct {
    uint32_t st_name; uint32_t st_value; uint32_t st_size;
    unsigned char st_info; unsigned char st_other; uint16_t st_shndx;
} Elf32_Sym;
typedef struct { uint32_t r_offset; uint32_t r_info; } Elf32_Rel;

#define DYN_PT_LOAD     1
#define DYN_PT_DYNAMIC  2
#define DYN_DT_NULL     0
#define DYN_DT_NEEDED   1
#define DYN_DT_HASH     4
#define DYN_DT_STRTAB   5
#define DYN_DT_SYMTAB   6
#define DYN_DT_PLTRELSZ 2
#define DYN_DT_REL      17
#define DYN_DT_RELSZ    18
#define DYN_DT_JMPREL   23
#define DYN_R_NONE      0
#define DYN_R_32        1
#define DYN_R_PC32      2
#define DYN_R_GLOB_DAT  6
#define DYN_R_JMP_SLOT  7
#define DYN_R_RELATIVE  8

#define DYN_LIB_BASE  0x0A400000u   /* free guest region: above argv strings
                                     * (0x0A000000) and below the stack top
                                     * (0x0C000000) / mmap arena.            */
#define DYN_MAX_LIBS  8

typedef struct {
    uint32_t base;          /* load bias: runtime addr of the image's vaddr 0 */
    uint32_t max_end;       /* highest mapped vaddr+memsz + base              */
    uint32_t* symtab;       /* runtime ptr to .dynsym                         */
    uint32_t  nsyms;        /* dynsym entry count (from DT_HASH nchain)       */
    const char* strtab;     /* runtime ptr to .dynstr                         */
    uint32_t* rel;          /* .rel.dyn runtime ptr (may be 0)                */
    uint32_t  relsz;
    uint32_t* jmprel;       /* .rel.plt runtime ptr (may be 0)                */
    uint32_t  jmprelsz;
} dyn_mod_t;

static int dl_strcmp(const char* a, const char* b){
    while (*a && *a == *b){ a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/* Map PT_LOAD segments of an ELF buffer into guest memory at bias+p_vaddr.
 * Returns the highest mapped runtime address, or 0 on overflow / bad image. */
static uint32_t dyn_map_image(const unsigned char* elf, int sz, uint32_t bias){
    (void)sz;
    uint32_t phoff = *(uint32_t*)(elf + 28);
    uint16_t phentsz = *(uint16_t*)(elf + 42);
    uint16_t phnum = *(uint16_t*)(elf + 44);
    if (phnum == 0 || phnum > 64) return 0;
    uint32_t max_end = 0;
    for (int i = 0; i < phnum; i++){
        const unsigned char* ph = elf + phoff + (uint32_t)i * phentsz;
        if (*(uint32_t*)(ph + 0) != DYN_PT_LOAD) continue;
        uint32_t p_offset = *(uint32_t*)(ph + 4);
        uint32_t p_vaddr  = *(uint32_t*)(ph + 8);
        uint32_t p_filesz = *(uint32_t*)(ph + 16);
        uint32_t p_memsz  = *(uint32_t*)(ph + 20);
        if (bias + p_vaddr + p_memsz > 0x10000000u) return 0;
        for (uint32_t j = 0; j < p_filesz; j++)
            *(unsigned char*)(bias + p_vaddr + j) = elf[p_offset + j];
        for (uint32_t j = p_filesz; j < p_memsz; j++)
            *(unsigned char*)(bias + p_vaddr + j) = 0;
        uint32_t end = bias + p_vaddr + p_memsz;
        if (end > max_end) max_end = end;
    }
    return max_end;
}

/* Parse PT_DYNAMIC of a mapped image into *mod.  Returns 0 on success, -1 if
 * there is no PT_DYNAMIC.  DT entries are read from guest memory. */
static int dyn_parse(const unsigned char* elf, uint32_t bias, dyn_mod_t* mod){
    uint32_t phoff = *(uint32_t*)(elf + 28);
    uint16_t phentsz = *(uint16_t*)(elf + 42);
    uint16_t phnum = *(uint16_t*)(elf + 44);
    for (int i = 0; i < phnum; i++){
        const unsigned char* ph = elf + phoff + (uint32_t)i * phentsz;
        if (*(uint32_t*)(ph + 0) != DYN_PT_DYNAMIC) continue;
        uint32_t d_vaddr = *(uint32_t*)(ph + 8);
        const uint32_t* d = (const uint32_t*)(bias + d_vaddr);
        uint32_t dt_hash=0, dt_strtab=0, dt_symtab=0, dt_rel=0, dt_relsz=0,
                 dt_jmprel=0, dt_jmprelsz=0;
        for (int k = 0; d[2*k] != DYN_DT_NULL; k++){
            uint32_t t = d[2*k], v = d[2*k+1];
            switch (t){
                case DYN_DT_HASH:     dt_hash     = v; break;
                case DYN_DT_STRTAB:   dt_strtab   = v; break;
                case DYN_DT_SYMTAB:   dt_symtab   = v; break;
                case DYN_DT_PLTRELSZ: dt_jmprelsz = v; break;
                case DYN_DT_REL:      dt_rel      = v; break;
                case DYN_DT_RELSZ:    dt_relsz    = v; break;
                case DYN_DT_JMPREL:   dt_jmprel   = v; break;
                default: break;
            }
        }
        if (dt_symtab == 0 || dt_strtab == 0 || dt_hash == 0) return -1;
        mod->symtab   = (uint32_t*)(bias + dt_symtab);
        mod->strtab   = (const char*)(bias + dt_strtab);
        mod->rel      = (uint32_t*)(bias + dt_rel);
        mod->relsz    = dt_relsz;
        mod->jmprel   = (uint32_t*)(bias + dt_jmprel);
        mod->jmprelsz = dt_jmprelsz;
        const uint32_t* hash = (const uint32_t*)(bias + dt_hash);
        mod->nsyms = hash[1];   /* nchain == number of .dynsym entries */
        return 0;
    }
    return -1;
}

/* Resolve a symbol name to its runtime address across mods[0..nmods-1].
 * Returns 0 if not found. */
static uint32_t dyn_resolve(dyn_mod_t* mods, int nmods, const char* name){
    for (int m = 0; m < nmods; m++){
        dyn_mod_t* mod = &mods[m];
        for (uint32_t s = 0; s < mod->nsyms; s++){
            Elf32_Sym* sym = (Elf32_Sym*)((unsigned char*)mod->symtab + s * 16);
            if (sym->st_shndx == 0) continue;   /* undefined */
            const char* sn = mod->strtab + sym->st_name;
            if (dl_strcmp(sn, name) == 0) return mod->base + sym->st_value;
        }
    }
    return 0;
}

/* Apply one relocation entry of `mod` (resolving symbols against mods[]). */
static void dyn_apply_one(dyn_mod_t* mod, const uint32_t* relp,
                          dyn_mod_t* mods, int nmods){
    Elf32_Rel r;
    r.r_offset = relp[0];
    r.r_info   = relp[1];
    uint32_t sym  = r.r_info >> 8;
    uint32_t type = r.r_info & 0xff;
    uint32_t* slot = (uint32_t*)(mod->base + r.r_offset);
    const char* name = mod->strtab +
        ((Elf32_Sym*)((unsigned char*)mod->symtab + sym * 16))->st_name;
    uint32_t addr;
    switch (type){
        case DYN_R_NONE: break;
        case DYN_R_RELATIVE: *slot += mod->base; break;
        case DYN_R_GLOB_DAT:
        case DYN_R_JMP_SLOT:
            addr = dyn_resolve(mods, nmods, name);
            if (addr) *slot = addr;
            else { serial_puts("dyn: unresolved "); serial_puts(name); serial_puts("\n"); }
            break;
        case DYN_R_32:
            addr = dyn_resolve(mods, nmods, name);
            if (addr) *slot += addr;
            break;
        case DYN_R_PC32:
            addr = dyn_resolve(mods, nmods, name);
            if (addr) *slot = addr - (uint32_t)slot + *slot;  /* addend in slot */
            break;
        default: break;
    }
}

/* Apply all relocations of `mod` (both .rel.dyn and .rel.plt). */
static void dyn_apply(dyn_mod_t* mod, dyn_mod_t* mods, int nmods){
    uint32_t n = mod->relsz / 8;
    for (uint32_t i = 0; i < n; i++) dyn_apply_one(mod, mod->rel + i*2, mods, nmods);
    n = mod->jmprelsz / 8;
    for (uint32_t i = 0; i < n; i++) dyn_apply_one(mod, mod->jmprel + i*2, mods, nmods);
}

/* Load + dynamically link + run a guest ELF.
 * Returns 1 if the image is STATIC (caller should run it via the normal
 * static path), -1 on error, and DOES NOT RETURN (transfers control to the
 * guest) when the image is dynamic and successfully linked. */
static int linux_dynload_and_exec(const char* name, int argc, const char** argv,
                                  int envc, const char** envp, uint32_t stack_top){
    static unsigned char elf[2 * 1024 * 1024];
    int sz = g_reader(name, elf, (int)sizeof(elf));
    if (sz <= 0){ serial_puts("dyn: file not found: "); serial_puts(name); serial_puts("\n"); return -1; }
    if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F') return -1;
    if (elf[4] != 1) return -1;

    uint32_t entry   = *(uint32_t*)(elf + 24);
    uint32_t phoff   = *(uint32_t*)(elf + 28);
    uint16_t phentsz = *(uint16_t*)(elf + 42);
    uint16_t phnum   = *(uint16_t*)(elf + 44);

    /* Choose a load bias so the main's first PT_LOAD lands at 0x08048000
     * (the guest load region), whether the main is non-PIE (linked there
     * already) or PIE (linked at vaddr 0).  main_bias compensates so the
     * runtime addresses stay at 0x08048000+ for both forms. */
    uint32_t first_v = 0, first_o = 0;
    for (int i = 0; i < phnum; i++){
        const unsigned char* ph = elf + phoff + (uint32_t)i * phentsz;
        if (*(uint32_t*)(ph + 0) == DYN_PT_LOAD){ first_v = *(uint32_t*)(ph+8); first_o = *(uint32_t*)(ph+4); break; }
    }
    uint32_t main_bias = 0x08048000u - (first_v - first_o);

    uint32_t main_max = dyn_map_image(elf, sz, main_bias);
    if (main_max == 0){ serial_puts("dyn: main map fail\n"); return -1; }

    dyn_mod_t mods[DYN_MAX_LIBS + 1];
    if (dyn_parse(elf, main_bias, &mods[0]) < 0) return 1;   /* no PT_DYNAMIC -> static */
    mods[0].base = main_bias;

    /* Collect DT_NEEDED library names from the main's dynamic string table. */
    uint32_t d_vaddr = 0;
    for (int i = 0; i < phnum; i++){
        const unsigned char* ph = elf + phoff + (uint32_t)i * phentsz;
        if (*(uint32_t*)(ph + 0) == DYN_PT_DYNAMIC){ d_vaddr = *(uint32_t*)(ph + 8); break; }
    }
    const uint32_t* d = (const uint32_t*)(main_bias + d_vaddr);
    const char* needed[DYN_MAX_LIBS];
    int nneeded = 0;
    for (int k = 0; d[2*k] != DYN_DT_NULL; k++){
        if (d[2*k] == DYN_DT_NEEDED){
            if (nneeded >= DYN_MAX_LIBS) break;
            needed[nneeded++] = mods[0].strtab + d[2*k+1];
        }
    }
    if (nneeded == 0) return 1;   /* dynamic info but no libs -> treat static */

    /* Load + link each needed library. */
    uint32_t lib_base = DYN_LIB_BASE;
    int nmods = 1;
    for (int i = 0; i < nneeded; i++){
        if (nmods >= DYN_MAX_LIBS + 1){ serial_puts("dyn: too many libs\n"); return -1; }
        static unsigned char lelf[2 * 1024 * 1024];
        int ls = g_reader(needed[i], lelf, (int)sizeof(lelf));
        if (ls <= 0){ serial_puts("dyn: lib not found: "); serial_puts(needed[i]); serial_puts("\n"); return -1; }
        if (lelf[0]!=0x7F||lelf[1]!='E'||lelf[2]!='L'||lelf[3]!='F'){
            serial_puts("dyn: lib not ELF: "); serial_puts(needed[i]); serial_puts("\n"); return -1;
        }
        uint32_t lmax = dyn_map_image(lelf, ls, lib_base);
        if (lmax == 0){ serial_puts("dyn: lib map fail\n"); return -1; }
        dyn_mod_t* lib = &mods[nmods];
        lib->base = lib_base;
        if (dyn_parse(lelf, lib_base, lib) < 0){
            serial_puts("dyn: lib no PT_DYNAMIC: "); serial_puts(needed[i]); serial_puts("\n"); return -1;
        }
        /* Apply the .so's own relocations (RELATIVE GOT + internal GLOB_DAT)
         * with the lib itself included in the symbol search set. */
        dyn_apply(lib, mods, nmods + 1);
        serial_puts("dyn: loaded "); serial_puts(needed[i]); serial_puts(" at 0x");
        { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
          uint32_t v=lib_base; for(int s=24;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
        serial_puts("\n");
        nmods++;
        lib_base = (lmax + 0xFFFu) & ~0xFFFu;
        if (lib_base >= 0x0C000000u){ serial_puts("dyn: lib region overflow\n"); return -1; }
    }

    /* Resolve the main's cross-object relocations against the loaded libs. */
    dyn_apply(&mods[0], mods, nmods);

    /* Compute phdr load base (runtime address of the first PT_LOAD). */
    uint32_t load_base = main_bias + (first_v - first_o);

    uint32_t at_base = mods[1].base;   /* first lib == interpreter base */
    uint32_t str_base_unused = 0;
    uint32_t entry_rt = entry + main_bias;   /* runtime entry (PIE: link off + bias) */
    g_guest_stack = linux_build_stack(argc, argv, envc, envp, stack_top,
                                      main_max, load_base, phoff, phentsz, phnum,
                                      entry_rt, &str_base_unused, at_base);
    g_guest_entry = entry_rt;
    serial_puts("dyn: transferring control to dynamic executable\n");
    __asm__ __volatile__(
        "mov %0, %%edx\n"
        "mov %1, %%eax\n"
        "mov %%edx, %%esp\n"
        "jmp *%%eax\n"
        :: "m"(g_guest_stack), "m"(g_guest_entry)
        : "eax", "edx", "memory");
    __builtin_unreachable();
}

// Full load-and-run entry point (used by cmd_linux).
int linux_run(const char* name, int argc, const char** argv){
    uint32_t entry = 0, max_end = 0, load_base = 0, phoff = 0;
    uint16_t phentsz = 0, phnum = 0;
    if (linux_load_image(name, &entry, &max_end, &load_base, &phoff,
                         &phentsz, &phnum) < 0) return -1;

    // Guest address-space layout (all inside the 64-256 MiB PG_USER map):
    //   ELF image    0x08048000 .. max_end (~0x0814C000 for busybox)
    //   brk heap     0x0814C000 .. 0x09000000 (144 MiB cap: below argv strs)
    //   argv strings 0x09000000 .. 0x0A000000
    //   guest stack  top 0x0C000000 (192 MiB), grows DOWN toward strings
    //   mmap arena   0x0C100000 .. 0x0FFFF000 (cursor grows down, ABOVE the
    //                stack so arena and stack never collide; ~63 MiB clean)
    // IMPORTANT: the stack MUST NOT sit right above the ELF image -- that
    // would leave only ~4 KiB of brk heap (glibc malloc fails and hits a
    // chunk-validate hlt as busybox pwd does).  192 MiB is still well clear.
    uint32_t stack_top = 0x0C000000u;
    if (stack_top >= 0x10000000u){
        serial_puts("linux: no room for guest stack in 256 MiB map\n");
        return -1;
    }

    // Reset per-guest heap/mmap state for this run.
    g_linux_stack_top = stack_top;
    g_linux_brk_base  = (max_end + 0xFFFu) & ~0xFFFu;  // end of data, page-aligned
    if (g_linux_brk_base > 0x09000000u) g_linux_brk_base = 0x09000000u;
    g_linux_brk       = g_linux_brk_base;
    linux_mmap_reset();

    // Reset the Linux thread table for this run (Stage 1).  TCB 0 is the
    // initial/main thread; it is RUNNING and owns tid 1.
    for (int i = 0; i < LINUX_MAX_THREADS; i++){
        g_threads[i].state     = THREAD_UNUSED;
        g_threads[i].gs_sel    = 0;
        g_threads[i].resume_esp = 0;
        g_threads[i].tid       = 0;
        g_threads[i].blocked   = 0;
        g_threads[i].pend      = 0;
        g_threads[i].sigframe  = linux_sigframe{};
        g_threads[i].saved_resume = 0;
        for (int s = 0; s <= LINUX_SIG_MAX; s++){
            g_threads[i].sa[s].handler  = 0;   // SIG_DFL
            g_threads[i].sa[s].flags    = 0;
            g_threads[i].sa[s].restorer = 0;
            g_threads[i].sa[s].mask     = 0;
        }
    }
    for (int i = 0; i < LINUX_FUTEX_MAX_WAIT; i++) g_futex_wait[i].tid = -1;
    g_cur = 0;
    g_live_threads = 1;
    g_next_tid = 1;
    g_threads[0].tid       = 1;
    g_threads[0].state     = THREAD_RUNNING;
    g_threads[0].gs_sel    = 0;
    g_threads[0].stack_top = stack_top;
    g_threads[0].resume_esp = 0;

    // Capture the kernel resume point. On a guest sys_exit, mini_longjmp()
    // unwinds back here with a nonzero value and we return cleanly.
    if (mini_setjmp(&g_ctx) != 0){
        serial_puts("linux: process exited\n");
        return g_linux_exit_code;
    }

    // If the image is dynamically linked, link it now (this maps the libs,
    // applies relocations, and transfers control -- it returns only if the
    // image is static or on error).
    int dr = linux_dynload_and_exec(name, argc, argv, 0, (const char**)0, stack_top);
    if (dr < 0) return -1;
    if (dr == 1) {
        // Static image: build the startup stack and jump to the entry point.
        // The values go through static slots (absolute addressing in the asm)
        // so GCC cannot park them in a callee-saved register that
        // mini_setjmp's naked asm (or the intervening code) clobbers.
        uint32_t str_base_unused = 0;
        g_guest_stack = linux_build_stack(argc, argv, 0, (const char**)0, stack_top,
                                          max_end, load_base, phoff, phentsz, phnum,
                                          entry, &str_base_unused, 0);
        g_guest_entry = entry;
        __asm__ __volatile__(
            "mov %0, %%edx\n"
            "mov %1, %%eax\n"
            "mov %%edx, %%esp\n"
            "jmp *%%eax\n"
            :: "m"(g_guest_stack), "m"(g_guest_entry)
            : "eax", "edx", "memory");
        __builtin_unreachable();
    }
    // dr == 0 is unreachable: the dynamic path jumped and never returns.
    return -1;
}

void linux_compat_init(int (*reader)(const char*, unsigned char*, int)){
    g_reader = reader;
}
