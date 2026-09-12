// =====================================================================
//  proc.cpp  -  Minimal process table (Foundation 0)
// ---------------------------------------------------------------------
//  A flat array of Process records plus the "current" pointer. The kernel
//  itself is process 0 (SYSTEM_UID, ring 0). Real scheduling/context
//  switching is a later milestone; for now enter_user() sets g_current
//  to a user Process and restores it on exit.
// =====================================================================

#include "proc.h"
#include <stdint.h>

static Process g_procs[16];
static int     g_proc_count = 0;
// Monotonic pid source.  Deriving the pid from the array index reused ids once
// proc_kill() compacted the table (kill pid 1 of {1,2,3} -> {2,3}, next spawn
// got 3 again), so `kill <pid>` could terminate the wrong process.
static uint32_t g_next_pid = 1;

// Kernel / system process (pid 0).
static Process g_kernel_proc = { 0, "kernel", SYSTEM_UID, 0, "/", 0, 0x01800000 };

Process* g_current = &g_kernel_proc;

extern "C" int proc_spawn(const char* name, uint32_t uid, const char* root_path, Process** out){
    if (g_proc_count >= 16) return -1;
    Process* p = &g_procs[g_proc_count];

    // Zero the variable-length fields, then copy name + root_path by hand
    // (no libc in the freestanding kernel).
    for (int i = 0; i < 64; i++) p->name[i] = 0;
    for (int i = 0; i < 256; i++) p->root_path[i] = 0;

    int n = 0;
    while (name[n] && n < 63){ p->name[n] = name[n]; n++; }
    p->name[n] = 0;

    p->pid   = g_next_pid++;                 // never reused, even after kill
    p->uid   = uid;
    p->ring  = 0;                            // enter_user() flips to 3
    p->entry = 0;
    p->brk   = 0x09000000;                   // user break (within user region)

    int r = 0;
    while (root_path[r] && r < 255){ p->root_path[r] = root_path[r]; r++; }
    p->root_path[r] = 0;

    g_proc_count++;
    *out = p;
    return 0;
}

// ---------------------------------------------------------------------
//  proc_list / proc_kill  -- referenced by the text-shell `ps` / `kill`
//  commands (kernel.cpp, _kp.cpp).  Foundation 0 has no real scheduler,
//  so these just walk the flat table.
// ---------------------------------------------------------------------
static void proc_puts(char* buf, int& n, int bufsz, const char* s) {
    while (*s && n < bufsz - 1) buf[n++] = *s++;
}
// Render v into buf, never writing past bufsz-1.  Also handles negatives:
// uid is uint32_t, so casting it to int made large uids negative and produced
// non-digit characters (and overflowed outright on INT_MIN).
static void proc_dec(char* buf, int& n, int bufsz, int v) {
    if (bufsz <= 0) return;
    uint32_t u;
    if (v < 0) {
        if (n < bufsz - 1) buf[n++] = '-';
        u = (uint32_t)0 - (uint32_t)v;   // -v without signed overflow
    } else {
        u = (uint32_t)v;
    }
    if (u == 0) { if (n < bufsz - 1) buf[n++] = '0'; return; }
    char rev[16]; int k = 0;
    while (u && k < 16) { rev[k++] = (char)('0' + (int)(u % 10)); u /= 10; }
    while (k) { if (n >= bufsz - 1) break; buf[n++] = rev[--k]; }
}

extern "C" int proc_list(char* buf, int bufsz) {
    int n = 0;
    proc_puts(buf, n, bufsz, "PID  NAME            UID  RING\n");
    proc_dec(buf, n, bufsz, (int)g_kernel_proc.pid); proc_puts(buf, n, bufsz, "    kernel          0    0\n");
    for (int i = 0; i < g_proc_count; i++) {
        proc_dec(buf, n, bufsz, (int)g_procs[i].pid);
        proc_puts(buf, n, bufsz, "    ");
        proc_puts(buf, n, bufsz, g_procs[i].name);
        // pad name to 16 (manual length, freestanding build has no strlen)
        int nl = 0; while (g_procs[i].name[nl]) nl++;
        for (int p = 0; p < 16 - nl; p++) proc_puts(buf, n, bufsz, " ");
        proc_dec(buf, n, bufsz, (int)g_procs[i].uid);  proc_puts(buf, n, bufsz, "    ");
        proc_dec(buf, n, bufsz, (int)g_procs[i].ring); proc_puts(buf, n, bufsz, "\n");
    }
    if (n >= bufsz) n = bufsz - 1;   // keep the NUL terminator inside the buffer
    buf[n] = 0;
    return n;
}

extern "C" int proc_kill(uint32_t pid) {
    for (int i = 0; i < g_proc_count; i++) {
        if (g_procs[i].pid == pid) {
            for (int j = i; j < g_proc_count - 1; j++) g_procs[j] = g_procs[j + 1];
            g_proc_count--;
            return 0;
        }
    }
    return -1;
}
