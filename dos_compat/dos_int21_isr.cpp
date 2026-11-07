// =====================================================================
//  dos_int21_isr.cpp  -  INT 21h / INT 20h DOS service interrupt entry
// ---------------------------------------------------------------------
//  The guest calls INT 21h with AH = function, and for string/file calls
//  it passes a pointer in DS:DX.  Because NexOS uses a FLAT 32-bit model
//  (all GDT bases 0) and the guest segment is identity-mapped, DS:DX is
//  simply a linear address -- no far-pointer arithmetic is needed.
//
//  Register preservation matters: a real DOS program expects INT 21h to
//  preserve every register except those the API documents as results
//  (notably AX).  The asm stub therefore saves/restores all GPRs.
// =====================================================================

#include <stdint.h>
#include "dos_int21.h"
#include "dos_prim.h"
#include "dos_int21_isr.h"

// ---------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------
static inline void dnote_outb(uint16_t p, uint8_t v){
    __asm__ __volatile__("outb %0,%1"::"a"(v),"Nd"(p));
}
static void serial_note(const char* s){
    while (*s) dnote_outb(0x3F8, (uint8_t)*s++);
    dnote_outb(0x3F8, '\n');
}

// Set/clear the carry flag inside the SAVED interrupt frame.  pushal does
// not save EFLAGS, but iret restores it, so we patch the frame in place.
//
// Frame layout when dos_int21_dispatch runs (low -> high address):
//     regs[0..7]  : the 8 GPRs saved by pushal
//     regs[8]     : EIP
//     regs[9]     : CS
//     regs[10]    : EFLAGS      <-- CF lives here
// The 4-byte argument the stub pushed was already popped before we run.
static void set_cf(uint32_t* regs, int on){
    uint32_t* eflags = regs + 10;
    if (on) *eflags |=  1u;          // CF
    else    *eflags &= ~1u;
}

// ---------------------------------------------------------------------
//  Shared asm body: push all GPRs, pass ESP as the argument (so the C side
//  can read the saved register block), call, restore, iret.
// ---------------------------------------------------------------------
#define DOS_ISR_BODY(label)                 \
    ".global " label "\n"                   \
    label ":\n"                             \
    "    pushal\n"                          \
    "    pushl %esp\n"                      \
    "    call dos_int21_dispatch\n"         \
    "    addl $4, %esp\n"                   \
    "    popal\n"                           \
    "    iret\n"

__asm__ (
    DOS_ISR_BODY("isr_int21")
    DOS_ISR_BODY("isr_int20")
);

// ---------------------------------------------------------------------
//  C dispatcher
// ---------------------------------------------------------------------
//  `regs` points at the block pushed by pushal.  pushal pushes in the
//  order EAX, ECX, EDX, EBX, original-ESP, EBP, ESI, EDI, so the LOWEST
//  address (regs[0]) holds EDI.  Getting this order wrong silently
//  corrupts every argument, so it is spelled out rather than assumed.
enum {
    R_EDI = 0, R_ESI, R_EBP, R_ESP_DUMMY, R_EBX, R_EDX, R_ECX, R_EAX
};

extern "C" void dos_int21_dispatch(uint32_t* regs){
    uint8_t  ah = (uint8_t)((regs[R_EAX] >> 8) & 0xFF);
    uint8_t  al = (uint8_t)(regs[R_EAX] & 0xFF);
    uint32_t edx = regs[R_EDX];

    switch (ah){
        // ---- AH=02h: output one character (DL) ----
        case 0x02:
            dos_console_putc((int)(edx & 0xFF));
            return;

        // ---- AH=09h: output '$'-terminated string at DS:DX ----
        case 0x09: {
            // Flat model: DS:DX is the linear address.  Guard against a bad
            // pointer so a buggy guest cannot take the kernel down.
            const char* s = (const char*)edx;
            if (edx < 0x1000 || edx >= 0x08000000u){
                serial_note("bad AH=09h ptr");
                return;
            }
            char buf[512];
            int n = 0;
            while (n < 511 && s[n] && s[n] != '$'){ buf[n] = s[n]; n++; }
            buf[n] = 0;
            dos_console_puts(buf);
            return;
        }

        // ---- AH=3Ch: create file (DS:DX = name, CX = attr) ----
        case 0x3C: {
            const char* nm = (const char*)edx;
            int h = dos_int21_create_file(nm, (int)(regs[R_ECX] & 0xFF));
            if (h < 0){
                // DOS reports failure via CF=1 and AX=error code.
                set_cf(regs, 1);
                regs[R_EAX] = (regs[R_EAX] & 0xFFFF0000u) | 0x0003u;  // path not found
            } else {
                set_cf(regs, 0);
                regs[R_EAX] = (regs[R_EAX] & 0xFFFF0000u) | ((uint32_t)h & 0xFFFFu);
            }
            return;
        }

        // ---- AH=3Dh: open file (DS:DX = name, AL = mode) ----
        case 0x3D: {
            const char* nm = (const char*)edx;
            int h = dos_int21_open_file(nm, (int)al);
            if (h < 0){ set_cf(regs, 1); regs[R_EAX] = 0x0002u; }   // file not found
            else      { set_cf(regs, 0); regs[R_EAX] = ((uint32_t)h & 0xFFFFu); }
            return;
        }

        // ---- AH=3Eh: close (BX = handle) ----
        case 0x3E: {
            int r = dos_int21_close_file((int)(regs[R_EBX] & 0xFFFF));
            set_cf(regs, r == 0 ? 0 : 1);
            return;
        }

        // ---- AH=3Fh: read (BX = handle, DS:DX = buf, CX = count) ----
        case 0x3F: {
            int got = 0;
            int r = dos_int21_read_file((int)(regs[R_EBX] & 0xFFFF),
                                        (void*)edx,
                                        (int)(regs[R_ECX] & 0xFFFF),
                                        &got);
            if (r != 0){ set_cf(regs, 1); regs[R_EAX] = 0x0005u; }
            else       { set_cf(regs, 0); regs[R_EAX] = (uint32_t)got; }
            return;
        }

        // ---- AH=40h: write (BX = handle, DS:DX = buf, CX = count) ----
        case 0x40: {
            int put = 0;
            int r = dos_int21_write_file((int)(regs[R_EBX] & 0xFFFF),
                                         (const void*)edx,
                                         (int)(regs[R_ECX] & 0xFFFF),
                                         &put);
            if (r != 0){ set_cf(regs, 1); regs[R_EAX] = 0x0005u; }
            else       { set_cf(regs, 0); regs[R_EAX] = (uint32_t)put; }
            return;
        }

        // ---- AH=4Ch: terminate with exit code AL ----
        case 0x4C:
            dos_int21_exit((int)al);
            return;                    // not reached when ring-3 is active

        // ---- AH=0Eh / AH=19h / AH=30h: benign queries ----
        case 0x19:
            regs[R_EAX] = (regs[R_EAX] & 0xFFFF0000u) | 2u;   // current drive = C:
            return;
        case 0x30: {
            regs[R_EAX] = (regs[R_EAX] & 0xFFFF0000u) | 7u;   // AL=major
            regs[R_EBX] = 0;                                   // BH:BL = 0
            regs[R_ECX] = 0;
            return;
        }

        default:
            serial_note("unhandled INT 21h AH");
            return;
    }
}