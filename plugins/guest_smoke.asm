; =====================================================================
;  guest_smoke.asm  -  the minimal guest program for the NexOS built-in VMM
; =====================================================================
;
;  PURPOSE
;  -------
;  The smallest payload that proves the whole virtualization chain works:
;
;    1. It runs INSIDE a hardware-virtualized guest (AMD SVM or Intel VMX).
;    2. It paints the SHARED framebuffer -- the same physical RAM NexOS maps
;       directly, so the NexOS window shows it with zero copying (no VNC).
;    3. It writes "ready\n" to the emulated UART (vUART), exercising the same
;       line protocol the real Linux guest will use.
;    4. It does `OUT` to the exit port, trapping back to the hypervisor.
;
;  BUILD
;  -----
;    nasm -f bin guest_smoke.asm -o guest_smoke.bin
;    ./tools/gen_guest_blob.sh          # regenerates plugins/guest_blob.h
;
;  `-f bin` gives a flat binary: no ELF headers, no relocations, just bytes
;  to drop at GUEST_CODE_PHYS.
;
;  GUEST MEMORY MAP (guest-physical, chosen by vmm.cpp)
;  ---------------------------------------------------
;    GUEST_CODE_PHYS  0x00001000  this code
;    GUEST_FB_PHYS    0x00100000  shared framebuffer, 1024x768x4 = 3 MiB
;    UART port        0x03F8      OUT = one byte to the host
;    EXIT port        0x0F00      OUT = "raise a VM-exit, I'm done"
;
;  REGISTER PLAN (no div -- integer division is slow and easy to get wrong)
;  -----------------------------------------------------------------------
;    edi = framebuffer write pointer
;    ebx = x  (0..GUEST_FB_W-1)
;    ecx = y  (0..GUEST_FB_H-1)
;    eax = accumulated pixel colour
;    edx = scratch channel value
;    esi = scratch / message pointer
; =====================================================================

BITS 32

; The payload is loaded and executed at GUEST_CODE_PHYS (0x1000).  `org` is
; essential: without it, `mov esi, msg_ready` would assemble the symbol's
; FILE OFFSET (0x53) instead of its runtime address (0x1053), and the guest
; would print garbage read from low memory.
org 0x1000

GUEST_FB_PHYS   equ 0x00100000
GUEST_FB_W      equ 1024
GUEST_FB_H      equ 768
UART_PORT       equ 0x03F8
EXIT_PORT       equ 0x0F00

start:
    ; ---------------------------------------------------------------
    ; 1) Paint the shared framebuffer with a gradient.
    ;    Pixel format is NexOS' 0x00RRGGBB.
    ;      red   = x >> 2   (0..255 across the width)
    ;      green = y >> 1   (0..255 down the height)
    ;      blue  = 0x80     (constant, so the pattern is unmistakable)
    ; ---------------------------------------------------------------
    mov     edi, GUEST_FB_PHYS
    xor     ecx, ecx                    ; ecx = y = 0

.y_loop:
    xor     ebx, ebx                    ; ebx = x = 0

.x_loop:
    ; green = (y >> 1) << 8
    mov     eax, ecx
    shr     eax, 1
    shl     eax, 8

    ; red = (x >> 2) << 16
    mov     edx, ebx
    shr     edx, 2
    shl     edx, 16
    or      eax, edx

    ; blue = 0x80
    or      eax, 0x00000080

    ; *edi++ = colour
    mov     [edi], eax
    add     edi, 4

    inc     ebx
    cmp     ebx, GUEST_FB_W
    jb      .x_loop

    inc     ecx
    cmp     ecx, GUEST_FB_H
    jb      .y_loop

    ; ---------------------------------------------------------------
    ; 2) Announce readiness on the emulated UART.  These bytes land in the
    ;    host's vUART ring and are consumed by the same line parser that
    ;    handles "start"/"stop"/"ready"/"down".
    ; ---------------------------------------------------------------
    mov     esi, msg_ready
.putc:
    mov     al, [esi]
    test    al, al
    jz      .uart_done
    mov     dx, UART_PORT
    out     dx, al
    inc     esi
    jmp     .putc
.uart_done:

    ; ---------------------------------------------------------------
    ; 3) Signal completion: OUT to the exit port raises a VM-exit, handing
    ;    control back to the hypervisor.  This proves guest->host works.
    ; ---------------------------------------------------------------
    mov     dx, EXIT_PORT
    mov     al, 0x42                    ; magic "I finished" byte
    out     dx, al

.halt:
    hlt
    jmp     .halt

; Data after the code, still in the same page.
msg_ready:  db "ready", 10, 0
