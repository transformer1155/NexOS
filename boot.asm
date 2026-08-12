; =====================================================================
;  boot.asm  -  Stage 1 Bootloader (512-byte boot sector)
; ---------------------------------------------------------------------
;  Loaded by the BIOS at 0x7C00 in 16-bit real mode.
;  Responsibilities:
;    1. Save the boot drive number passed by the BIOS in DL.
;    2. Load Stage 2 from disk (LBA 1) into memory at 0x8000 using
;       INT 13h Extended Read (LBA addressing).
;    3. Far-jump to Stage 2, passing the boot drive in DL.
;
;  Build:  nasm -f bin boot.asm -o boot.bin      (exactly 512 bytes)
; =====================================================================

[BITS 16]
[ORG 0x7C00]

; ----- Layout constants -----
STAGE2_LOAD_SEG    equ 0x0000      ; load Stage 2 at linear 0x0000:0x8000 = 0x8000
STAGE2_LOAD_OFF    equ 0x8000
STAGE2_LBA         equ 1           ; Stage 2 begins at LBA 1 (right after this 512B sector)
STAGE2_SECTORS     equ 32          ; 32 sectors = 16 KiB (Stage 2 is padded to this size)

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                 ; stack grows downward from the load address
    mov [boot_drive], dl           ; preserve BIOS boot drive number

    mov si, msg_loading
    call print_string

    ; ----- Load Stage 2 with INT 13h AH=42h (Extended Read, LBA) -----
    mov si, dap_stage2
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    mov si, msg_done
    call print_string

    ; ----- Hand off to Stage 2 -----
    mov dl, [boot_drive]           ; pass boot drive to Stage 2 via DL
    jmp STAGE2_LOAD_SEG:STAGE2_LOAD_OFF

disk_error:
    mov si, msg_error
    call print_string
.hang:
    cli
    hlt
    jmp .hang

; =====================================================================
;  print_string  -  print a NUL-terminated string at DS:SI via BIOS
;                   teletype (INT 10h / AH=0Eh).
; =====================================================================
print_string:
    pusha
    mov ah, 0x0E
.loop:
    lodsb                          ; AL = [DS:SI]; SI++
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
boot_drive:   db 0
msg_loading:  db '[Stage1] Loading Stage2 from disk...', 0x0D, 0x0A, 0
msg_done:     db '[Stage1] Stage2 loaded, jumping...',   0x0D, 0x0A, 0
msg_error:    db '[Stage1] DISK READ ERROR!',            0x0D, 0x0A, 0

; ----- Disk Address Packet (DAP) for Stage 2 load -----
align 4
dap_stage2:
    db 0x10                         ; size of DAP (16 bytes)
    db 0                            ; reserved
    dw STAGE2_SECTORS               ; number of sectors to read
    dw STAGE2_LOAD_OFF              ; transfer buffer offset
    dw STAGE2_LOAD_SEG              ; transfer buffer segment
    dq STAGE2_LBA                   ; starting LBA

; =====================================================================
;  Boot signature
; =====================================================================
times 510-($-$$) db 0
dw 0xAA55
