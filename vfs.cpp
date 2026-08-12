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
static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
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

// ---- open file table ----
struct VfsFile {
    int      used;
    char     path[VFS_PATH_MAX];     // resolved virtual path
    int      size;                   // bytes actually cached
    int      pos;                    // read cursor
    uint8_t  data[VFS_FILE_CACHE];
};
static VfsFile g_fds[VFS_MAX_FD];

void vfs_init(vfs_read_fn rd, vfs_size_fn sz){
    g_backend_read = rd;
    g_backend_size = sz;
    g_mounted      = (rd != 0);
    for (int i = 0; i < VFS_MAX_FD; i++){ g_fds[i].used = 0; g_fds[i].size = 0; g_fds[i].pos = 0; }
    serial_puts("[VFS] mounted on SFS, ");
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

    // SFS is read-only; a write attempt inside the sandbox is still refused,
    // but it is refused as "read-only medium", not as a sandbox violation.
    if (mode & VFS_ACC_WRITE){
        serial_puts("[VFS] DENIED pid=");   serial_putdec((int)g_current->pid);
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
int vfs_open(const char* path, int flags){
    if (!g_mounted) return -1;

    char abs[VFS_PATH_MAX];
    if (vfs_resolve(path, abs, VFS_PATH_MAX) != 0){
        serial_puts("[VFS] open: unresolvable path\n");
        return -1;
    }

    int mode = (flags & 0x3) ? VFS_ACC_WRITE : VFS_ACC_READ;
    if (vfs_check_access(abs, mode) != VFS_ALLOW)
        return -1;                       // -EACCES

    int slot = -1;
    for (int i = 3; i < VFS_MAX_FD; i++)  // 0/1/2 stay reserved for std streams
        if (!g_fds[i].used){ slot = i; break; }
    if (slot < 0){ serial_puts("[VFS] open: fd table full\n"); return -1; }

    VfsFile* f = &g_fds[slot];
    int n = g_backend_read(path_basename(abs), f->data, VFS_FILE_CACHE);
    if (n < 0){
        serial_puts("[VFS] open: not found ");
        serial_puts(abs); serial_puts("\n");
        return -1;                       // -ENOENT
    }

    f->used = 1;
    f->size = n;
    f->pos  = 0;
    vstrcpy_n(f->path, abs, VFS_PATH_MAX);
    return slot;
}

int vfs_read(int fd, void* buf, int count){
    if (fd < 3 || fd >= VFS_MAX_FD || !g_fds[fd].used) return -1;
    VfsFile* f = &g_fds[fd];
    int left = f->size - f->pos;
    if (left <= 0) return 0;                    // EOF
    int n = (count < left) ? count : left;
    uint8_t* d = (uint8_t*)buf;
    for (int i = 0; i < n; i++) d[i] = f->data[f->pos + i];
    f->pos += n;
    return n;
}

int vfs_write(int fd, const void* buf, int count){
    (void)buf; (void)count;
    if (fd < 3 || fd >= VFS_MAX_FD || !g_fds[fd].used) return -1;
    // Routed through the choke point so the refusal is auditable.
    if (vfs_check_access(g_fds[fd].path, VFS_ACC_WRITE) != VFS_ALLOW) return -1;
    return -1;                                   // -EROFS
}

int vfs_close(int fd){
    if (fd < 3 || fd >= VFS_MAX_FD || !g_fds[fd].used) return -1;
    g_fds[fd].used = 0;
    g_fds[fd].size = 0;
    g_fds[fd].pos  = 0;
    return 0;
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
            serial_puts(" -> ");       serial_puts(g_fds[i].path);
            serial_puts(" ("); serial_putdec(g_fds[i].size); serial_puts(" bytes)\n");
        }
}
