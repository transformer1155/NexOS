; =====================================================================
;  boot_cd.asm  -  El Torito no-emulation CD/DVD boot sector
; ---------------------------------------------------------------------
;  Loaded by BIOS at 0x7C00 in 16-bit real mode (no-emulation mode).
;  Uses the boot info table (-boot-info-table) to find the boot image
;  LBA on the CD, then loads the kernel from the CD.
;
;  Boot Info Table (patched by xorriso, offset 8-63, 56 bytes):
;    offset  8:  PVD LBA (4B), offset 12: Boot image LBA (4B),
;    offset 16:  Boot image len (4B), offset 20: Checksum (4B),
;    offset 24-63: reserved (40 bytes, ZEROED by xorriso)
;
;  IMPORTANT: main code MUST start at offset 64 (after the full 56-byte
;  boot info table).  If it starts earlier, xorriso will zero it out!
;
;  Build:  nasm -f bin boot_cd.asm -o boot_cd.bin  (exactly 512 bytes)
; =====================================================================

[BITS 16]
[ORG 0x7C00]

KERNEL_CD_START   equ 1              ; kernel at CD sector 1 (relative to boot image)
KERNEL_ENTRY      equ 0x10000        ; kernel linked address
CHUNK_SECTORS     equ 16             ; 16 CD sectors = 32KB per read
KERNEL_CHUNKS     equ 16            ; 16 chunks = 256 CD sectors = 512KB max.
                                    ; Load address stays 0x10000-0x90000 (segment 0x1000..0x9000),
                                    ; never touching VGA text buffer at 0xB8000 / graphics 0xA0000.

; =====================================================================
;  Boot sector entry + El Torito boot info table (offset 0-55)
; =====================================================================
start:
    jmp short main                   ; offset 0-1
    nop                              ; offset 2
    times 5 db 0                     ; offset 3-7
    ; --- Boot info table (48 bytes, offset 8-55) ---
    bi_pvd:      dd 0                ; offset  8: PVD LBA
    bi_boot_lba: dd 0                ; offset 12: Boot image LBA
    bi_boot_len: dd 0                ; offset 16: Boot image length
    bi_checksum: dd 0                ; offset 20: Checksum
    times 40 db 0                    ; offset 24-63: reserved (ZEROED by xorriso!)

; =====================================================================
;  Main code starts at offset 64
; =====================================================================
main:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [boot_drive], dl
    ; SeaBIOS passes 0xF8 (boot signature) for CD/HDD boot - map to 0x80
    cmp dl, 0xF8
    jne .bcd_drive_ok
    mov dl, 0x80
    mov [boot_drive], dl
.bcd_drive_ok:

    mov si, msg_loading
    call puts

    ; ===== Calculate kernel LBA =====
    mov eax, [bi_boot_lba]
    add eax, KERNEL_CD_START
    mov [cd_kernel_lba], eax

    ; ===== Load kernel in chunks (loop) =====
    ; NOTE: INT 13h clobbers AX, so we store LBA in memory and reload each iter.
    mov cx, KERNEL_CHUNKS            ; chunk counter
    mov word [load_seg], 0x1000      ; first chunk -> 0x1000:0000
.load_loop:
    push cx

    ; Setup DAP (reload LBA from memory - INT 13h clobbers EAX)
    mov eax, [cd_kernel_lba]
    mov dword [dap + 8], eax         ; LBA low
    mov dword [dap + 12], 0          ; LBA high
    mov word [dap + 2], CHUNK_SECTORS
    mov word [dap + 4], 0x0000       ; offset
    mov ax, [load_seg]
    mov word [dap + 6], ax           ; segment
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; Advance LBA and segment (reload LBA - AX was clobbered)
    mov eax, [cd_kernel_lba]
    add eax, CHUNK_SECTORS
    mov [cd_kernel_lba], eax
    add word [load_seg], 0x0800      ; +32KB

    pop cx
    loop .load_loop

    mov si, msg_loaded
    call puts

    ; ===== Enable A20 =====
    in  al, 0x92
    or  al, 2
    out 0x92, al

    ; ===== Enter protected mode =====
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp CODE_SEG:init_pm

; =====================================================================
[BITS 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    jmp KERNEL_ENTRY

; =====================================================================
[BITS 16]
disk_error:
    mov [err_ah], ah
    mov si, msg_err
    call puts
    mov al, [err_ah]
    call print_hex
.hang:
    cli
    hlt
    jmp .hang

; =====================================================================
;  puts - print NUL-terminated string at DS:SI
; =====================================================================
puts:
    pusha
    mov ah, 0x0E
.l:
    lodsb
    test al, al
    jz .d
    int 0x10
    jmp .l
.d:
    popa
    ret

; =====================================================================
;  print_hex - print AL as 2 hex digits
; =====================================================================
print_hex:
    pusha
    mov bl, al
    shr al, 4
    call .nib
    mov al, bl
    and al, 0x0F
    call .nib
    popa
    ret
.nib:
    add al, '0'
    cmp al, '9'
    jle .p
    add al, 7
.p:
    mov ah, 0x0E
    int 0x10
    ret

; =====================================================================
;  Data
; =====================================================================
boot_drive:    db 0
err_ah:        db 0
cd_kernel_lba: dd 0
load_seg:      dw 0

msg_loading:   db '[CDBoot] Loading...', 0x0D, 0x0A, 0
msg_loaded:    db '[CDBoot] OK, entering PM', 0x0D, 0x0A, 0
msg_err:       db 0x0D, 0x0A, 'DISK ERR AH=', 0

; ----- Disk Address Packet -----
align 4
dap:
    db 0x10
    db 0
    dw 0
    dw 0
    dw 0
    dq 0

; =====================================================================
;  GDT
; =====================================================================
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; =====================================================================
times 510-($-$$) db 0
dw 0xAA55
