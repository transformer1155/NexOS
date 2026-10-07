; =====================================================================
;  stage2_v86.asm - Stage 2 bootloader for the V86 (browser) target.
;
;  Loaded by Stage 1 at linear 0x8000 in 16-bit real mode.  Responsibilities:
;    1. Load kernel.bin from disk into memory at 0x10000.
;    2. Enter protected mode and jump to the kernel.
;
;  This is a reduced, self-contained Stage 2 used when booting the 32-bit
;  NexOS kernel under V86.  It deliberately keeps the video mode alone
;  (no VBE mode set, no VBE enumeration) so the kernel stays on the text
;  console, which is all the browser target needs and avoids the graphics
;  initialisation that faults under V86.
;
;  It carries the fix that matters for correctness under V86: ESI holds the
;  sector offset within the kernel while SI is borrowed for the DAP pointer,
;  and INT 13h may clobber SI, so ESI is saved across the call.
;
;  Build:  nasm -f bin stage2_v86.asm -o stage2_v86.bin   (exactly 16 KiB)
; =====================================================================
[BITS 16]
[ORG 0x8000]

KERNEL_LOAD_SEG equ 0x1000         ; kernel loads at 0x1000:0x0000 = 0x10000
KERNEL_LBA      equ 33             ; 1 (boot) + 32 (stage 2) = 33
KERNEL_SECTORS  equ 512            ; 256 KiB ceiling (kernel.bin ~227 KB)
KERNEL_ENTRY    equ 0x10000

start:
    mov [boot_drive], dl
    cmp dl, 0xF8                   ; SeaBIOS reports CD/HDD as 0xF8
    jne .drive_ok
    mov dl, 0x80
    mov [boot_drive], dl
.drive_ok:
    mov si, msg_hello
    call print_string

    ; ----- 1. Load the kernel, one sector per INT 13h call -----
    mov ax, KERNEL_SECTORS
    xor si, si
.load:
    or ax, ax
    jz .loaded
    mov [dap + 8], si
    add word [dap + 8], KERNEL_LBA
    mov word [dap + 2], 1
    push ax
    push si
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    pop si
    pop ax
    jc disk_error
    dec ax
    inc si
    add word [dap + 6], 0x20        ; one sector = 512 bytes = 0x20 segments
    jmp .load

.loaded:
    mov si, msg_kernel
    call print_string
    jmp enter_protected_mode

disk_error:
    mov si, msg_error
    call print_string
.hang:
    cli
    hlt
    jmp .hang

; =====================================================================
;  A20 + GDT + protected mode, then jump to the kernel
; =====================================================================
enter_protected_mode:
    cli
    in  al, 0x92                   ; fast A20
    test al, 2
    jnz .a20_done
    or  al, 2
    and al, 0xFE                   ; keep bit 0 (reset) clear
    out 0x92, al
.a20_done:
    lgdt [gdtr]

    mov eax, cr0
    or  eax, 1                     ; CR0.PE
    mov cr0, eax

    ; Far jump flushes the prefetch queue and loads CS.
    jmp CODE_SEG:init_pm

[BITS 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000               ; 32-bit stack
    jmp KERNEL_ENTRY

; The [BITS 32] above is sticky, so switch back before any more 16-bit code.
[BITS 16]

; =====================================================================
;  print_string - NUL-terminated string at DS:SI via BIOS teletype
; =====================================================================
print_string:
    pusha
    mov ah, 0x0E
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    popa
    ret

; =====================================================================
;  Data
; =====================================================================
boot_drive:  db 0
msg_hello:   db '[Stage2] Two-stage bootloader -> C++ kernel', 0x0D, 0x0A, 0
msg_kernel:  db '[Stage2] Kernel loaded, entering protected mode', 0x0D, 0x0A, 0
msg_error:   db '[Stage2] DISK READ ERROR!', 0x0D, 0x0A, 0

align 4
dap:
    db 0x10
    db 0
    dw 1
    dw 0x0000
    dw KERNEL_LOAD_SEG
    dq KERNEL_LBA

align 8
gdt_start:
gdt_null:
    dq 0
gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
gdt_end:

gdtr:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

times 16384-($-$$) db 0
