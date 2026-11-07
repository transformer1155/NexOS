// vfs_host_shim.cpp — host-side support so the REAL NexOS vfs.cpp can be
// exercised off-kernel.
//
// vfs.cpp is freestanding kernel code: it calls outb() (privileged), reads
// g_current (proc.h) and consults perm_* (perm.h).  Rather than reimplement
// it, we supply those three things and link the genuine translation unit:
//
//     g++ -DVFS_HOST_TEST -c vfs.cpp        <- the real logic
//     g++ -c vfs_host_shim.cpp              <- this file: proc/perm/backend
//
// -DVFS_HOST_TEST only swaps `outb` for stdout; every VFS decision
// (sandbox choke point, RAMFS overlay, EROFS protection) is untouched.

#include <cstdio>
#include <cstring>
#include <cstdint>
#include "vfs.h"
#include "proc.h"      // the REAL Process layout -- do not re-declare it

// ---- the current process pointer vfs.cpp expects ----
Process  g_host_proc;
Process* g_current = &g_host_proc;

// ---- SFS backend, faked with one read-only file ----
static const char* kSfsName = "SEED.TXT";
static const char* kSfsBody = "sfs-readonly-content";

static int host_read(const char* name, unsigned char* buf, int bufsize) {
    if (strcmp(name, kSfsName) != 0) return -1;
    int n = (int)strlen(kSfsBody);
    if (n > bufsize) n = bufsize;
    memcpy(buf, kSfsBody, n);
    return n;
}
static int host_size(const char* name) {
    return strcmp(name, kSfsName) == 0 ? (int)strlen(kSfsBody) : -1;
}

// ---- perms: allow everything, so tests exercise the MEDIUM not the sandbox ----
int perm_classify(const char* p, const char* r) { (void)p; (void)r; return 0; }
int perm_request(int cls, const char* p, int w) { (void)cls; (void)p; (void)w; return 1; }

// ---- helpers for the DOS test harness ----
extern "C" void host_vfs_init(void) { vfs_init(host_read, host_size); }

extern "C" void host_set_identity(int pid, int uid, int ring, const char* root) {
    g_host_proc.pid  = (uint32_t)pid;
    g_host_proc.uid  = (uint32_t)uid;
    g_host_proc.ring = (uint32_t)ring;
    strncpy(g_host_proc.name, "test", sizeof(g_host_proc.name) - 1);
    strncpy(g_host_proc.root_path, root, sizeof(g_host_proc.root_path) - 1);
    g_host_proc.root_path[sizeof(g_host_proc.root_path) - 1] = 0;
}

// Seed a read-only SFS-visible file so DOS read tests have something to open.
extern "C" void host_seed_sfs(const char* name, const char* body) {
    (void)name; (void)body;   // the fake backend ships exactly one file
}