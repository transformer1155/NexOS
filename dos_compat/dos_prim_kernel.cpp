// =====================================================================
//  dos_prim_kernel.cpp  -  DOS primitives backed by real NexOS services
// ---------------------------------------------------------------------
//  This is the KERNEL build of dos_prim.h.  It reuses the kernel's own
//  mechanisms rather than inventing parallel ones:
//
//    * console output -> the COM1 port (0x3F8), same as linux_compat.cpp
//    * program exit   -> sys_exit_current(), i.e. the SAME unwind path
//                        SYS_EXIT uses (mini_longjmp into enter_user's
//                        checkpoint)
//
//  Why the exit path matters
//  -------------------------
//  enter_user() enters ring 3 with a hand-built iret frame and a
//  mini_setjmp checkpoint held privately inside syscall.cpp.  A .COM
//  program terminating via INT 21h AH=4Ch must unwind through that exact
//  checkpoint; anything else leaves the kernel's saved context dangling
//  and g_current stuck at ring 3.
//
//  So dos_process_exit() does NOT longjmp itself -- it delegates to the
//  exported sys_exit_current(), which owns the static g_user_ctx.
//  This is also what lets a .COM program's trailing RET land cleanly:
//  the DOS loader pushes the exit stub as the return address, so RET and
//  AH=4Ch converge on the same termination path.
// =====================================================================

#include "dos_prim.h"
#include "proc.h"
#include "vfs.h"

#include <stdint.h>

// Owned by syscall.cpp; exported specifically for non-syscall guests.
extern "C" int sys_exit_current(int code);
extern "C" int sys_last_exit_code(void);

// ---- serial console (mirrors linux_compat.cpp) ----
static inline void dprim_outb(uint16_t p, uint8_t v){
    __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p));
}
static void dprim_serial_puts(const char* s){ while (*s) dprim_outb(0x3F8, (uint8_t)*s++); }

// ---------------------------------------------------------------------
//  console
// ---------------------------------------------------------------------
// A sink lets the kernel mirror DOS output onto the on-screen terminal.
// The serial port is ALWAYS driven, so the two are additive: serial for
// headless/QEMU runs, GUI for interactive VBox use.
static dos_console_sink_fn g_sink = 0;
static char g_sinkbuf[128];

void dos_console_set_sink(dos_console_sink_fn fn){ g_sink = fn; }

// Emit one character to the sink, buffering until a newline or until the
// caller flushes.  term.write() wants a NUL-terminated string, so we must
// not hand it one byte at a time.
static void sink_emit(int ch){
    static int n = 0;
    if (!g_sink) return;
    if (ch == '\n' || n >= (int)sizeof(g_sinkbuf) - 1){
        g_sinkbuf[n] = 0;
        if (n) g_sink(g_sinkbuf);
        n = 0;
        if (ch != '\n' && n < (int)sizeof(g_sinkbuf) - 1) g_sinkbuf[n++] = (char)ch;
        return;
    }
    g_sinkbuf[n++] = (char)ch;
}

// Push out any partially-buffered sink text.  Called by the exit path so a
// final line without a trailing newline is not swallowed.
static void sink_flush(void){
    sink_emit('\n');
}

int dos_console_putc(int ch){
    dprim_outb(0x3F8, (uint8_t)ch);   // always: serial
    sink_emit(ch);                    // plus: on-screen if installed
    return 0;
}

int dos_console_puts(const char *s){
    if (!s) return -1;
    dprim_serial_puts(s);             // always: serial
    for (const char* p = s; *p; p++) sink_emit((unsigned char)*p);
    return 0;
}

int dos_console_getc(void){
    // No ring-3 input path yet (the PIC stays masked with IF=0 inside
    // enter_user).  Report "nothing available" rather than fabricating a
    // character, so AH=01h callers can detect the condition honestly.
    return -1;
}

// ---------------------------------------------------------------------
//  process termination
// ---------------------------------------------------------------------
int dos_process_exit(int code){
    // Flush any buffered sink text before unwinding, so a .COM program
    // whose last line lacks a trailing newline still appears on screen.
    sink_flush();
    // Delegates to the kernel's own exit path.  Does not return when a
    // ring-3 program is active; returns `code` otherwise (host/testing).
    return sys_exit_current(code);
}

int dos_process_exit_code(void){
    return sys_last_exit_code();
}