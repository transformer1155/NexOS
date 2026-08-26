#ifndef NexOS_PROC_H
#define NexOS_PROC_H

#include <stdint.h>

// UID of the system/kernel itself. Anything running as SYSTEM_UID bypasses
// the sandbox/VFS access checks (see security model v1.0, 3.2).
#define SYSTEM_UID 0

// A process (real user-mode task once ring-3 isolation lands).
// Foundation 0 uses a single flat table; a real scheduler is later.
struct Process {
    uint32_t pid;            // 0 = kernel, 1..N = user
    char     name[64];
    uint32_t uid;            // 0 = system, else app uid
    uint32_t ring;           // 0 = kernel/ring-0, 3 = user
    char     root_path[256]; // per-process VFS root (sandbox, 3.2)
    uint32_t entry;          // entry point (info)
    uint32_t brk;            // current break (heap top) for sys_brk
};

// The currently executing process. Defaults to the kernel process.
extern Process* g_current;

// Allocate/register a process. Sets pid, copies name + root_path.
// Returns 0 on success, -1 if the table is full.
// extern "C": also called from C-compiled translation units (e.g. linux_compat).
extern "C" int proc_spawn(const char* name, uint32_t uid, const char* root_path, Process** out);

#endif // NexOS_PROC_H
