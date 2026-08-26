; 16-bit boot sector for QEMU (SeaBIOS) — loads the DCN freestanding kernel
; (built as a raw binary at linear 0x10000) and switches to 32-bit protected
; mode, then far-jumps to the kernel entry. 512 bytes, ends with 0x55AA.
BITS 16
org 0x7C00

start:
    ; Normalize CS to 0 regardless of how the BIOS loaded us.
    jmp 0x0000:.setcs
.setcs:
    ; NOTE: DO NOT cli here. The BIOS int 0x13 disk read needs interrupts
    ; enabled (it waits on the ATA/IDE IRQ); calling it with IF=0 hangs
    ; forever. We leave interrupts on through the disk read and only cli
    ; right before entering protected mode (no IDT is set up there yet).
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00           ; (CPU delays IRQs one instr after mov ss)

    ; Enable A20 (fast A20 through port 0x92).
    in al, 0x92
    or al, 0x02
    out 0x92, al

    ; DEBUG breadcrumb: 'A' = A20 enabled, about to call int 13h.
    mov al, 'A'
    call ser16_putc

    ; Extended disk read: 127 sectors (LBA 1..127) -> linear 0x10000.
    mov si, dap
    mov ah, 0x42
    mov dl, 0x80            ; first hard disk (QEMU -drive / -boot c)
    int 0x13
    jc hang

    ; DEBUG breadcrumb: 'K' = disk read OK, about to enter protected mode.
    mov al, 'K'
    call ser16_putc

    ; Now that the disk read is done, disable interrupts before PM (no IDT yet).
    cli
    ; Load a flat 32-bit GDT and enter protected mode.
    lgdt [gdt_ptr]
    mov eax, cr0
    or eax, 0x01
    mov cr0, eax
    ; In 16-bit mode nasm truncates a 32-bit far-jump offset to 16 bits
    ; ("word data exceeds bounds"), so a plain `jmp 0x08:0x100000` assembled
    ; as `jmp 0x8:0x0` and the CPU landed at physical 0 -> triple fault. Hand
    ; -encode the 0x66 operand-size prefix + 0xEA (jmp ptr16:32) with a full
    ; 32-bit EIP so we reach the kernel entry at linear 0x100000.
    db 0x66
    db 0xEA
    dd 0x00010000           ; 32-bit EIP (kernel entry @ linear 0x10000)
    dw 0x0008               ; code selector (flat 4 GiB, ring0)

hang:
    ; DEBUG breadcrumb: 'E' = int 13h failed. Print the BIOS error code (AH)
    ; as two hex digits so we can diagnose (01=invalid, 02=no addr mark,
    ; 0C=media changed, etc.).
    mov al, 'E'
    call ser16_putc
    mov al, ah
    call ser16_hex
    hlt
    jmp hang

; ---- 16-bit COM1 polled output (no DLAB required: we force 8N1) ----
ser16_putc:                 ; al = char
    push ax
    mov dx, 0x3FB
    mov al, 0x03            ; LCR: DLAB=0, 8 data bits, 1 stop, no parity
    out dx, al
    mov dx, 0x3FD
.w: in al, dx
    test al, 0x20           ; THR empty?
    jz .w
    pop ax                  ; restore char
    mov dx, 0x3F8
    out dx, al
    ret

ser16_hex:                  ; al = byte -> print two hex digits
    push ax
    shr al, 4
    call ser16_nib
    pop ax
    and al, 0x0F
    call ser16_nib
    ret
ser16_nib:                  ; al = nibble -> print one hex digit
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, ('A' - '9' - 1)
.out:
    push ax
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FD
.w2: in al, dx
    test al, 0x20
    jz .w2
    pop ax
    mov dx, 0x3F8
    out dx, al
    ret

; ---- DAP for int 13h, ah=42h ----
; NOTE: the 4-byte buffer field is a 16:16 SEGMENT:OFFSET (offset in low word,
; segment in high word), NOT a flat 32-bit address. We load to linear 0x10000
; via seg=0x1000, off=0x0000 (a plain low-memory address SeaBIOS handles
; trivially). Loading above 1 MB via seg:off is fragile and hung on this BIOS.
dap:
    db 0x10                 ; size of DAP
    db 0x00                 ; reserved
    dw 127                  ; sector count (BIOS-safe max per ext read; 127*512=65KB
                             ;   covers the kernel, which is far smaller)
    dw 0x0000               ; buffer offset   (low word)
    dw 0x1000               ; buffer segment  (seg 0x1000 : off 0 = linear 0x10000)
    dq 1                    ; starting LBA (sector 1, right after this boot sector)

; ---- GDT (flat 4 GiB) ----
gdt:
    dq 0x0000000000000000   ; null
    dq 0x00CF9A000000FFFF   ; code: base 0, limit 4G, ring0, 32-bit
    dq 0x00CF92000000FFFF   ; data: base 0, limit 4G, ring0
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt

times 510-($-$$) db 0
dw 0xAA55
