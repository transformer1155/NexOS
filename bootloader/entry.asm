; =====================================================================
;  entry.asm  -  Kernel entry stub (32-bit protected mode)
; ---------------------------------------------------------------------
;  This object is linked FIRST so its code occupies the very first
;  bytes of the kernel image. Because the kernel is loaded at and
;  linked to 0x10000, `_start` then sits exactly at 0x10000, which is
;  where Stage 2 jumps.
;
;  It establishes a stack, enables the FPU + SSE, zeroes the .bss
;  section, and calls kmain.
;
;  Build:  nasm -f elf32 entry.asm -o entry.o
; =====================================================================

[BITS 32]
[GLOBAL _start]
[GLOBAL fpu_init]
[EXTERN kmain]
[EXTERN __bss_start]
[EXTERN __bss_end]

section .text
_start:
    ; Debug: output 'S' to serial port 0x3F8
    mov  dx, 0x3F8
    mov  al, 0x53               ; 'S' = kernel _start reached
    out  dx, al

    mov  esp, 0x90000              ; establish a valid 32-bit stack

    ; ---- Enable FPU (x87) + SSE ----
    ; CR0: clear EM (bit 2), set MP (bit 1), set NE (bit 5), clear TS (bit 3)
    mov  eax, cr0
    and  eax, ~(1 << 2)           ; clear EM - no FPU emulation
    and  eax, ~(1 << 3)           ; clear TS - no task switch delay
    or   eax, (1 << 1)            ; set MP - monitor coprocessor
    or   eax, (1 << 5)            ; set NE - native FPU error reporting
    mov  cr0, eax

    ; CR4: set OSFXSR (bit 9) for SSE, set OSXMMEXCPT (bit 10)
    mov  eax, cr4
    or   eax, (1 << 9)            ; OSFXSR - enable SSE state save/restore
    or   eax, (1 << 10)           ; OSXMMEXCPT - enable SSE exceptions
    mov  cr4, eax

    ; Initialize the FPU
    fninit

    ; Debug: output 'F' = FPU enabled
    mov  dx, 0x3F8
    mov  al, 0x46               ; 'F' = FPU initialised
    out  dx, al

    ; ---- Zero the .bss section ----
    mov  edi, __bss_start
    mov  ecx, __bss_end
    sub  ecx, edi                  ; byte count
    xor  eax, eax                  ; fill with zeros
    cld
    rep  stosb

    ; Debug: output 'B' = BSS zeroed
    mov  dx, 0x3F8
    mov  al, 0x42               ; 'B' = BSS cleared
    out  dx, al

    

    call kmain                     ; enter C++ code
.hang:
    cli
    hlt
    jmp .hang

; =====================================================================
;  fpu_init  -  Re-initialise FPU (callable from C)
; =====================================================================
fpu_init:
    fninit
    ret
