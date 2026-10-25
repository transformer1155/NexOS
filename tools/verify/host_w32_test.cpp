// =====================================================================
//  host_w32_test.cpp  -  run the NexOS Win32 subsystem on the build host
// ---------------------------------------------------------------------
//  win32.cpp is freestanding and only needs kmalloc/kfree plus a file
//  reader and a console writer.  That makes it possible to link it into
//  a tiny static 32-bit Linux program and exercise the whole PE32 path
//  (mapping, base relocations, import binding, native execution, the GDI
//  display list) in a second, instead of rebuilding an ISO and booting
//  QEMU for every change.
//
//  Build (see tools/host_w32_test.sh):
//    g++ -m32 -ffreestanding -fno-exceptions -fno-rtti -nostdlib
//        -DW32_HOSTTEST -static -Wl,-z,execstack ...
//
//  -z execstack matters: on i386 Linux a RWE PT_GNU_STACK turns on the
//  READ_IMPLIES_EXEC personality, so the heap the PE image is mapped
//  into becomes executable and the entry point can actually be called.
// =====================================================================
#include <stdint.h>
#include "../win32.h"

// ---------------------------------------------------------------------
//  Raw Linux i386 syscalls (no libc is linked)
// ---------------------------------------------------------------------
static inline int sys3(int nr, int a, int b, int c){
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static void sys_exit(int code){ sys3(1, code, 0, 0); __builtin_unreachable(); }
static int  sys_read (int fd, void* buf, int n){ return sys3(3, fd, (int)buf, n); }
static int  sys_write(int fd, const void* buf, int n){ return sys3(4, fd, (int)buf, n); }
static int  sys_open (const char* path, int flags){ return sys3(5, (int)path, flags, 0); }
static int  sys_close(int fd){ return sys3(6, fd, 0, 0); }
static int  sys_mprotect(void* a, int n, int prot){ return sys3(125, (int)a, n, prot); }

static int  h_len(const char* s){ int n = 0; while (s && s[n]) n++; return n; }
static void out(const char* s){ sys_write(1, s, h_len(s)); }
static void outn(int v){
    char t[16]; int n = 0; unsigned u = (unsigned)v;
    if (v < 0){ sys_write(1, "-", 1); u = (unsigned)(-v); }
    if (!u){ sys_write(1, "0", 1); return; }
    while (u){ t[n++] = (char)('0' + u % 10); u /= 10; }
    char o[16]; int i = 0; while (n) o[i++] = t[--n];
    sys_write(1, o, i);
}

// ---------------------------------------------------------------------
//  Kernel allocator stand-in: a plain bump allocator with a free list of
//  one (win32.cpp only ever holds a couple of blocks at a time).
// ---------------------------------------------------------------------
// Page aligned: host_main() marks it PROT_READ|WRITE|EXEC so the PE image
// mapped into it can actually be executed.  (Since Linux 5.8 a RWE
// PT_GNU_STACK no longer enables READ_IMPLIES_EXEC on i386, so relying on
// -z execstack alone is not enough.)
static uint8_t g_heap[4u * 1024u * 1024u] __attribute__((aligned(4096)));
static uint32_t g_top = 0;

extern "C" void* kmalloc(uint32_t size){
    size = (size + 15u) & ~15u;
    if (g_top + size > sizeof(g_heap)) return 0;
    void* p = &g_heap[g_top];
    g_top += size;
    return p;
}
extern "C" void kfree(void*){ /* bump allocator: nothing to do */ }

// ---------------------------------------------------------------------
//  Host file reader / console writer handed to win32_init()
// ---------------------------------------------------------------------
static const char* g_dir = "";

static int host_read(const char* name, uint8_t* buf, int bufsize){
    char path[256]; int n = 0;
    for (const char* p = g_dir; *p && n < 200; p++) path[n++] = *p;
    for (const char* p = name;  *p && n < 250; p++) path[n++] = *p;
    path[n] = 0;
    int fd = sys_open(path, 0 /* O_RDONLY */);
    if (fd < 0) return -1;
    int total = 0;
    while (total < bufsize){
        int r = sys_read(fd, buf + total, bufsize - total);
        if (r <= 0) break;
        total += r;
    }
    sys_close(fd);
    return total;
}
static void host_write(const char* s){ out(s); }

// ---------------------------------------------------------------------
static const char* kind_name(uint8_t k){
    switch (k){
        case W32_CMD_FILLRECT:  return "FILLRECT ";
        case W32_CMD_FRAMERECT: return "FRAMERECT";
        case W32_CMD_TEXT:      return "TEXT     ";
        case W32_CMD_LINE:      return "LINE     ";
        case W32_CMD_ELLIPSE:   return "ELLIPSE  ";
        case W32_CMD_BUTTON:    return "BUTTON   ";
    }
    return "?        ";
}

static void dump_windows(const char* tag){
    out("\n--- GDI display list ("); out(tag); out(") ---\n");
    int wn = win32_window_count();
    out("windows: "); outn(wn); out("\n");
    for (int i = 0; i < wn; i++){
        W32WinInfo wi;
        if (!win32_window_info(i, &wi)) continue;
        out("  [win "); outn(i); out("] \""); out(wi.title);
        out("\" cls="); out(wi.cls);
        out(" pos="); outn(wi.x); out(","); outn(wi.y);
        out(" size="); outn(wi.w); out("x"); outn(wi.h);
        out(wi.visible ? " visible" : " hidden"); out("\n");
        const W32DrawCmd* c = 0;
        int n = win32_window_cmds(i, &c);
        out("    draw commands: "); outn(n); out("\n");
        for (int k = 0; k < n; k++){
            out("      "); out(kind_name(c[k].kind));
            out(" x="); outn(c[k].x); out(" y="); outn(c[k].y);
            out(" w="); outn(c[k].w); out(" h="); outn(c[k].h);
            if (c[k].text[0]){ out("  \""); out(c[k].text); out("\""); }
            out("\n");
        }
    }
}

static char g_buf[4096];

extern "C" void host_main(int argc, char** argv){
    const char* file = (argc > 1) ? argv[1] : "hello32.exe";
    if (argc > 2) g_dir = argv[2];

    // NexOS runs with paging off / everything executable; reproduce that
    // for the block the PE image gets mapped into.
    if (sys_mprotect(g_heap, (int)sizeof(g_heap), 7) < 0){
        out("mprotect(RWX) failed - the entry point cannot be called\n");
        sys_exit(2);
    }

    win32_init(host_read, host_write);

    out("=== registry sanity check ===\n");
    int rc = win32_reg_query("HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             "ProductName", g_buf, (int)sizeof(g_buf));
    out("reg_query -> "); outn(rc); out("\n"); out(g_buf);
    out("keys="); outn(win32_reg_key_count());
    out(" values="); outn(win32_reg_value_count()); out("\n");

    // Pass 1: map + relocate + bind imports, but do not run the code.  If
    // the executing pass crashes we still have this report on screen.
    out("\n=== inspecting ");
    out(file);
    out(" (info only) ===\n");
    int r = win32_run(file, "", 1);
    out(win32_last_report());
    out("win32_run(info) -> "); outn(r); out("\n");
    if (r != 0) sys_exit(1);

    out("\n=== executing ");
    out(file);
    out(" ===\n");
    r = win32_run(file, "", 0);
    out(win32_last_report());
    out("win32_run -> "); outn(r); out("\n");

    if (r != 0) sys_exit(1);

    dump_windows("after WinMain");

    // simulate what gui.cpp does when the user clicks and types
    out("\n=== simulating a click + keystroke from the GUI ===\n");
    win32_window_dispatch(0, 0x0201, 0, (30 << 16) | 340);   // WM_LBUTTONDOWN
    win32_window_dispatch(0, 0x0202, 0, (30 << 16) | 340);   // WM_LBUTTONUP
    win32_window_dispatch(0, 0x0102, 'K', 0);                // WM_CHAR
    win32_window_repaint(0);
    dump_windows("after click + key");

    out("\nHOST TEST OK\n");
    sys_exit(0);
}

// ---------------------------------------------------------------------
//  ELF entry point: the stack holds argc, argv[0..], NULL, envp...
// ---------------------------------------------------------------------
extern "C" __attribute__((naked)) void _start(){
    __asm__ volatile(
        "mov  %esp, %eax\n"
        "mov  (%eax), %ebx\n"        // argc
        "lea  4(%eax), %ecx\n"       // argv
        "and  $-16, %esp\n"
        "push %ecx\n"
        "push %ebx\n"
        "call host_main\n"
        "push $0\n"
        "push $0\n"
        "push $0\n"
        "mov  $1, %eax\n"
        "xor  %ebx, %ebx\n"
        "int  $0x80\n"
    );
}
