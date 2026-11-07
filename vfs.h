#ifndef NexOS_VFS_H
#define NexOS_VFS_H

// =====================================================================
//  vfs.h  -  Minimal virtual filesystem with per-process sandbox roots
// ---------------------------------------------------------------------
//  Foundation 0 substrate for the security architecture (doc v1.0 3.2):
//  every file access made by a ring-3 process is resolved against that
//  process's root_path and passed through vfs_check_access() before the
//  physical backend is touched.  The Y/N permission popup (3.3) and the
//  deception engine (3.6) hook into that single choke point later.
// =====================================================================

#include <stdint.h>

#define VFS_MAX_FD     8
#define VFS_PATH_MAX   192
#define VFS_FILE_CACHE 2048          // per-fd payload cache (SFS files are small)

// ---- writable medium (RAMFS) ----
// SFS is a read-only boot medium, so writes need a second, writable layer.
// RAMFS is a small in-memory tree that is consulted FIRST for read/write;
// anything it does not own falls through to SFS (read-only).
#define VFS_RAMFS_MAX      16        // concurrent ramfs files
#define VFS_RAMFS_CAP      4096      // bytes per ramfs file
#define VFS_RAMFS_NAME_MAX 48        // flat name (mirrors SFS's flat namespace)

// Access intent passed to vfs_check_access().
#define VFS_ACC_READ   0x1
#define VFS_ACC_WRITE  0x2

// Access decision (returned by vfs_check_access).
#define VFS_DENY       0
#define VFS_ALLOW      1

// Backend hooks into the physical filesystem (SFS lives inside kernel.cpp,
// so it is injected the same way linux_compat_init() takes its reader).
typedef int (*vfs_read_fn)(const char* name, unsigned char* buf, int bufsize);
typedef int (*vfs_size_fn)(const char* name);

void vfs_init(vfs_read_fn rd, vfs_size_fn sz);

// Normalise `path` (handles \, //, . and ..) and resolve it against the
// current process's root.  Absolute paths address the global namespace;
// relative paths are joined onto root_path.  Returns 0 on success.
int  vfs_resolve(const char* path, char* out, int outsz);

// The single security choke point.  Returns VFS_ALLOW / VFS_DENY and logs
// the decision to the serial console.
int  vfs_check_access(const char* abspath, int mode);

int  vfs_open(const char* path, int flags);
int  vfs_read(int fd, void* buf, int count);
int  vfs_write(int fd, const void* buf, int count);
int  vfs_close(int fd);

// Create a file and return a writable fd, or -1 on failure.
// Routes to the writable RAMFS layer, so this succeeds even though the
// SFS boot medium is read-only.
int  vfs_create(const char* path, int flags);

// ---- writable-medium introspection (used by tests and vfs_dump) ----
// Returns 1 when the path currently lives in the writable layer.
int  vfs_is_writable(const char* path);

// Debug helper: print mount state + the calling process's sandbox root.
void vfs_dump(void);

#endif // NexOS_VFS_H
