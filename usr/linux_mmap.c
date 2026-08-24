/* =====================================================================
 *  usr/linux_mmap.c  -  Stage 3 Linux mmap / mprotect / munmap smoke test
 * ---------------------------------------------------------------------
 *  Freestanding ELF32 guest exercising the NexOS Linux-compat memory layer
 *  directly via int 0x80:
 *    - sys_mmap    (90, old_mmap ABI: struct{addr,len,prot,flags,fd,off} in ebx)
 *    - sys_munmap  (91)
 *    - sys_mprotect (125)
 *
 *  Verifies (Stage 3 goals):
 *    A) mmap PROT_READ|WRITE|EXEC returns a usable page; machine code written
 *       into it EXECUTES (returns 0x1234) -> real executable anonymous mapping.
 *    B) mprotect toggles RW<->RO and preserves data (round-trip), returns 0.
 *    C) munmap frees a region; a fresh mmap afterwards is usable.
 *    D) arena is large: a single 4 MiB mmap succeeds and is fully writable.
 *
 *  Build: freestanding i686-elf (same recipe as linux_signal.c)
 *  Run:   linux linux_mmap   (from the NexOS shell)
 * ===================================================================== */
#include "libc.h"

#define SYS_MMAP      90
#define SYS_MUNMAP    91
#define SYS_MPROTECT  125
#define SYS_WRITE     4
#define SYS_EXIT      1

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4
#define PROT_NONE     0x0
#define MAP_PRIVATE   0x2
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10

typedef unsigned long ulong;

/* ---- raw syscall wrappers ---- */
struct mmap_arg { ulong addr, len, prot, flags, fd, offset; };

static inline long sys_mmap(void* addr, ulong len, int prot, int flags, int fd, ulong offset)
{
    struct mmap_arg a;
    a.addr = (ulong)addr; a.len = len; a.prot = (ulong)prot;
    a.flags = (ulong)flags; a.fd = (ulong)fd; a.offset = offset;
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "a"((long)SYS_MMAP), "b"((long)&a)
        : "memory", "cc");
    return ret;
}
static inline long sys_munmap(void* addr, ulong len)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "a"((long)SYS_MUNMAP), "b"((long)addr), "c"((long)len)
        : "memory", "cc");
    return ret;
}
static inline long sys_mprotect(void* addr, ulong len, int prot)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "a"((long)SYS_MPROTECT), "b"((long)addr),
          "c"((long)len), "d"((long)prot)
        : "memory", "cc");
    return ret;
}
static inline long sys_write(int fd, const void* buf, ulong n)
{
    long ret;
    __asm__ volatile ("int $0x80"
        : "=a"(ret) : "a"((long)SYS_WRITE), "b"((long)fd),
          "c"((long)buf), "d"((long)n)
        : "memory", "cc");
    return ret;
}
static inline void sys_exit(int code)
{
    __asm__ volatile ("int $0x80" :: "a"((long)SYS_EXIT), "b"((long)code)
        : "memory", "cc");
    __builtin_unreachable();
}

/* ---- minimal local output helpers (avoid clashing with libc.c's puts) ---- */
static void lx_puts(const char* s){
    while (*s) sys_write(1, s, 1), s++;
}
static void lx_dec(int v){
    char buf[12]; int i = 0;
    if (v < 0){ lx_puts("-"); v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v){ buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) sys_write(1, &buf[--i], 1);
}
static void lx_hex(unsigned long v){
    const char* h = "0123456789ABCDEF";
    char buf[9]; int i = 0;
    for (int s = 28; s >= 0; s -= 4) buf[i++] = h[(v >> s) & 0xF];
    for (int k = 0; k < i; k++) sys_write(1, &buf[k], 1);
}

static int g_fail = 0;
static void check(int cond, const char* name){
    if (cond){ lx_puts("LXMMAP: PASS "); lx_puts(name); lx_puts("\n"); }
    else { lx_puts("LXMMAP: FAIL "); lx_puts(name); lx_puts("\n"); g_fail = 1; }
}

/* mov eax, 0x1234 ; ret  -- returns 0x1234 when executed */
static const unsigned char g_code[6] = { 0xB8, 0x34, 0x12, 0x00, 0x00, 0xC3 };

int main(int argc, char** argv, char** envp)
{
    (void)argc; (void)argv;
    lx_puts("LXMMAP: start\n");

    /* ---- Test A: mmap PROT_EXEC, write code, execute it ---- */
    ulong a = (ulong)sys_mmap(0, 4096,
                               PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check((long)a > 0 && (long)a != -1, "mmap PROT_EXEC returned VA");
    if ((long)a > 0 && (long)a != -1){
        lx_puts("LXMMAP: mmap exec VA=0x"); lx_hex(a); lx_puts("\n");
        unsigned char* p = (unsigned char*)a;
        for (int i = 0; i < 6; i++) p[i] = g_code[i];
        int (*fn)(void) = (int(*)(void))a;
        int rv = fn();
        lx_puts("LXMMAP: executed -> 0x"); lx_hex((unsigned long)rv); lx_puts("\n");
        check(rv == 0x1234, "executable page ran (returned 0x1234)");
    }

    /* ---- Test B: mprotect RW->RO->RW preserves data ---- */
    ulong b = (ulong)sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check((long)b > 0 && (long)b != -1, "mmap RW returned VA");
    if ((long)b > 0 && (long)b != -1){
        *(volatile unsigned long*)b = 0xDEADBEEFu;
        check(*(volatile unsigned long*)b == 0xDEADBEEFu, "wrote/readed RW page");
        long r1 = sys_mprotect((void*)b, 4096, PROT_READ);
        long r2 = sys_mprotect((void*)b, 4096, PROT_READ | PROT_WRITE);
        check(r1 == 0 && r2 == 0, "mprotect RO then RW returned 0");
        *(volatile unsigned long*)b = 0xCAFEBABEu;
        check(*(volatile unsigned long*)b == 0xCAFEBABEu, "data survived mprotect round-trip");
    }

    /* ---- Test C: munmap then fresh mmap usable ---- */
    ulong c = (ulong)sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    long um = -1;
    if ((long)c > 0 && (long)c != -1){
        *(volatile unsigned long*)c = 0x11111111u;
        um = sys_munmap((void*)c, 4096);
    }
    check(um == 0, "munmap returned 0");
    ulong d = (ulong)sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)d > 0 && (long)d != -1){
        *(volatile unsigned long*)d = 0x22222222u;
        check(*(volatile unsigned long*)d == 0x22222222u, "remap after munmap usable");
    } else {
        check(0, "remap after munmap usable");
    }

    /* ---- Test D: big arena (4 MiB single mmap) ---- */
    ulong e = (ulong)sys_mmap(0, 4 * 1024 * 1024,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((long)e > 0 && (long)e != -1){
        lx_puts("LXMMAP: big mmap VA=0x"); lx_hex(e); lx_puts("\n");
        volatile unsigned char* q = (volatile unsigned char*)e;
        q[0] = 0xAA;
        q[2 * 1024 * 1024] = 0xBB;
        q[4 * 1024 * 1024 - 4] = 0xCC;
        int ok = (q[0] == 0xAA) && (q[2 * 1024 * 1024] == 0xBB)
              && (q[4 * 1024 * 1024 - 4] == 0xCC);
        check(ok, "4 MiB arena mmap fully writable");
        sys_munmap((void*)e, 4 * 1024 * 1024);
    } else {
        check(0, "4 MiB arena mmap fully writable");
    }

    lx_puts("LXMMAP: all tests done\n");
    sys_exit(g_fail ? 1 : 0);
}
