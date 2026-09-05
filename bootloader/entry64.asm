; =====================================================================
;  entry64.asm  -  64-bit kernel entry point
; ---------------------------------------------------------------------
;  NASM ELF64 format, linked as first object in kernel64.elf
;  Entry address: 0x100000 (1MB), page-aligned for 2MB pages
;
;  Responsibilities:
;    1. Debug output ('6' on serial)
;    2. Set up 64-bit stack
;    3. Enable FPU + SSE
;    4. Load permanent GDT (with 64-bit + 32-bit compat segments)
;    5. Clear BSS
;    6. Call kmain64()
; =====================================================================

[BITS 64]

global _start64
extern kmain64
extern __bss_start
extern __bss_end

section .text

_start64:
    ; ---- Serial debug: '6' for 64-bit ----
    mov dx, 0x3F8
    mov al, 0x36           ; '6'
    out dx, al

    ; ---- Set up 64-bit stack ----
    mov rsp, 0x90000

    ; ---- Enable FPU (x87) + SSE ----
    mov rax, cr0
    and rax, ~(1 << 2)     ; Clear EM (use FPU, not emulation)
    and rax, ~(1 << 3)     ; Clear TS
    or rax, (1 << 1)       ; Set MP
    or rax, (1 << 5)       ; Set NE
    mov cr0, rax

    mov rax, cr4
    or rax, (1 << 9)       ; OSFXSR (SSE)
    or rax, (1 << 10)      ; OSXMMEXCPT
    mov cr4, rax

    fninit                  ; Init x87 FPU

    ; ---- Load permanent GDT ----
    lgdt [rel gdt64_desc]

    ; Reload segment registers with new GDT selectors
    mov ax, 0x10            ; 64-bit data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far jump to refresh CS with 64-bit code segment
    ; (Not strictly necessary since we're already in 64-bit, but clean)
    ; Skip far jump - CS is already 0x08 from switch32to64

    ; ---- Clear BSS ----
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi            ; byte count
    xor rax, rax
    cld
    rep stosb

    ; ---- Call C++ kernel ----
    call kmain64

.hang:
    cli
    hlt
    jmp .hang

; =====================================================================
;  GDT for 64-bit kernel
;  Contains both 64-bit and 32-bit compat segments (for switch back)
; =====================================================================
section .data
align 8

gdt64:
    dq 0x0000000000000000       ; 0x00: Null descriptor
    dq 0x00AF9A000000FFFF       ; 0x08: 64-bit code (L=1, D=0, G=1)
    dq 0x00CF92000000FFFF       ; 0x10: 64-bit data (flat, G=1, D=1)
    dq 0x00CF9A000000FFFF       ; 0x18: 32-bit compat code (L=0, D=1)
    dq 0x00CF92000000FFFF       ; 0x20: 32-bit data (flat)
gdt64_end:

; 64-bit GDT descriptor (10 bytes: 2 limit + 8 base)
gdt64_desc:
    dw gdt64_end - gdt64 - 1
    dq gdt64
