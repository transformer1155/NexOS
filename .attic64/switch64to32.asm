; =====================================================================
;  switch64to32.asm  -  Transition from 64-bit long mode to 32-bit PM
; ---------------------------------------------------------------------
;  NASM ELF64 format, linked into the 64-bit kernel
;
;  void switch_to_32bit(void);
;  Assumes kernel32.bin is already loaded at 0x10000 by caller
;
;  Steps:
;    1. Far return to 32-bit compatibility mode (CS=0x18, L=0)
;    2. In compat mode: disable paging (CR0.PG=0)
;       -> CPU exits long mode, enters 32-bit PM without paging
;    3. Clear EFER.LME
;    4. Clear CR4.PAE
;    5. Load 32-bit GDT
;    6. Far jump to 32-bit code segment -> kernel32 at 0x10000
; =====================================================================

[BITS 64]

%define KERNEL32_ENTRY  0x10000
%define COMPAT_CS       0x18      ; 32-bit compat code segment in GDT
%define PM32_CS         0x08      ; 32-bit code segment in new GDT

global switch_to_32bit

section .text

switch_to_32bit:
    cli

    ; ---- 1. Switch to 32-bit compatibility mode ----
    ; In 64-bit mode, we can't use 'jmp far'. Use 'retfq' instead.
    ; Push CS (32-bit compat) then RIP (compat32_code) onto stack.
    mov rax, COMPAT_CS
    push rax                 ; [rsp+8] = CS
    lea rax, [rel compat32_code]
    push rax                 ; [rsp]   = RIP
    retfq                    ; Far return -> compat mode

; =====================================================================
;  32-bit compatibility mode code
;  Still in long mode (EFER.LME=1), but executing 32-bit instructions
; =====================================================================
[BITS 32]
compat32_code:
    ; ---- 2. Disable paging (CR0.PG = 0) ----
    ; In compat mode, clearing PG exits long mode -> 32-bit PM, no paging
    mov eax, cr0
    and eax, ~(1 << 31)      ; Clear PG bit
    mov cr0, eax

    ; ---- 3. Clear EFER.LME ----
    mov ecx, 0xC0000080
    rdmsr
    and eax, ~(1 << 8)       ; Clear LME
    wrmsr

    ; ---- 4. Clear CR4.PAE ----
    mov eax, cr4
    and eax, ~(1 << 5)       ; Clear PAE
    mov cr4, eax

    ; ---- 5. Set up 32-bit stack ----
    mov esp, 0x90000

    ; ---- 6. Load 32-bit GDT ----
    lgdt [gdt32_desc]

    ; ---- 7. Far jump to 32-bit kernel ----
    ; Load CS=0x08 (32-bit code) and jump to 0x10000
    jmp PM32_CS:KERNEL32_ENTRY

; =====================================================================
;  32-bit GDT (for post-transition use)
; =====================================================================
section .data
align 8

gdt32:
    dq 0x0000000000000000       ; 0x00: Null
    dq 0x00CF9A000000FFFF       ; 0x08: 32-bit code (G=1, D=1)
    dq 0x00CF92000000FFFF       ; 0x10: 32-bit data (flat)
gdt32_end:

; GDT descriptor (32-bit format: 2-byte limit + 4-byte base)
gdt32_desc:
    dw gdt32_end - gdt32 - 1
    dd gdt32
