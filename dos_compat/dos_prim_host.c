// dos_prim_host.c — host-side implementation of the DOS primitives.
//
// IMPORTANT: this file no longer carries a VFS stub.  Earlier it did, purely
// to get tests running before NexOS had a writable medium; that stub then
// became a liability, because it could pass while the real VFS failed.
// The DOS tests now link the REAL vfs.cpp (built with -DVFS_HOST_TEST, which
// swaps only the privileged `outb` for stdout).  What is exercised here is
// therefore the same code the kernel runs.

#include "dos_prim.h"
#include "vfs.h"

#include <stdio.h>

// ---- console output (mirrors serial_putc / serial_puts) ----
// The host build has no GUI terminal, so the sink is accepted and ignored;
// stdout already serves the same purpose as COM1 in the kernel build.
static dos_console_sink_fn g_sink = 0;

void dos_console_set_sink(dos_console_sink_fn fn) {
    g_sink = fn;
    (void)g_sink;
}

int dos_console_putc(int ch) {
    fputc(ch, stdout);
    fflush(stdout);
    if (g_sink) { char b[2]; b[0] = (char)ch; b[1] = 0; g_sink(b); }
    return 0;
}

int dos_console_puts(const char *s) {
    if (!s) return -1;
    fputs(s, stdout);
    fflush(stdout);
    if (g_sink) g_sink(s);
    return 0;
}

// ---- console input ----
int dos_console_getc(void) {
    int c = fgetc(stdin);
    if (c == EOF) return -1;
    return c & 0xFF;
}

// ---- process termination ----
// The host build has no ring-3 unwind target, so this records the code and
// returns; the kernel build (dos_prim_kernel.cpp) unwinds instead.
static int g_exit_code = 0;

int dos_process_exit(int code) {
    g_exit_code = code;
    return 0;
}

int dos_process_exit_code(void) {
    return g_exit_code;
}