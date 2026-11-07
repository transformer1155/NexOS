; =====================================================================
;  spinloop.S  -  the most trivial possible ring-3 payload
; ---------------------------------------------------------------------
;  Literally:  jmp $      (EB FE)
;
;  Purpose: separate two hypotheses for the ring-3 hang.
;    (a) the `iret` in enter_user() never succeeds  -> nothing at all
;        happens, and this probe behaves exactly like the others.
;    (b) the iret DOES land in ring 3, but executing real work (int 0x80,
;        segment reloads, memory access) is what fails.
;
;  If (a), even an infinite loop of two bytes hangs identically.  If (b),
;  this also hangs, but for a different reason -- so we additionally poke a
;  byte to a KNOWN-WRITABLE address first.  That write is the discriminator:
;  if the kernel can observe the byte changing, ring-3 code really ran.
; =====================================================================

BITS 32
section .text
global _start

; A scratch byte the kernel can poll.  Placed in the guest image itself, so
; writing it proves the guest executed at least one store.
%define FLAG_ADDR 0x04000800

_start:
        ; store 0xAB to a fixed address, then spin.
        mov     byte [FLAG_ADDR], 0xAB
spin:
        jmp     spin