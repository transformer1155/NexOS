; =====================================================================
;  switch32to64.asm  -  Transition from 32-bit PM to 64-bit long mode
; ---------------------------------------------------------------------
;  NASM ELF32 format, linked into the 32-bit kernel
;
;  void switch_to_64bit(void);
;  Assumes kernel64.bin is already loaded at 0x100000 (1MB) by caller
; =====================================================================

[BITS 32]

%define PML4_ADDR       0x60000
%define PDPT_ADDR       0x61000
%define PD_ADDR         0x62000
%define PD2_ADDR        0x63000       ; Extra PD for LFB (3GB-4GB range)
%define KERNEL64_ENTRY  0x100000

global switch_to_64bit

section .text

; ---- Serial debug helper (outputs one char to port 0x3F8) ----
%macro SERIAL_DBG 1
    pushfd
    push eax
    push dx
    mov dx, 0x3F8
    mov al, %1
    out dx, al
    pop dx
    pop eax
    popfd
%endmacro

switch_to_64bit:
    cli
    SERIAL_DBG 'S'         ; Start

    ; ---- Check if already in long mode (UEFI compat path) ----
    mov ecx, 0xC0000080          ; EFER MSR
    rdmsr
    test eax, (1 << 8)           ; EFER.LME
    jnz .compat_path

    ; =================================================================
    ;  BIOS 32-bit path: Full transition
    ; =================================================================

    ; ---- 1. Build identity-mapped page tables ----
    ; Use pushad/popad to preserve registers
    pushad
    ; Clear 4 pages at PML4_ADDR (16384 bytes = 4096 dwords)
    mov edi, PML4_ADDR
    mov ecx, 4096
    xor eax, eax
    cld
    rep stosd

    ; PML4[0] -> PDPT (present=1, writable=1)
    mov dword [PML4_ADDR], PDPT_ADDR | 0x03

    ; PDPT[0] -> PD (present=1, writable=1)
    mov dword [PDPT_ADDR], PD_ADDR | 0x03

    ; PD[0..511] -> 2MB pages, identity mapped, present=1, writable=1, PS=1
    mov edi, PD_ADDR
    mov eax, 0x00000083           ; addr=0 | P=1 | W=1 | PS=1
    mov ecx, 512
.fill_pd:
    mov [edi], eax
    add eax, 0x200000             ; next 2MB block
    add edi, 8
    loop .fill_pd

    ; ---- Map VBE LFB area (3GB-4GB) if VBE was set up ----
    ; Check vbe_ok flag at 0x500D
    mov al, [0x500D]
    test al, al
    jz .no_lfb

    ; Clear PD2 page (1024 dwords = 4096 bytes)
    mov edi, PD2_ADDR
    mov ecx, 1024
    xor eax, eax
    rep stosd

    ; PDPT[3] -> PD2 (covers 3GB-4GB, present=1, writable=1)
    mov dword [PDPT_ADDR + 3*8], PD2_ADDR | 0x03

    ; PD2[0..511] -> 2MB pages, identity mapped (3GB-4GB)
    mov edi, PD2_ADDR
    mov eax, 0xC0000083          ; addr=3GB | P=1 | W=1 | PS=1
    mov ecx, 512
.fill_pd2:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .fill_pd2

.no_lfb:
    popad

    SERIAL_DBG 'P'         ; Page tables built

    ; ---- 2. Disable paging (CR0.PG = 0) ----
    ; Must disable paging BEFORE modifying CR4.PSE
    mov eax, cr0
    and eax, 0x7FFFFFFF           ; Clear PG bit (bit 31)
    mov cr0, eax

    SERIAL_DBG 'D'         ; Paging disabled

    ; ---- 3. Set up CR4: clear PSE, enable PAE ----
    mov eax, cr4
    and eax, 0xFFFFFFEF           ; Clear PSE (bit 4)
    or eax, 0x00000020            ; Set PAE (bit 5)
    mov cr4, eax

    SERIAL_DBG 'A'         ; PAE enabled

    ; ---- 4. Load CR3 with PML4 address ----
    mov eax, PML4_ADDR
    mov cr3, eax

    SERIAL_DBG 'C'         ; CR3 loaded

    ; ---- 5. Set EFER.LME (MSR 0xC0000080, bit 8) ----
    mov ecx, 0xC0000080
    rdmsr
    or eax, 0x00000100            ; Set LME (bit 8)
    wrmsr

    SERIAL_DBG 'E'         ; EFER.LME set

    ; ---- 6. Enable paging (CR0.PG = bit 31) ----
    ; This is the magic moment: long mode activates!
    ; The CPU enters IA-32e compatibility mode (32-bit code in long mode)
    mov eax, cr0
    or eax, 0x80000000            ; Set PG bit
    mov cr0, eax

    ; If we get here, long mode is active in compatibility mode
    SERIAL_DBG 'L'         ; Long mode enabled

    ; ---- 7. Load 64-bit GDT ----
    lgdt [gdt64_desc]

    SERIAL_DBG 'G'         ; GDT loaded

    ; ---- 8. Far jump to 64-bit code segment (0x08) ----
    ; This switches from compat mode to true 64-bit mode
    jmp 0x08:long_mode_entry

.compat_path:
    ; =================================================================
    ;  UEFI compat mode path: Already in long mode, just switch CS
    ; =================================================================
    SERIAL_DBG 'U'         ; UEFI compat path
    lgdt [gdt64_desc]
    jmp 0x08:long_mode_entry

; =====================================================================
;  64-bit code entry point
; =====================================================================
[BITS 64]
long_mode_entry:
    ; We are now in 64-bit long mode!

    ; Serial debug: '6' for 64-bit entry
    mov dx, 0x3F8
    mov al, 0x36           ; '6'
    out dx, al

    ; Load flat data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Set up 64-bit stack
    mov rsp, 0x90000

    ; Ensure interrupts are off
    cli

    ; Jump to the 64-bit kernel at 0x100000 (1MB)
    mov rax, KERNEL64_ENTRY
    jmp rax

; =====================================================================
;  64-bit GDT (used during transition)
; =====================================================================
section .data
align 8

gdt64:
    dq 0x0000000000000000       ; 0x00: Null descriptor

gdt64_code:                     ; 0x08: 64-bit code (L=1, D=0)
    dq 0x00AF9A000000FFFF       ; P=1 DPL=0 S=1 Type=code G=1 L=1

gdt64_data:                     ; 0x10: Data (flat, G=1, D=1)
    dq 0x00CF92000000FFFF       ; P=1 DPL=0 S=1 Type=data G=1 D=1

gdt64_end:

; GDT descriptor (32-bit format: 2-byte limit + 4-byte base)
gdt64_desc:
    dw gdt64_end - gdt64 - 1
    dd gdt64
