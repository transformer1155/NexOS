#!/usr/bin/env bash
# =====================================================================
#  test_vfs_ramfs.sh  -  verify the writable-medium layer in vfs.cpp
#
#  vfs.cpp is freestanding kernel code (outb to 0x3F8, no libc), so it
#  cannot link against the host's glibc directly.  This harness compiles
#  the REAL vfs.cpp with thin host shims for the kernel-only symbols
#  (proc/perm), then exercises the real vfs_open/read/write/create logic
#  -- the same code the kernel runs.
#
#  Run from WSL:  bash test_vfs_ramfs.sh
# =====================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # NexOS/dos_compat
NEXROOT="$(cd "$HERE/.." && pwd)"                      # NexOS/
OUT="$HERE/out"
mkdir -p "$OUT"

# ---- shim: kernel-only symbols, host implementations ----
cat > "$OUT/vfs_shim.cpp" <<'SHIM'
// Host shims so the real vfs.cpp can be exercised off-kernel.
// NOTE: vfs.cpp declares these without extern "C" (it is C++), so the
// shims must use C++ linkage too -- matching vfs.h's own #include of
// proc.h / perm.h, which are C++ headers.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "vfs.h"
#include "proc.h"      // the REAL Process layout -- do not re-declare it

// vfs.cpp reads g_current->pid / uid / ring / root_path.  Use the genuine
// struct from proc.h so field offsets are correct; a hand-rolled copy
// (wrong widths, wrong root_path size) segfaults.
Process  g_host_proc;
Process* g_current = &g_host_proc;

// ---- SFS backend, faked with one read-only file ----
static const char* kName = "SEED.TXT";
static const char* kBody = "sfs-readonly-content";

static int host_read(const char* name, unsigned char* buf, int bufsize) {
    if (strcmp(name, kName) != 0) return -1;
    int n = (int)strlen(kBody);
    if (n > bufsize) n = bufsize;
    memcpy(buf, kBody, n);
    return n;
}
static int host_size(const char* name) {
    return strcmp(name, kName) == 0 ? (int)strlen(kBody) : -1;
}

// ---- perms: allow everything so we test the MEDIUM, not the sandbox ----
int perm_classify(const char* p, const char* r) { (void)p; (void)r; return 0; }
int perm_request(int cls, const char* p, int w) { (void)cls; (void)p; (void)w; return 1; }

void host_vfs_init(void) { vfs_init(host_read, host_size); }
SHIM

# ---- test body ----
cat > "$OUT/vfs_test.cpp" <<'TEST'
#include <cstdio>
#include <cstring>
#include "vfs.h"
#include "proc.h"
void host_vfs_init(void);

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } \
                         else printf("ok  : %s\n", m); } while (0)

int main(void) {
    printf("=== VFS writable-medium test ===\n");
    host_vfs_init();
    // ring 0 = kernel/System, which bypasses the sandbox so this test
    // isolates the MEDIUM behaviour from the permission layer.
    g_current->pid  = 1;
    g_current->uid  = 0;
    g_current->ring = 0;
    strcpy(g_current->name, "test");
    strcpy(g_current->root_path, "/");

    // 1) SFS still readable (existing behaviour preserved)
    int rfd = vfs_open("SEED.TXT", 0);
    CHECK(rfd >= 0, "SFS file still opens read-only");
    char rb[64]; memset(rb, 0, sizeof(rb));
    int rn = vfs_read(rfd, rb, 32);
    CHECK(rn == 20 && memcmp(rb, "sfs-readonly-content", 20) == 0, "SFS content intact");
    vfs_close(rfd);

    // 2) SFS still refuses writes (EROFS rule preserved)
    int wfd = vfs_open("SEED.TXT", 0);
    CHECK(vfs_write(wfd, "x", 1) < 0, "SFS write still refused (EROFS)");
    vfs_close(wfd);

    // 3) create -> write -> read back through the writable layer
    int cfd = vfs_create("NEW.TXT", 0);
    CHECK(cfd >= 0, "vfs_create succeeds on writable medium");
    CHECK(vfs_is_writable("NEW.TXT") == 1, "created file is in the writable layer");
    const char* payload = "hello-writable-world";
    int wn = vfs_write(cfd, payload, (int)strlen(payload));
    CHECK(wn == (int)strlen(payload), "write stores full payload");
    vfs_close(cfd);

    int ofd = vfs_open("NEW.TXT", 0);
    CHECK(ofd >= 0, "created file reopens");
    char ob[64]; memset(ob, 0, sizeof(ob));
    int on = vfs_read(ofd, ob, 32);
    CHECK(on == (int)strlen(payload), "read-back length matches");
    CHECK(memcmp(ob, payload, strlen(payload)) == 0, "read-back bytes match");
    vfs_close(ofd);

    // 4) truncate-on-recreate
    int tfd = vfs_create("NEW.TXT", 0);
    CHECK(tfd >= 0, "re-create truncates");
    vfs_write(tfd, "abc", 3);
    vfs_close(tfd);
    int t2 = vfs_open("NEW.TXT", 0);
    char tb[16]; memset(tb, 0, sizeof(tb));
    int tn = vfs_read(t2, tb, 16);
    CHECK(tn == 3 && memcmp(tb, "abc", 3) == 0, "truncate gave a clean 3-byte file");
    vfs_close(t2);

    printf("\n=== %s ===\n", fails == 0 ? "ALL PASS" : "TEST FAILED");
    return fails == 0 ? 0 : 1;
}
TEST

echo "==> compiling real vfs.cpp (host harness)"
g++ -std=c++17 -Wall -DVFS_HOST_TEST -I"$NEXROOT" -I"$HERE" -c "$NEXROOT/vfs.cpp" -o "$OUT/vfs_real.o" || exit 1
echo "    vfs.cpp compiled OK"
g++ -std=c++17 -Wall -DVFS_HOST_TEST -I"$NEXROOT" -I"$HERE" -c "$OUT/vfs_shim.cpp" -o "$OUT/vfs_shim.o" || exit 1
g++ -std=c++17 -Wall -DVFS_HOST_TEST -I"$NEXROOT" -I"$HERE" -c "$OUT/vfs_test.cpp" -o "$OUT/vfs_test.o" || exit 1
g++ "$OUT/vfs_real.o" "$OUT/vfs_shim.o" "$OUT/vfs_test.o" -o "$OUT/vfs_test" || exit 1

echo
echo "==> running VFS writable-medium test"
"$OUT/vfs_test"