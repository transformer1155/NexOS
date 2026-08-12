; =====================================================================
;  switch32to64.asm  -  Transition from 32-bit PM to 64-bit long mode
; ---------------------------------------------------------------------
;  NASM ELF32 format, linked into the 32-bit kernel
;
;  void switch_to_64bit(uint32_t stage_phys);
;
;  The caller stages kernel64.bin in a heap buffer (`stage_phys`) instead of
;  reading it straight to 0x100000: the 32-bit kernel's own .bss sits at
;  0x120000 and a direct load corrupts the running ATA driver mid-read.
;  We blit the staged image down to 0x100000 here, where trashing 32-bit
;  globals no longer matters because nothing returns to C++.
; =====================================================================

[BITS 32]

; ---------------------------------------------------------------------
;  Long-mode page tables live at 0x90000-0x93FFF.
;
;  They used to sit at 0x60000, which was free when kernel.bin was small.
;  stage2 loads kernel.bin at 0x10000 (see stage2.asm KERNEL_SECTORS) and the
;  image is now >400 KiB, i.e. 0x10000-0x77E44 -- so the `rep stosd` that
;  clears the page-table pages was zeroing the *running kernel's own code*.
;  The symptom was a hang immediately after the 'S' debug char, before 'P'.
;
;  0x90000-0x9FBFF is the gap between the top of the 32/64-bit stack
;  (esp/rsp = 0x90000, grows DOWN) and the EBDA at 0x9FC00, so it stays clear
;  of both the kernel image and either stack.
; ---------------------------------------------------------------------
%define PML4_ADDR       0x90000
%define PDPT_ADDR       0x91000
%define PD_ADDR         0x92000
%define PD2_ADDR        0x93000       ; Extra PD for LFB (3GB-4GB range)
%define KERNEL64_ENTRY  0x100000
%define KERNEL64_DWORDS (1024 * 512 / 4)   ; 512 KiB staged image, in dwords

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
    ; cdecl argument: stage_phys.  Grab it before anything touches the stack.
    mov esi, [esp + 4]
    SERIAL_DBG 'S'         ; Start

    ; ---- Check if already in long mode (UEFI compat path) ----
    mov ecx, 0xC0000080          ; EFER MSR
    rdmsr
    test eax, (1 << 8)           ; EFER.LME
    jnz .compat_path

    ; =================================================================
    ;  BIOS 32-bit path: Full transition
    ; =================================================================

    ; ---- 0. Disable paging FIRST ----
    ; The 32-bit kernel runs with its own paging enabled and that mapping
    ; does NOT translate 0x90000 -> physical 0x90000 identically, so any
    ; page table we build at virtual 0x90000 would land on the wrong
    ; physical page while CR3 expects them at physical 0x90000 -> the CPU
    ; would triple-fault the instant long mode is entered.  Turn paging off
    ; here so every write below hits true physical RAM and CR3 lines up.
    mov eax, cr0
    and eax, 0x7FFFFFFF           ; Clear PG bit (bit 31)
    mov cr0, eax
    SERIAL_DBG 'd'         ; paging disabled (physical mode)

    ; ---- Blit the staged 64-bit image down to 0x100000 ----
    ; CRITICAL: this MUST run with paging OFF.  With paging on, writing the
    ; window 0x100000..0x180000 clobbers the 32-bit kernel's own page
    ; directory (g_page_directory_store @ 0x128000, inside the window) and
    ; every `movsd` past that point lands on the wrong physical page --
    ; the image tail (incl. gdt64 @ 0x1657E0) was garbage and the 64-bit
    ; entry #GP'd on `mov ds,0x10`.  With paging off, all writes are direct
    ; to physical RAM.  The blit also trashes .bss @ 0x120000..0x180000,
    ; which is deliberate: we never return to C++ from here.  switch_to_64bit
    ; itself lives at 0x10060 (< 1 MiB), so it is NOT overwritten.
    mov edi, KERNEL64_ENTRY
    mov ecx, KERNEL64_DWORDS
    cld
    rep movsd
    SERIAL_DBG 'M'         ; iMage copied into place

%macro DUMP_DW 1
    push eax
    push edx
    push ecx
    mov edx, 0x3F8
    mov eax, [%1]
    mov ecx, 8
%%loop:
    rol eax, 4
    and al, 0x0F
    cmp al, 9
    jbe %%digit
    add al, 0x37
    jmp %%out
%%digit:
    add al, 0x30
%%out:
    out dx, al
    loop %%loop
    mov al, 0x0A
    out dx, al
    pop ecx
    pop edx
    pop eax
%endmacro

    ; ---- TEMP DEBUG: verify the copied 64-bit GDT landed intact ----
    SERIAL_DBG 'Z'
    DUMP_DW 0x1657F0       ; gdt64[2] low  (expect FFFF)
    DUMP_DW 0x1657F4       ; gdt64[2] high (expect 00CF9200)
    DUMP_DW 0x165808       ; gdt64_desc limit+base_lo (expect 57E00027)
    DUMP_DW 0x16580C       ; gdt64_desc base_hi (expect 00000016)

    ; ---- 1. Build identity-mapped page tables (now at physical addrs) ----
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

    ; PDPT[0..7] -> 1GiB pages, identity mapped, present=1, writable=1, PS=1.
    ; This covers 0..8 GiB of physical RAM so GB-scale model weights (loaded
    ; from NTFS) are directly accessible. The 3-4GB VBE LFB range is naturally
    ; included, so no separate PD2 mapping is needed.
    mov edi, PDPT_ADDR
    mov eax, 0x00000083           ; addr=0 | P=1 | W=1 | PS=1 (1GiB page)
    mov ecx, 8
.fill_pdpt:
    mov [edi], eax
    add eax, 0x40000000           ; next 1GiB block
    add edi, 8
    loop .fill_pdpt

    popad

    SERIAL_DBG 'P'         ; Page tables built

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
    ; NOTE: must NOT be in 0x10000-0x90000 -- cmd_switch32() loads the
    ; 32-bit kernel.bin there and would corrupt a low stack mid-switch.
    mov rsp, 0x1F0000

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
