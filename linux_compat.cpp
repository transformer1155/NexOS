// linux_compat.cpp — Milestone 0 of Wine-on-NexOS.
//
// A minimal Linux binary-compatibility shim:
//   * int 0x80 syscall dispatcher (Linux i386 ABI: eax=sysno, ebx..ebp=args)
//   * flat ELF32 loader (PT_LOAD only, identity-mapped address space)
//   * runs the guest in RING 0 (de-risked; ring-3 isolation is a later milestone)
//
// This is the host ABI Wine's Linux backend requires. Once enough syscalls
// exist and ring-3 + per-process paging land, a cross-built Wine can sit on
// top and, in turn, run Win32 binaries (including, long-term, Windows Chrome).

#include "linux_compat.h"
#include "setjmp.h"      // shared Ctx + mini_setjmp/mini_longjmp
#include "syscall.h"     // shared SysRegs
#include <stdint.h>

// ---- serial output (self-contained; mirrors kernel's 0x3F8 port) ----
static inline void outb(uint16_t port, uint8_t val){
    __asm__ __volatile__("outb %0, %1" :: "a"(val), "Nd"(port));
}
static void serial_puts(const char* s){ while(*s) outb(0x3F8, (uint8_t)*s++); }

// ---- registered file reader (SFS) ----
static int (*g_reader)(const char*, unsigned char*, int) = 0;

// setjmp/longjmp-style resume context: lets a guest sys_exit unwind straight
// back into linux_run() and return cleanly to its caller (cmd_linux), without
// any fragile in-function label / inline-asm ESP tricks.
// (Ctx is defined in setjmp.h; mini_setjmp/mini_longjmp are the naked asm
//  functions implemented further below.)
static Ctx     g_ctx = {};
static int      g_linux_exit_code = 0;

// =========================================================================
//  Syscall dispatch (Linux i386 numbers)
// =========================================================================
static void serial_putdec(int v){
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { if (v < 0){ b[i++]='-'; v=-v; } while(v){ b[i++] = (char)('0' + v%10); v/=10; } }
    for (int j = i-1; j >= 0; j--) outb(0x3F8, (uint8_t)b[j]);
}

// Minimal freestanding setjmp/longjmp (i386), written as NAKED asm functions
// so we control the prologue/epilogue and can read the true return address.
// Ctx layout: { ebp, ebx, esi, edi, esp, eip } at offsets 0,4,8,12,16,20.
extern "C" int __attribute__((naked)) mini_setjmp(Ctx*){
    __asm__ __volatile__(
        "movl 4(%esp), %eax\n"       // eax = c (first cdecl arg)
        "movl %ebp, 0(%eax)\n"
        "movl %ebx, 4(%eax)\n"
        "movl %esi, 8(%eax)\n"
        "movl %edi, 12(%eax)\n"
        "movl %esp, 16(%eax)\n"      // esp points at this fn's return address
        "movl 0(%esp), %ecx\n"       // return address into caller (linux_run)
        "movl %ecx, 20(%eax)\n"
        "xorl %eax, %eax\n"          // setjmp returns 0 on first call
        "ret\n"
    );
    __builtin_unreachable();
}

extern "C" void __attribute__((naked)) mini_longjmp(Ctx*, int){
    __asm__ __volatile__(
        "movl 4(%esp), %eax\n"       // eax = c
        "movl 0(%eax), %ebp\n"
        "movl 4(%eax), %ebx\n"
        "movl 8(%eax), %esi\n"
        "movl 12(%eax), %edi\n"
        "movl 16(%eax), %esp\n"      // restore esp (points at ret-addr slot)
        "movl 20(%eax), %ecx\n"      // return address into linux_run
        "movl $1, %eax\n"            // setjmp returns nonzero on longjmp
        "movl %ecx, (%esp)\n"        // place ret addr on the restored stack
        "ret\n"                      // return into linux_run after setjmp
    );
    __builtin_unreachable();
}

extern "C" void linux_syscall_dispatch(SysRegs* r){
    uint32_t num = r->eax;
    serial_puts("[sc "); serial_putdec((int)num); serial_puts("]");
    switch (num){
        case 1: { // sys_exit
            g_linux_exit_code = (int)r->ebx;
            // Unwind straight back into linux_run() (the setjmp return),
            // which will print the exit notice and return to its caller.
            mini_longjmp(&g_ctx, 1);
            __builtin_unreachable();
        }
        case 3: // sys_read  -> EOF for now
            r->eax = 0;
            return;
        case 4: { // sys_write  (fd 1/2 -> serial)
            int fd = (int)r->ebx;
            const char* buf = (const char*)r->ecx;
            uint32_t count = r->edx;
            if (fd == 1 || fd == 2){
                for (uint32_t i = 0; i < count; i++) outb(0x3F8, (uint8_t)buf[i]);
            }
            r->eax = count;
            return;
        }
        case 6: // sys_close
            r->eax = 0;
            return;
        case 20: // sys_getpid
            r->eax = 1;
            return;
        case 45: { // sys_brk (single growing break, benign)
            static uint32_t brk = 0x01800000;
            r->eax = brk;
            return;
        }
        case 90: // sys_mmap (old_mmap) — stub: report failure for now
            r->eax = (uint32_t)-1;
            return;
        case 122: // sys_uname — stub
            r->eax = 0;
            return;
        default:
            r->eax = (uint32_t)-1; // -ENOSYS
            return;
    }
}

// =========================================================================
//  int 0x80 trap entry (C stub wraps the asm handler)
// =========================================================================
__asm__(
    ".global linux_syscall_entry\n"
    "linux_syscall_entry:\n"
    "    pushl %ebp\n"   // arg5  (pushed first -> highest address)
    "    pushl %edi\n"   // arg4
    "    pushl %esi\n"   // arg3
    "    pushl %edx\n"   // arg2
    "    pushl %ecx\n"   // arg1
    "    pushl %ebx\n"   // arg0
    "    pushl %eax\n"   // sysno  (pushed last -> lowest address == SysRegs.eax)
    "    movl %esp, %eax\n"
    "    pushl %eax\n"   // cdecl: pass SysRegs* as the first stack argument
    "    call linux_syscall_dispatch\n"
    "    addl $32, %esp\n"   // pop 7 registers + 1 argument
    "    iret\n"
);

// =========================================================================
//  ELF32 loader + execution
// =========================================================================
int linux_run(const char* name){
    if (!g_reader){ serial_puts("linux: no file reader registered\n"); return -1; }

    static unsigned char elf[65536];
    int sz = g_reader(name, elf, (int)sizeof(elf));
    if (sz <= 0){ serial_puts("linux: file not found: "); serial_puts(name); serial_puts("\n"); return -1; }

    // ELF magic + class check
    if (elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F'){
        serial_puts("linux: not an ELF image\n"); return -1;
    }
    if (elf[4] != 1){ serial_puts("linux: not ELF32\n"); return -1; } // 1 = 32-bit

    uint32_t  entry    = *(uint32_t*)(elf + 24);          // e_entry
    uint32_t  phoff    = *(uint32_t*)(elf + 28);          // e_phoff
    uint16_t  phentsz  = *(uint16_t*)(elf + 42);          // e_phentsize
    uint16_t  phnum    = *(uint16_t*)(elf + 44);          // e_phnum

    serial_puts("linux: loading "); serial_puts(name);
    serial_puts(" entry=0x");
    {
        static const char* hexd = "0123456789ABCDEF";
        char hb[9]; int hi = 0;
        for (int s = 24; s >= 0; s -= 8){
            uint8_t b = (uint8_t)(entry >> s);
            hb[hi++] = hexd[(b >> 4) & 0xF]; hb[hi++] = hexd[b & 0xF];
        }
        hb[hi] = 0; serial_puts(hb);
    }
    serial_puts("\n");

    // Map PT_LOAD segments into the identity-mapped address space (< 32 MiB).
    for (int i = 0; i < phnum; i++){
        unsigned char* ph = elf + phoff + (uint32_t)i * phentsz;
        uint32_t p_type = *(uint32_t*)(ph + 0);
        if (p_type != 1) continue; // PT_LOAD == 1
        uint32_t p_offset = *(uint32_t*)(ph + 4);
        uint32_t p_vaddr  = *(uint32_t*)(ph + 8);
        uint32_t p_filesz = *(uint32_t*)(ph + 16);
        uint32_t p_memsz  = *(uint32_t*)(ph + 20);
        serial_puts("  PH"); serial_putdec(i); serial_puts(" vaddr=0x");
        { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
          uint32_t v=p_vaddr; for(int s=28;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
        serial_puts(" off=0x");
        { static const char* h="0123456789ABCDEF"; char b[9]; int bi=0;
          uint32_t v=p_offset; for(int s=28;s>=0;s-=8){ uint8_t x=(uint8_t)(v>>s); b[bi++]=h[x>>4]; b[bi++]=h[x&0xF]; } b[bi]=0; serial_puts(b); }
        serial_puts("\n");
        if (p_vaddr + p_memsz > 0x02000000){
            serial_puts("linux: segment above 32 MiB identity map\n"); return -1;
        }
        for (uint32_t j = 0; j < p_filesz; j++)
            *(unsigned char*)(p_vaddr + j) = elf[p_offset + j];
        for (uint32_t j = p_filesz; j < p_memsz; j++)
            *(unsigned char*)(p_vaddr + j) = 0; // zero .bss
    }
    serial_puts("  entry bytes: ");
    for (int k = 0; k < 8; k++){
        uint8_t v = *(unsigned char*)(entry + k);
        static const char* h = "0123456789ABCDEF"; char b2[3];
        b2[0] = h[v >> 4]; b2[1] = h[v & 0xF]; b2[2] = 0; serial_puts(b2);
    }
    serial_puts("\n");

    // Dedicated guest stack (within 32 MiB, above the kernel heap).
    uint32_t stack_top = 0x017FF000;

    // Capture the kernel resume point. On a guest sys_exit, mini_longjmp()
    // unwinds back here with a nonzero value and we return cleanly.
    if (mini_setjmp(&g_ctx) != 0){
        serial_puts("linux: process exited\n");
        return g_linux_exit_code;
    }

    // First entry: switch to the guest stack and jump to the ELF entry point.
    __asm__ __volatile__(
        "mov %[stack], %%esp\n"
        "jmp *%[entry]\n"
        :: [stack] "r" (stack_top), [entry] "r" (entry)
        : "memory");
    __builtin_unreachable();
}

void linux_compat_init(int (*reader)(const char*, unsigned char*, int)){
    g_reader = reader;
}
