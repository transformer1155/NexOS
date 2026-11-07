// =====================================================================
//  vfs.cpp  -  Minimal virtual filesystem + per-process sandbox
// ---------------------------------------------------------------------
//  Namespace model
//  ---------------
//  The VFS exposes a single global tree ("/apps/<app>", "/system", ...).
//  Each Process carries a root_path; that root is the sandbox boundary:
//
//    * relative paths  ->  joined onto root_path
//    * absolute paths  ->  taken literally, then checked against root_path
//    * ".." is collapsed BEFORE the check, so "../../system/passwd" can
//      not be used to climb out.
//
//  Backing store
//  -------------
//  The physical filesystem is SFS, which is a FLAT 20-char namespace with
//  no directories.  The VFS therefore maps a resolved virtual path onto
//  its basename.  The security decision is made on the FULL virtual path,
//  which is the part that matters for the sandbox; giving SFS real
//  directories is an orthogonal (and later) piece of work.
// =====================================================================

#include "vfs.h"
#include "proc.h"
#include "perm.h"
#include <stdint.h>

// ---- serial ----
// `outb` writes to the COM1 data port, which is a PRIVILEGED I/O
// instruction: it faults immediately in userspace.  VFS_HOST_TEST lets the
// host harness (dos_compat/test_vfs_ramfs.sh) exercise this exact file
// without the kernel, while the real kernel build is unchanged.
#ifdef VFS_HOST_TEST
  #include <cstdio>
  static inline void outb(uint16_t, uint8_t v){ fputc((int)v, stdout); fflush(stdout); }
#else
  static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
#endif
static void serial_puts(const char* s){ while(*s) outb(0x3F8,(uint8_t)*s++); }
static void serial_putdec(int v){
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { int t = v; if (v < 0){ b[i++]='-'; t=-t; } while(t){ b[i++]=(char)('0'+t%10); t/=10; } }
    for (int j = i-1; j >= 0; j--) outb(0x3F8,(uint8_t)b[j]);
}

// ---- tiny freestanding string helpers ----
static int  vstrlen(const char* s){ int n=0; while(s[n]) n++; return n; }
static int  vstreq(const char* a, const char* b){ while(*a && *a==*b){a++;b++;} return *a==*b; }
static void vstrcpy_n(char* d, const char* s, int cap){
    int i=0; while(s[i] && i<cap-1){ d[i]=s[i]; i++; } d[i]=0;
}

// ---- backend ----
static vfs_read_fn g_backend_read = 0;
static vfs_size_fn g_backend_size = 0;
static int         g_mounted      = 0;

// =====================================================================
//  Writable layer: RAMFS
// ---------------------------------------------------------------------
//  The boot medium (SFS) is read-only, so the VFS gains a second, in-memory
//  layer that owns every file created or written at runtime.
//
//  Lookup order for read/write:  RAMFS  ->  SFS (read-only fallback).
//  This keeps SFS semantics untouched (existing reads still work, and the
//  EROFS rule still protects SFS bytes) while giving DOS AH=3Ch/40h a real
//  destination.
//
//  Names are flat, mirroring SFS's flat 20-char namespace.
// =====================================================================
struct VfsRamFile {
    int      used;
    char     name[VFS_RAMFS_NAME_MAX];   // flat basename
    int      size;
    uint8_t  data[VFS_RAMFS_CAP];
};
static VfsRamFile g_ram[VFS_RAMFS_MAX];

static int ram_find(const char* name){
    for (int i = 0; i < VFS_RAMFS_MAX; i++)
        if (g_ram[i].used && vstreq(g_ram[i].name, name)) return i;
    return -1;
}

static int ram_alloc(const char* name){
    for (int i = 0; i < VFS_RAMFS_MAX; i++){
        if (!g_ram[i].used){
            g_ram[i].used = 1;
            vstrcpy_n(g_ram[i].name, name, VFS_RAMFS_NAME_MAX);
            g_ram[i].size = 0;
            for (int k = 0; k < VFS_RAMFS_CAP; k++) g_ram[i].data[k] = 0;
            return i;
        }
    }
    return -1;
}

// ---- open file table ----
struct VfsFile {
    int      used;
    char     path[VFS_PATH_MAX];     // resolved virtual path
    int      size;                   // bytes actually cached
    int      pos;                    // read/write cursor
    int      writable;               // 1 when backed by RAMFS
    int      ram;                    // RAMFS slot index, or -1 for SFS
    uint8_t  data[VFS_FILE_CACHE];
};
static VfsFile g_fds[VFS_MAX_FD];

void vfs_init(vfs_read_fn rd, vfs_size_fn sz){
    g_backend_read = rd;
    g_backend_size = sz;
    g_mounted      = (rd != 0);
    for (int i = 0; i < VFS_MAX_FD; i++){ g_fds[i].used = 0; g_fds[i].size = 0; g_fds[i].pos = 0; g_fds[i].writable = 0; g_fds[i].ram = -1; }
    for (int i = 0; i < VFS_RAMFS_MAX; i++) g_ram[i].used = 0;
    serial_puts("[VFS] mounted on SFS + RAMFS, ");
    serial_putdec(VFS_MAX_FD);
    serial_puts(" fds, sandbox roots active\n");
}

// ---------------------------------------------------------------------
//  Path normalisation: collapse \, //, "." and ".."
//  `in` must already be absolute. Result always starts with '/'.
// ---------------------------------------------------------------------
static int path_normalize(const char* in, char* out, int outsz){
    // Component stack (offsets into `out`).
    int  starts[32];
    int  depth = 0;
    int  o     = 0;

    if (outsz < 2) return -1;
    out[o++] = '/';

    int i = 0;
    while (in[i]){
        // skip separators (both flavours)
        while (in[i] == '/' || in[i] == '\\') i++;
        if (!in[i]) break;

        // grab one component
        int cs = i;
        while (in[i] && in[i] != '/' && in[i] != '\\') i++;
        int clen = i - cs;

        if (clen == 1 && in[cs] == '.') continue;                       // "."
        if (clen == 2 && in[cs] == '.' && in[cs+1] == '.'){             // ".."
            if (depth > 0){ depth--; o = starts[depth]; if (o < 1) o = 1; }
            // climbing above "/" just stays at "/"
            continue;
        }

        if (depth >= 32) return -1;
        if (o > 1){ if (o + 1 >= outsz) return -1; out[o++] = '/'; }
        starts[depth++] = (o > 1) ? o - 1 : 1;                          // position of the '/' that precedes us
        if (o + clen >= outsz) return -1;
        for (int k = 0; k < clen; k++) out[o++] = in[cs + k];
    }
    out[o] = 0;
    if (o == 0){ out[0] = '/'; out[1] = 0; }
    return 0;
}

// Is `path` inside `root`?  root "/" contains everything.
static int path_is_under(const char* root, const char* path){
    if (root[0] == '/' && root[1] == 0) return 1;
    int rl = vstrlen(root);
    // trailing slash on the root is tolerated
    while (rl > 1 && root[rl-1] == '/') rl--;
    for (int i = 0; i < rl; i++) if (path[i] != root[i]) return 0;
    return path[rl] == 0 || path[rl] == '/';
}

static const char* path_basename(const char* p){
    const char* b = p;
    for (const char* q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

// ---------------------------------------------------------------------
//  vfs_resolve
// ---------------------------------------------------------------------
int vfs_resolve(const char* path, char* out, int outsz){
    if (!path || !out) return -1;

    const char* root = (g_current && g_current->root_path[0]) ? g_current->root_path : "/";

    char joined[VFS_PATH_MAX];
    if (path[0] == '/' || path[0] == '\\'){
        // absolute: addresses the global namespace directly
        vstrcpy_n(joined, path, VFS_PATH_MAX);
    } else {
        // relative: anchored at the process's sandbox root
        int j = 0;
        for (int i = 0; root[i] && j < VFS_PATH_MAX-2; i++) joined[j++] = root[i];
        if (j == 0 || joined[j-1] != '/') joined[j++] = '/';
        for (int i = 0; path[i] && j < VFS_PATH_MAX-1; i++) joined[j++] = path[i];
        joined[j] = 0;
    }
    return path_normalize(joined, out, outsz);
}

// ---------------------------------------------------------------------
//  vfs_check_access  -  the single security choke point
// ---------------------------------------------------------------------
//  Today: sandbox containment only.  The Y/N popup (doc 3.3), signature
//  trust (3.4) and the deception engine (3.6) all plug in right here.
// ---------------------------------------------------------------------
int vfs_check_access(const char* abspath, int mode){
    // Kernel / SYSTEM bypasses the sandbox entirely.
    if (!g_current || g_current->ring == 0 || g_current->uid == SYSTEM_UID){
        serial_puts("[VFS] BYPASS (system) path=");
        serial_puts(abspath);
        serial_puts("\n");
        (void)mode;
        return VFS_ALLOW;
    }

    const char* root = g_current->root_path[0] ? g_current->root_path : "/";

    if (!path_is_under(root, abspath)){
        // Leaving the sandbox is not automatically fatal any more: doc 3.3
        // says the HUMAN decides.  perm_request() classifies the target,
        // refuses the non-negotiable classes outright (credential stores,
        // autostart), consults remembered answers, and only then raises a
        // blocking Y/N prompt.  Its contract is fail-safe: anything that
        // is not an explicit ALLOW comes back as DENY.
        int cls = perm_classify(abspath, root);
        if (perm_request(cls, abspath, (mode & VFS_ACC_WRITE) ? 1 : 0) != PERM_ALLOW){
            serial_puts("[VFS] DENIED pid=");   serial_putdec((int)g_current->pid);
            serial_puts(" uid=");               serial_putdec((int)g_current->uid);
            serial_puts(" root=");              serial_puts(root);
            serial_puts(" path=");              serial_puts(abspath);
            serial_puts(" (outside sandbox)\n");
            return VFS_DENY;
        }
        // Consent granted -- fall through to the medium checks below, so a
        // "yes" can never smuggle a write past the read-only backend.
        serial_puts("[VFS] GRANTED-BY-USER pid="); serial_putdec((int)g_current->pid);
        serial_puts(" path=");                     serial_puts(abspath);
        serial_puts("\n");
    }

    // SFS is read-only, but RAMFS is not.  A write is only refused when the
    // target is NOT already owned by the writable layer AND is not being
    // created there; vfs_create() establishes ownership before writing.
    //
    // `mode` carries VFS_ACC_WRITE; the g_ram lookup below is what makes the
    // two media distinguishable at the choke point, so the EROFS log stays
    // accurate instead of firing for every write.
    if (mode & VFS_ACC_WRITE){
        if (ram_find(path_basename(abspath)) >= 0){
            serial_puts("[VFS] RW (ramfs) pid="); serial_putdec((int)(g_current ? g_current->pid : -1));
            serial_puts(" path=");                serial_puts(abspath);
            serial_puts("\n");
            return VFS_ALLOW;
        }
        serial_puts("[VFS] EROFS pid=");   serial_putdec((int)(g_current ? g_current->pid : -1));
        serial_puts(" path=");              serial_puts(abspath);
        serial_puts(" (read-only backend)\n");
        return VFS_DENY;
    }

    serial_puts("[VFS] GRANTED pid=");      serial_putdec((int)g_current->pid);
    serial_puts(" path=");                  serial_puts(abspath);
    serial_puts("\n");
    return VFS_ALLOW;
}

// ---------------------------------------------------------------------
//  open / read / write / close
// ---------------------------------------------------------------------
//  Medium selection happens here and nowhere else:
//    * a name owned by RAMFS opens writable and is served from g_ram
//    * everything else falls through to the read-only SFS cache
// ---------------------------------------------------------------------
int vfs_open(const char* path, int flags){
    if (!g_mounted) return -1;

    char abs[VFS_PATH_MAX];
    if (vfs_resolve(path, abs, VFS_PATH_MAX) != 0){
        serial_puts("[VFS] open: unresolvable path\n");
        return -1;
    }

    const char* base = path_basename(abs);

    // ---- RAMFS first ----
    int ri = ram_find(base);
    if (ri >= 0){
        if (vfs_check_access(abs, VFS_ACC_READ) != VFS_ALLOW) return -1;
        int slot = -1;
        for (int i = 3; i < VFS_MAX_FD; i++) if (!g_fds[i].used){ slot = i; break; }
        if (slot < 0){ serial_puts("[VFS] open: fd table full\n"); return -1; }
        VfsFile* f = &g_fds[slot];
        f->used     = 1;
        f->writable = 1;
        f->ram      = ri;
        f->size     = g_ram[ri].size;
        f->pos      = 0;
        vstrcpy_n(f->path, abs, VFS_PATH_MAX);
        return slot;
    }

    // ---- SFS (read-only) ----
    // BUG #8: resolve the write intent from the O_RDWR/O_WRONLY bits AND
    // from O_CREAT/O_TRUNC, so "open for create" cannot slip through as a
    // read.  Without vfs_create() first, SFS refuses the write intent.
    int mode = VFS_ACC_READ;
    if (flags & 0x3)   mode |= VFS_ACC_WRITE;   // O_RDWR / O_WRONLY
    if (flags & 0x40)  mode |= VFS_ACC_WRITE;   // O_CREAT
    if (flags & 0x200) mode |= VFS_ACC_WRITE;  // O_TRUNC
    if (vfs_check_access(abs, mode) != VFS_ALLOW)
        return -1;                       // -EACCES / -EROFS

    int slot = -1;
    for (int i = 3; i < VFS_MAX_FD; i++)  // 0/1/2 stay reserved for std streams
        if (!g_fds[i].used){ slot = i; break; }
    if (slot < 0){ serial_puts("[VFS] open: fd table full\n"); return -1; }

    VfsFile* f = &g_fds[slot];
    int n = g_backend_read(base, f->data, VFS_FILE_CACHE);
    if (n < 0){
        serial_puts("[VFS] open: not found ");
        serial_puts(abs); serial_puts("\n");
        return -1;                       // -ENOENT
    }

    f->used     = 1;
    f->writable = 0;
    f->ram      = -1;
    f->size     = n;
    f->pos      = 0;
    vstrcpy_n(f->path, abs, VFS_PATH_MAX);
    return slot;
}

// Create (or truncate) a file in the writable layer and return a handle.
// SFS is never touched, so the boot image stays pristine.
int vfs_create(const char* path, int flags){
    (void)flags;
    if (!g_mounted) return -1;

    char abs[VFS_PATH_MAX];
    if (vfs_resolve(path, abs, VFS_PATH_MAX) != 0){
        serial_puts("[VFS] create: unresolvable path\n");
        return -1;
    }
    const char* base = path_basename(abs);

    // Create intentionally establishes ownership in the writable layer, so
    // the choke point must be consulted with READ semantics for the
    // "containment" decision, NOT with WRITE.
    //
    // The old order (check WRITE -> then allocate) was a chicken-and-egg
    // deadlock: vfs_check_access(mode=WRITE) only ALLOWS a write when the
    // name is ALREADY in RAMFS, and at create time it never is, so every
    // create was refused with EROFS.  That is what `AH=3Ch create FAILED`
    // in fileio.com was reporting.
    //
    // Sandbox security is unaffected: containment is still enforced on the
    // full resolved path, and the medium rule is deliberately bypassed
    // only because this function is the thing that MAKES the file writable.
    if (vfs_check_access(abs, VFS_ACC_READ) != VFS_ALLOW) return -1;

    int ri = ram_find(base);
    if (ri < 0){
        ri = ram_alloc(base);
        if (ri < 0){ serial_puts("[VFS] create: ramfs full\n"); return -1; }
    } else {
        g_ram[ri].size = 0;              // O_TRUNC semantics
    }

    int slot = -1;
    for (int i = 3; i < VFS_MAX_FD; i++) if (!g_fds[i].used){ slot = i; break; }
    if (slot < 0){ serial_puts("[VFS] create: fd table full\n"); return -1; }

    VfsFile* f = &g_fds[slot];
    f->used     = 1;
    f->writable = 1;
    f->ram      = ri;
    f->size     = 0;
    f->pos      = 0;
    vstrcpy_n(f->path, abs, VFS_PATH_MAX);

    serial_puts("[VFS] create: "); serial_puts(abs); serial_puts("\n");
    return slot;
}

int vfs_read(int fd, void* buf, int count){
    if (fd < 3 || fd >= VFS_MAX_FD || !g_fds[fd].used) return -1;
    VfsFile* f = &g_fds[fd];

    // RAMFS-backed: read straight from the writable store.
    if (f->ram >= 0){
        int ri = f->ram;
        int left = g_ram[ri].size - f->pos;
        if (left <= 0) return 0;                    // EOF
        int n = (count < left) ? count : left;
        uint8_t* d = (uint8_t*)buf;
        for (int i = 0; i < n; i++) d[i] = g_ram[ri].data[f->pos + i];
        f->pos += n;
        return n;
    }

    // SFS: serve from the per-fd cache.
    int left = f->size - f->pos;
    if (left <= 0) return 0;                    // EOF
    int n = (count < left) ? count : left;
    uint8_t* d = (uint8_t*)buf;
    for (int i = 0; i < n; i++) d[i] = f->data[f->pos + i];
    f->pos += n;
    return n;
}

int vfs_write(int fd, const void* buf, int count){
    if (fd < 3 || fd >= VFS_MAX_FD || !g_fds[fd].used) return -1;
    VfsFile* f = &g_fds[fd];

    // SFS stays read-only: routed through the choke point so the refusal
    // is still auditable via the EROFS log.
    if (!f->writable || f->ram < 0){
        if (vfs_check_access(f->path, VFS_ACC_WRITE) != VFS_ALLOW) return -1;
        serial_puts("[VFS] EROFS write fd="); serial_putdec(fd);
        serial_puts(" path=");                serial_puts(f->path);
        serial_puts("\n");
        return -1;                                   // -EROFS
    }

    int ri = f->ram;
    int room = VFS_RAMFS_CAP - f->pos;
    if (room <= 0) return 0;                         // medium full
    int n = (count < room) ? count : room;
    const uint8_t* s = (const uint8_t*)buf;
    for (int i = 0; i < n; i++) g_ram[ri].data[f->pos + i] = s[i];
    f->pos += n;
    if (f->pos > g_ram[ri].size) g_ram[ri].size = f->pos;
    f->size = g_ram[ri].size;
    return n;
}

int vfs_close(int fd){
    if (fd < 3 || fd >= VFS_MAX_FD || !g_fds[fd].used) return -1;
    g_fds[fd].used = 0;
    g_fds[fd].size = 0;
    g_fds[fd].pos  = 0;
    g_fds[fd].ram  = -1;
    g_fds[fd].writable = 0;
    return 0;
}

// ---------------------------------------------------------------------
int vfs_is_writable(const char* path){
    char abs[VFS_PATH_MAX];
    if (!path || vfs_resolve(path, abs, VFS_PATH_MAX) != 0) return 0;
    return ram_find(path_basename(abs)) >= 0 ? 1 : 0;
}

// ---------------------------------------------------------------------
void vfs_dump(void){
    serial_puts("[VFS] mounted=");   serial_putdec(g_mounted);
    serial_puts(" current=");        serial_puts(g_current ? g_current->name : "(none)");
    serial_puts(" uid=");            serial_putdec(g_current ? (int)g_current->uid : -1);
    serial_puts(" ring=");           serial_putdec(g_current ? (int)g_current->ring : -1);
    serial_puts(" root=");           serial_puts(g_current ? g_current->root_path : "?");
    serial_puts("\n");
    for (int i = 3; i < VFS_MAX_FD; i++)
        if (g_fds[i].used){
            serial_puts("       fd "); serial_putdec(i);
            serial_puts(g_fds[i].writable ? " (rw) -> " : " (ro) -> ");
            serial_puts(g_fds[i].path);
            serial_puts(" ("); serial_putdec(g_fds[i].size); serial_puts(" bytes)\n");
        }
    serial_puts("[VFS] ramfs files: ");
    {
        int n = 0;
        for (int i = 0; i < VFS_RAMFS_MAX; i++) if (g_ram[i].used) n++;
        serial_putdec(n);
    }
    serial_puts("\n");
    for (int i = 0; i < VFS_RAMFS_MAX; i++)
        if (g_ram[i].used){
            serial_puts("       ram "); serial_puts(g_ram[i].name);
            serial_puts(" ("); serial_putdec(g_ram[i].size); serial_puts(" bytes)\n");
        }
}
