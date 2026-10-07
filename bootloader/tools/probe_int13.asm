; =====================================================================
;  probe.asm - minimal INT 13h AH=42h probe (512-byte boot sector)
;  Tests a single extended read and prints CF / AH / the byte read.
;  Built by tools/probe_build.js; not part of the normal boot path.
; =====================================================================
[BITS 16]
[ORG 0x7C00]

%define TEST_LBA   33
%define TEST_SECS  1
%define TEST_SEG   0x1000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    mov si, msg_start
    call print_string

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    ; Record the outcome in memory before any helper clobbers the registers.
    setc [cf_flag]
    mov  [ah_code], ah
    mov  [si_code], si

    mov si, msg_cf
    call print_string
    mov al, [cf_flag]
    call print_hex8

    mov si, msg_ah
    call print_string
    mov al, [ah_code]
    call print_hex8

    mov si, msg_si
    call print_string
    mov ax, [si_code]
    call print_hex16

    mov si, msg_lba
    call print_string
    mov ax, [dap + 8]
    call print_hex16

    mov si, msg_seg
    call print_string
    mov ax, [dap + 6]
    call print_hex16

    mov si, msg_crlf
    call print_string

    ; If the read succeeded, report the first byte captured at TEST_SEG:0.
    mov si, msg_byte
    call print_string
    push ds
    mov  ax, TEST_SEG
    mov  ds, ax
    xor  si, si
    mov  al, [si]
    pop  ds
    call print_hex8
    mov si, msg_crlf
    call print_string

.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------------
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

print_hex8:
    push ax
    shr al, 4
    and al, 0x0F
    call print_nibble
    pop ax
print_nibble:
    push ax
    and al, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10 - '0'
.digit:
    add al, '0'
    mov ah, 0x0E
    int 0x10
    pop ax
    ret

print_hex16:
    push ax
    mov al, ah
    call print_hex8
    pop ax
    call print_hex8
    ret

; ---------------------------------------------------------------------
boot_drive:  db 0
cf_flag:     db 0
ah_code:     db 0
si_code:     dw 0
msg_start:   db '[PROBE] INT 13h AH=42h read test', 0x0D, 0x0A, 0
msg_cf:      db 'CF=', 0
msg_ah:      db ' AH=0x', 0
msg_si:      db ' SI=0x', 0
msg_lba:     db ' DAP.lba=0x', 0
msg_seg:     db ' DAP.seg=0x', 0
msg_byte:    db ' first_byte=0x', 0
msg_crlf:    db 0x0D, 0x0A, 0

align 4
dap:
    db 0x10
    db 0
    dw TEST_SECS
    dw 0x0000
    dw TEST_SEG
    dq TEST_LBA

times 510-($-$$) db 0
dw 0xAA55
