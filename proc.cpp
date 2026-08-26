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

    p->pid   = (uint32_t)g_proc_count + 1;   // pids start at 1
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
