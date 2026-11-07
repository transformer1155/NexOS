// dos_exec.c - minimal .COM loader for NexOS
//
// .COM format: raw binary, max 64KB-256, loaded as a single segment.
// Standard layout:
//   - PSP at segment start (offset 0)
//   - program loaded at offset 0x100
//   - CS=DS=ES=SS = PSP segment
//   - IP = 0x100, SP = 0xFFFE (or top of segment)
//
// Exit strategy (two convergent paths):
//   * The stack is pre-loaded with a return address pointing at a tiny
//     in-guest exit stub, so a program's trailing RET terminates cleanly.
//   * INT 21h AH=4Ch reaches dos_int21_exit -> dos_process_exit ->
//     sys_exit_current(), which unwinds into enter_user's setjmp.
//
// IMPORTANT (why this is not just "jump to USER_BASE+0x100"):
//   NexOS enters ring 3 with a flat 32-bit model: CS=0x1B, SS=0x23, and
//   the GDT bases are all 0.  A .COM program written for real DOS assumes
//   DS=ES=CS=SS, because it addresses its data with a *segment-relative*
//   offset.  Those selectors must therefore be set BEFORE the first guest
//   instruction runs, or the very first `mov dx, msg` reads garbage and
//   the guest faults or hangs.  enter_user() only loads CS/SS, so this
//   loader drops to a small asm trampoline to fix DS/ES first.

#include <stdint.h>
#include "dos_exec.h"
#include "dos_psp.h"
#include "dos_int21.h"
#include "vfs.h"
#include "proc.h"
#include "mm.h"
#include "syscall.h"

extern "C" int enter_user(uint32_t entry, uint32_t stack_top);

// freestanding serial helpers (no libc in the kernel build)
static inline void outb(uint16_t p, uint8_t v){ __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p)); }
static void serial_puts(const char* s){ while(*s) outb(0x3F8,(uint8_t)*s++); }
static void serial_putdec(int v){
    char b[12]; int i = 0;
    if (v == 0) b[i++] = '0';
    else { int t = v; if (v < 0){ b[i++] = '-'; t = -t; }
           while (t){ b[i++] = (char)('0' + t % 10); t /= 10; } }
    for (int j = i - 1; j >= 0; j--) outb(0x3F8, (uint8_t)b[j]);
}
static void serial_puthex(uint32_t v){
    const char* H = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) outb(0x3F8, (uint8_t)H[(v >> (i * 4)) & 0xF]);
}

static void memzero(void* p, uint32_t n){
    uint8_t* d = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}
static void memcopy(void* dst, const void* src, uint32_t n){
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

// ---------------------------------------------------------------------
//  Guest layout inside USER_BASE (one 64 KiB DOS segment)
// ---------------------------------------------------------------------
//   +0x000  PSP (256 bytes)
//   +0x100  program image
//   +0xF000 exit stub  <- deliberately far above the image, so a large
//                         .COM cannot overwrite it (the old +0xF0 sat
//                         inside the PSP and was clobbered by the loader)
#define DOS_STUB_OFF   0xF000u
#define DOS_STACK_TOP  (USER_BASE + 0xFF00u)   // conventional .COM stack

// Ring-3 selectors, matching gdt.cpp (ucode 0x1B / udata 0x23).
#define SEL_UCODE 0x1Bu
#define SEL_UDATA 0x23u

// ---------------------------------------------------------------------
//  Guest entry trampoline
// ---------------------------------------------------------------------
// enter_user() performs the iret with CS/SS correct, but DS and ES still
// hold kernel selectors, and -- importantly -- it does NOT set EAX.
//
// An earlier version ended with `jmp eax`, assuming the loader had put the
// entry point in EAX.  Nothing guarantees that: EAX at iret time is
// whatever the kernel happened to leave behind.  That worked by luck for
// the first program and then jumped into the scratch buffer for the next
// one (observed as EIP = USER_BASE+0x1000C with a #PF).
//
// The trampoline therefore carries the entry address as an IMMEDIATE, so
// it is independent of every register the kernel might clobber:
//
//      mov edx, 0x23        ; ring-3 data selector
//      mov ds,  dx
//      mov es,  dx
//      mov eax, <entry>     ; baked in at install time
//      jmp eax
//
// It must be COPIED into the guest segment: these bytes live in the kernel
// image, whose pages are supervisor-only, so ring 3 cannot fetch them.
#define TRAMPOLINE_OFF 0xF200u   // between the image and the exit stub

static void install_exit_stub(uint32_t base){
    uint8_t* s = (uint8_t*)(base + DOS_STUB_OFF);
    uint32_t i = 0;
    // mov ax, 0x4C00
    s[i++] = 0xB8; s[i++] = 0x00; s[i++] = 0x4C;
    // int 21h
    s[i++] = 0xCD; s[i++] = 0x21;
    // int 20h (classic backup terminator)
    s[i++] = 0xCD; s[i++] = 0x20;
    // hlt ; jmp $-1   (if a terminator ever returned)
    s[i++] = 0xF4;
    s[i++] = 0xEB; s[i++] = 0xFD;
    while (i < 16) s[i++] = 0x90;   // pad with NOPs
}

static uint32_t install_trampoline(uint32_t base, uint32_t entry){
    uint8_t* t = (uint8_t*)(base + TRAMPOLINE_OFF);
    uint32_t i = 0;
    t[i++] = 0x66; t[i++] = 0xBA;                 // mov edx, imm32
    t[i++] = 0x23; t[i++] = 0x00; t[i++] = 0x00; t[i++] = 0x00;
    t[i++] = 0x8E; t[i++] = 0xDA;                 // mov ds, dx
    t[i++] = 0x8E; t[i++] = 0xC2;                 // mov es, dx
    t[i++] = 0xB8;                                // mov eax, imm32
    t[i++] = (uint8_t)(entry & 0xFF);
    t[i++] = (uint8_t)((entry >> 8) & 0xFF);
    t[i++] = (uint8_t)((entry >> 16) & 0xFF);
    t[i++] = (uint8_t)((entry >> 24) & 0xFF);
    t[i++] = 0xFF; t[i++] = 0xE0;                 // jmp eax
    return base + TRAMPOLINE_OFF;
}

// ---------------------------------------------------------------------
int dos_exec_com(const char *name){
    if (!name || !name[0]) return -1;

    uint32_t base = USER_BASE;
    uint32_t prog = base + 0x100;

    // --- read the image FIRST, into a scratch area at the top of the
    //     segment, so nothing we stage earlier can be clobbered by it.
    int fd = vfs_open(name, 0);
    if (fd < 0){
        serial_puts("[dos_exec] cannot open "); serial_puts(name); serial_puts("\n");
        return -1;
    }
    // A .COM may not exceed 64 KiB minus the PSP (0x100) and our stubs.
    // Read into a scratch window well clear of the PSP/stub region.
    uint8_t* scratch = (uint8_t*)(base + 0x10000u);   // second 64 KiB
    int sz = vfs_read(fd, scratch, 0xF000);
    vfs_close(fd);
    if (sz <= 0){
        serial_puts("[dos_exec] empty or unreadable\n");
        return -1;
    }
    if (sz > 0xF000){
        serial_puts("[dos_exec] image too large\n");
        return -1;
    }

    // --- stage the guest segment ---
    memzero((void*)base, 0x10000);

    dos_psp_build((void*)base, (uint16_t)(base >> 4), 0, "");

    install_exit_stub(base);
    uint32_t tramp = install_trampoline(base, prog);

    // Copy the image into the guest at +0x100.  Guard against overlapping
    // the exit stub / trampoline: those live at 0xF000 / 0xF200, so the
    // image must stay below 0xF000.  Reading them over would move control
    // to data and #PF (observed as EIP=USER_BASE+0xF00C, CR2=garbage).
    if (sz > (int)(0xF000 - 0x100)){
        serial_puts("[dos_exec] image overlaps stubs, refusing\n");
        return -1;
    }
    memcopy((void*)prog, scratch, (uint32_t)sz);

    serial_puts("[dos_exec] loaded ");
    serial_putdec(sz);
    serial_puts(" bytes at 0x"); serial_puthex(prog);
    serial_puts(" stub=0x");     serial_puthex(base + DOS_STUB_OFF);
    serial_puts(" tramp=0x");    serial_puthex(tramp);
    serial_puts("\n");

    // Seed the conventional stack so a trailing RET reaches the exit stub.
    // SP must point at the slot holding that return address.
    uint32_t ret_slot = DOS_STACK_TOP - 4;
    *(uint32_t*)ret_slot = base + DOS_STUB_OFF;

    Process* p = 0;
    if (proc_spawn(name, 1000, "/apps", &p) != 0){
        serial_puts("[dos_exec] proc_spawn failed\n");
        return -1;
    }
    p->entry = prog;

    Process* prev = g_current;
    g_current = p;
    serial_puts("[dos_exec] entering ring-3 (CS=0x1B DS=ES=0x23 SP=0x");
    serial_puthex(ret_slot);
    serial_puts(")\n");

    // Enter at the TRAMPOLINE: it sets DS/ES from ring 3, then jumps to the
    // real program.  `stack_top` becomes ESP, so it must equal ret_slot.
    serial_puts("[dos_exec] about to iret to ring-3...\n");
    int rc = enter_user(tramp, ret_slot);
    serial_puts("[dos_exec] iret returned!\n");

    g_current = prev;
    serial_puts("[dos_exec] returned from ring-3 rc=");
    serial_putdec(rc);
    serial_puts("\n");
    return rc;
}