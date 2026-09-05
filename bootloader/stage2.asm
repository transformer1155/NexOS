; =====================================================================
;  stage2.asm  -  Stage 2 Bootloader
; ---------------------------------------------------------------------
;  Loaded by Stage 1 at 0x8000 in 16-bit real mode.
;  Responsibilities:
;    1. Load the C++ kernel from disk (LBA 33) into memory at 0x10000.
;    2. Enable the A20 line (fast A20 via system port 0x92).
;    3. Load the GDT and switch to 32-bit protected mode.
;    4. Far-jump into the kernel entry at 0x10000.
;
;  This binary is padded to exactly 16 KiB (32 sectors) so that the
;  kernel always begins at a fixed, predictable LBA (33).
;
;  Build:  nasm -f bin stage2.asm -o stage2.bin   (exactly 16384 bytes)
; =====================================================================

[BITS 16]
[ORG 0x8000]

; ----- Layout constants -----
KERNEL_LOAD_SEG    equ 0x1000      ; load kernel at linear 0x1000:0x0000 = 0x10000
KERNEL_LOAD_OFF    equ 0x0000
KERNEL_LBA         equ 33          ; 1 (boot) + 32 (stage2) = 33
KERNEL_SECTORS     equ 512         ; 256 KiB max (kernel.bin ~219 KB; keeps load below 0xA0000 VGA)
KERNEL_CHUNK       equ 64          ; sectors per INT 13h call (BIOS-safe limit)
KERNEL_ENTRY       equ 0x10000     ; kernel entry (linked at this address)

start:
    ; boot_drive from boot.bin or boot_cd (0xF8 = SeaBIOS CD/HDD signature).
    ; Map 0xF8 -> 0x80 (HDD) so INT 13h AH=42h works.
    mov [boot_drive], dl
    cmp dl, 0xF8
    jne .drive_ok
    mov dl, 0x80
    mov [boot_drive], dl
.drive_ok:

    mov si, msg_hello
    call print_string

    ; ----- 1. Load the kernel in chunks of KERNEL_CHUNK sectors (loop) -----
    ; kernel.bin can be up to KERNEL_SECTORS*512 bytes (default 1 MiB).
    ; Each chunk advances the segment by 32KB (chunk*512/16 = 0x800).
    mov si, dap_kernel
    mov byte [dap_kernel + 2], KERNEL_CHUNK     ; sectors per call
    mov ax, KERNEL_SECTORS                       ; remaining sectors
    xor esi, esi                                 ; LBA offset (relative to KERNEL_LBA)
    mov word [dap_kernel + 6], KERNEL_LOAD_SEG   ; segment (0x1000 -> linear 0x10000)
.load_loop:
    or ax, ax
    jz .load_done
    mov cx, ax
    cmp cx, KERNEL_CHUNK
    jbe .chunk_ok
    mov cx, KERNEL_CHUNK
.chunk_ok:
    mov word [dap_kernel + 2], cx                ; sectors this call
    mov word [dap_kernel + 4], 0x0000            ; offset (segment-based addressing)
    mov eax, esi
    add eax, KERNEL_LBA
    mov dword [dap_kernel + 8], eax              ; LBA
    mov si, dap_kernel
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    sub ax, cx                                   ; remaining -= chunk
    movzx ecx, cx
    add esi, ecx                                 ; lba_off += chunk
    add word [dap_kernel + 6], 0x0800            ; segment += 32KB
    jmp .load_loop
.load_done:
    mov word [dap_kernel + 2], KERNEL_CHUNK      ; restore

    mov si, msg_kernel
    call print_string

    ; ----- 1b. Set VBE graphics mode (dynamic mode enumeration) -----
    ; Result stored at 0x5000 for kernel. Format:
    ;   0x5000: uint32 framebuffer_phys
    ;   0x5004: uint16 width
    ;   0x5006: uint16 height
    ;   0x5008: uint8  bpp
    ;   0x5009: uint16 pitch (bytes per scanline)
    ;   0x500B: uint16 mode_number
    ;   0x500D: uint8  vbe_ok (1=ok, 0=failed)
    mov byte [0x500D], 0          ; assume failure

    ; Serial debug: 'V' = starting VBE setup
    mov dx, 0x3F8
    mov al, 'V'
    out dx, al

    ; ---- Step 1: Get VBE controller info at 0x5400 ----
    ; ES:DI = 0x0000:0x5400
    xor ax, ax
    mov es, ax
    mov di, 0x5400
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .vbe_no_bios

    ; Check VBE signature "VESA" at 0x5400
    mov eax, [0x5400]
    cmp eax, 0x41534556          ; "VESA" little-endian
    jne .vbe_no_bios

    ; Serial: 'B' = VBE BIOS found
    mov dx, 0x3F8
    mov al, 'B'
    out dx, al

    ; ---- Step 2: Get VideoModePtr from VbeInfoBlock offset 14 ----
    ; Format: 2-byte offset, 2-byte segment
    mov si, [0x5400 + 14]        ; mode list offset
    mov ax, [0x5400 + 16]        ; mode list segment
    mov [vbe_list_off], si
    mov [vbe_list_seg], ax

    ; Serial: 'E' = enumerating
    mov dx, 0x3F8
    mov al, 'E'
    out dx, al

    ; ---- Step 3: Iterate through mode list ----
    ; Best match tracking: prefer higher resolutions
    ; Score: 7=1920x1080, 6=1600x900, 5=1366x768, 4=1280x1024/1280x720,
    ;        3=1024x768, 2=800x600, 1=640x480
    mov word [vbe_best_mode], 0xFFFF   ; no match yet
    mov word [vbe_best_score], 0       ; 0=no match

.vbe_enum_loop:
    ; Load mode list pointer into ES:SI
    mov ax, [vbe_list_seg]
    mov es, ax
    mov si, [vbe_list_off]

    ; Read mode number
    mov cx, [es:si]
    cmp cx, 0xFFFF               ; end of list?
    je .vbe_enum_done

    ; Advance list pointer
    add word [vbe_list_off], 2

    ; Skip if mode >= 0x200 (not a valid VBE mode)
    cmp cx, 0x200
    jae .vbe_enum_loop

    ; ---- Get mode info for this mode ----
    ; ES:DI = 0x0000:0x5300
    push cx                      ; save mode number
    xor ax, ax
    mov es, ax
    mov di, 0x5300
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .vbe_enum_skip

    ; Check ModeAttributes: bit 7 (LFB) must be set
    test word [0x5300], 0x0080
    jz .vbe_enum_skip

    ; Check BitsPerPixel: accept 32, 24, or 16 (broader hardware support)
    cmp byte [0x5300 + 25], 32
    je .vbe_bpp_ok
    cmp byte [0x5300 + 25], 24
    je .vbe_bpp_ok
    cmp byte [0x5300 + 25], 16
    je .vbe_bpp_ok
    jmp .vbe_enum_skip
.vbe_bpp_ok:

    ; Check MemoryModel == 6 (Direct Color)
    cmp byte [0x5300 + 27], 6
    jne .vbe_enum_skip

    ; Check PhysBasePtr != 0
    mov eax, [0x5300 + 40]
    test eax, eax
    jz .vbe_enum_skip

    ; Check resolution and score it
    mov ax, [0x5300 + 18]        ; XResolution
    cmp ax, 1920
    je .vbe_check_1920_y
    cmp ax, 1600
    je .vbe_check_1600_y
    cmp ax, 1366
    je .vbe_check_1366_y
    cmp ax, 1280
    je .vbe_check_1280_y
    cmp ax, 1024
    je .vbe_check_1024_y
    cmp ax, 800
    je .vbe_check_800_y
    cmp ax, 640
    je .vbe_check_640_y
    jmp .vbe_enum_skip           ; not a supported resolution

.vbe_check_1920_y:
    cmp word [0x5300 + 20], 1080
    jne .vbe_enum_skip
    cmp word [vbe_best_score], 7
    jae .vbe_enum_skip
    mov word [vbe_best_score], 7
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_check_1600_y:
    cmp word [0x5300 + 20], 900
    jne .vbe_enum_skip
    cmp word [vbe_best_score], 6
    jae .vbe_enum_skip
    mov word [vbe_best_score], 6
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_check_1366_y:
    cmp word [0x5300 + 20], 768
    jne .vbe_enum_skip
    cmp word [vbe_best_score], 5
    jae .vbe_enum_skip
    mov word [vbe_best_score], 5
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_check_1280_y:
    cmp word [0x5300 + 20], 1024
    je .vbe_check_1280_1024
    cmp word [0x5300 + 20], 720
    je .vbe_check_1280_720
    jmp .vbe_enum_skip

.vbe_check_1280_1024:
    cmp word [vbe_best_score], 4
    jae .vbe_enum_skip
    mov word [vbe_best_score], 4
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_check_1280_720:
    cmp word [vbe_best_score], 4
    jae .vbe_enum_skip
    mov word [vbe_best_score], 4
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_check_1024_y:
    cmp word [0x5300 + 20], 768
    jne .vbe_enum_skip
    cmp word [vbe_best_score], 3
    jae .vbe_enum_skip
    mov word [vbe_best_score], 3
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_check_800_y:
    cmp word [0x5300 + 20], 600
    jne .vbe_enum_skip
    cmp word [vbe_best_score], 2
    jae .vbe_enum_skip
    mov word [vbe_best_score], 2
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_check_640_y:
    cmp word [0x5300 + 20], 480
    jne .vbe_enum_skip
    cmp word [vbe_best_score], 1
    jae .vbe_enum_skip
    mov word [vbe_best_score], 1
    pop cx
    push cx
    mov [vbe_best_mode], cx
    jmp .vbe_enum_loop

.vbe_enum_skip:
    pop cx                       ; discard mode number
    jmp .vbe_enum_loop

.vbe_enum_done:
    ; Check if we found a suitable mode
    cmp word [vbe_best_mode], 0xFFFF
    je .vbe_no_match

    ; Serial: 'F' = found a mode
    mov dx, 0x3F8
    mov al, 'F'
    out dx, al

    ; ---- Query mode info for best mode ----
    ; Mode will be SET via INT 10h below (real hardware compatible)
    mov cx, [vbe_best_mode]
    call vbe_query_only
    jnc .vbe_settled
    jmp .vbe_skip

.vbe_no_match:
    ; Serial: 'N' = no matching mode
    mov dx, 0x3F8
    mov al, 'N'
    out dx, al

    ; ---- Fallback: try hardcoded VESA/Bochs mode numbers ----
    ; Try higher resolutions first, then standard modes
    mov cx, 0x14A                  ; Bochs: 1920x1080x32
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x149                  ; Bochs: 1600x900x32
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x148                  ; Bochs: 1366x768x32
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x147                  ; Bochs: 1280x720x32
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x144                  ; Bochs: 1024x768x32
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x143                  ; Bochs: 800x600x32
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x142                  ; Bochs: 640x480x32
    call vbe_query_only
    jnc .vbe_settled
    ; Try standard VESA modes (24bpp)
    mov cx, 0x11B                  ; VESA: 1280x1024x32
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x118                  ; VESA: 1024x768x24
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x115                  ; VESA: 800x600x24
    call vbe_query_only
    jnc .vbe_settled
    mov cx, 0x112                  ; VESA: 640x480x24
    call vbe_query_only
    jnc .vbe_settled
    jmp .vbe_skip

.vbe_settled:
    ; vbe_query_only already stored framebuffer info at 0x5000
    ; CX = mode number (preserved by pusha/popa in vbe_query_only)
    mov [0x500B], cx              ; mode number
    mov byte [0x500D], 1          ; success - VBE info available
    mov byte [0x500E], 0          ; vbe_mode_set = 0 (default: not set yet)

    ; ---- SET VBE mode via INT 10h (real hardware compatible) ----
    ; AX=0x4F02, BX=mode | 0x4000 (bit 14 = use LFB framebuffer)
    ; Some BIOSes require ES:DI to point at the mode info block we just queried.
    push cx
    xor ax, ax
    mov es, ax
    mov di, 0x5300                 ; mode info block from vbe_query_only
    mov bx, cx
    or  bx, 0x4000                ; set LFB bit (use linear framebuffer)
    mov ax, 0x4F02
    int 0x10
    pop cx
    cmp ax, 0x004F
    jne .vbe_set_failed
    ; Debug: write AX to serial
    push ax
    mov dx, 0x3F8
    mov al, 'Q'
    out dx, al
    pop ax
    shr ax, 8
    and ax, 0xFF
    add al, '0'
    out dx, al
    jmp .vbe_skip

.vbe_set_failed:
    ; INT 10h 0x4F02 failed (QEMU SeaBIOS may reject repeated mode set).
    ; Try direct Bochs VBE ports (0x1CE/0x1CF) which QEMU always honours.
    mov dx, 0x1CE                 ; VBE_DISPI_IOPORT_INDEX
    mov ax, 0x0004                ; VBE_DISPI_INDEX_XRES
    out dx, ax
    mov dx, 0x1CF
    mov ax, [0x5300 + 18]         ; width from VBE info block
    out dx, ax
    mov dx, 0x1CE
    mov ax, 0x0005                ; VBE_DISPI_INDEX_YRES
    out dx, ax
    mov dx, 0x1CF
    mov ax, [0x5300 + 20]         ; height
    out dx, ax
    mov dx, 0x1CE
    mov ax, 0x0001                ; VBE_DISPI_INDEX_ENABLE
    out dx, ax
    mov dx, 0x1CF
    mov ax, 0x0041                ; ENABLE | LFB_ENABLED (0x40 + 0x01)
    out dx, ax

    mov byte [0x500E], 1          ; vbe_mode_set = 1 (via BGA ports)

    mov dx, 0x3F8
    mov al, 'B'                   ; 'B' = BGA fallback succeeded
    out dx, al

    jmp .vbe_skip

.vbe_no_bios:
    ; VBE 0x4F00 failed in 32-bit PM (SeaBIOS may not handle PM INT 10h).
    ; Try direct Bochs VBE ports (always available in QEMU) to set mode
    ; and write the resulting framebuffer info to 0x5000 for the kernel.
    mov dx, 0x1CE                 ; VBE_DISPI_IOPORT_INDEX
    mov ax, 0x0001                ; VBE_DISPI_INDEX_ENABLE
    out dx, ax
    mov dx, 0x1CF
    mov ax, 0x0040                ; ENABLE without LFB (just check presence)
    out dx, ax
    mov dx, 0x1CE
    mov ax, 0x0000                ; VBE_DISPI_INDEX_ID
    out dx, ax
    mov dx, 0x1CF
    in  ax, dx                    ; read BGA ID (should be 0xB0C0..0xB0C6)
    cmp ax, 0xB0C0
    jb .vbe_skip                  ; not a BGA adapter, give up

    ; Set a default 1024x768x32 mode via BGA ports (works in 32-bit PM).
    mov dx, 0x1CE
    mov ax, 0x0004                ; XRES
    out dx, ax
    mov dx, 0x1CF
    mov ax, 1024
    out dx, ax
    mov dx, 0x1CE
    mov ax, 0x0005                ; YRES
    out dx, ax
    mov dx, 0x1CF
    mov ax, 768
    out dx, ax
    mov dx, 0x1CE
    mov ax, 0x0001                ; ENABLE
    out dx, ax
    mov dx, 0x1CF
    mov ax, 0x0041                ; ENABLE | LFB_ENABLED
    out dx, ax

    ; Tell the kernel about this mode (Bochs Cirrus LFB is typically
    ; at 0xFE000000; 0xFD000000 is the legacy Bochs VBE LFB address).
    mov dword [0x5000], 0xFE000000   ; framebuffer_phys (32-bit, kernel can read it)
    mov word  [0x5004], 1024         ; width
    mov word  [0x5006], 768          ; height
    mov byte  [0x5008], 32           ; bpp
    mov word  [0x5009], 1024 * 4     ; pitch
    mov word  [0x500B], 0x144        ; mode number (1024x768x32)
    mov byte  [0x500D], 1            ; vbe_ok
    mov byte  [0x500E], 1            ; vbe_mode_set (BGA ports succeeded)

    mov dx, 0x3F8
    mov al, 'G'                   ; 'G' = BGA fallback mode set
    out dx, al

.vbe_skip:

    ; ----- 2. Enable A20 line (fast A20, port 0x92) -----
    in  al, 0x92
    or  al, 2                      ; set bit 1 (A20 enable); keep bit 0 (reset) clear
    out 0x92, al

    ; ----- 3. Enter protected mode -----
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or  eax, 1                     ; set CR0.PE (Protection Enable)
    mov cr0, eax

    ; Far jump flushes the prefetch queue and loads CS with the code selector.
    jmp CODE_SEG:init_pm

[BITS 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000               ; set up a 32-bit stack

    ; ----- 4. Jump to the kernel -----
    jmp KERNEL_ENTRY

; =====================================================================
;  16-bit helper functions (must be [BITS 16] — called from real mode)
; =====================================================================
[BITS 16]

; =====================================================================
;  16-bit error handler
; =====================================================================
disk_error:
    mov si, msg_error
    call print_string
.hang:
    cli
    hlt
    jmp .hang

; =====================================================================
;  print_string  -  NUL-terminated string at DS:SI via BIOS teletype
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
;  vbe_query_only  -  Query VBE mode info WITHOUT setting the mode
;  Input:  CX = VBE mode number
;  Output: CF clear = success (ModeInfoBlock at 0x5300, mode NOT changed)
;          CF set   = failure
; =====================================================================
vbe_query_only:
    pusha

    ; Get mode info -> ES:DI = 0x0000:0x5300
    push 0x0000
    pop es
    mov di, 0x5300
    mov ax, 0x4F01
    int 0x10
    cmp ax, 0x004F
    jne .vqo_fail1

    ; Check ModeAttributes bit 7 (LFB supported)
    mov ax, [0x5300]               ; ModeAttributes
    test ax, 0x0080
    jz .vqo_fail2

    ; Check BitsPerPixel: accept 32, 24, or 16 (broader hardware support)
    mov al, [0x5300 + 25]
    cmp al, 32
    je .vqo_bpp_ok
    cmp al, 24
    je .vqo_bpp_ok
    cmp al, 16
    je .vqo_bpp_ok
    jmp .vqo_fail3
.vqo_bpp_ok:

    ; Check MemoryModel == 6 (Direct Color / RGB)
    mov al, [0x5300 + 27]
    cmp al, 6
    jne .vqo_fail4

    ; Check PhysBasePtr != 0
    mov eax, [0x5300 + 40]
    test eax, eax
    jz .vqo_fail5

    ; All checks passed - store info at 0x5000
    mov eax, [0x5300 + 40]
    mov [0x5000], eax
    mov ax, [0x5300 + 18]
    mov [0x5004], ax
    mov ax, [0x5300 + 20]
    mov [0x5006], ax
    mov al, [0x5300 + 25]
    mov [0x5008], al
    mov ax, [0x5300 + 16]
    mov [0x5009], ax

    popa
    clc                            ; success
    ret

.vqo_fail1:
    mov dx, 0x3F8
    mov al, '1'
    out dx, al
    popa
    stc
    ret

.vqo_fail2:
    mov dx, 0x3F8
    mov al, '2'
    out dx, al
    popa
    stc
    ret

.vqo_fail3:
    mov dx, 0x3F8
    mov al, '3'
    out dx, al
    popa
    stc
    ret

.vqo_fail4:
    mov dx, 0x3F8
    mov al, '4'
    out dx, al
    popa
    stc
    ret

.vqo_fail5:
    mov dx, 0x3F8
    mov al, '5'
    out dx, al
    popa
    stc
    ret

; =====================================================================
;  Data
; =====================================================================
boot_drive:  db 0
msg_hello:   db '[Stage2] Two-stage bootloader -> C++ kernel', 0x0D, 0x0A, 0
msg_kernel:  db '[Stage2] Kernel loaded, switching to 32-bit PM...', 0x0D, 0x0A, 0
msg_error:   db '[Stage2] DISK READ ERROR!', 0x0D, 0x0A, 0
msg_vbe_ok:  db '[Stage2] VBE mode set via INT 10h (graphics mode active)', 0x0D, 0x0A, 0

; ----- VBE enumeration variables -----
align 2
vbe_list_off:    dw 0
vbe_list_seg:    dw 0
vbe_best_mode:   dw 0
vbe_best_score:  dw 0

; ----- Disk Address Packet (DAP) for kernel load -----
align 4
dap_kernel:
    db 0x10
    db 0
    dw KERNEL_CHUNK               ; sectors per call (128)
    dw KERNEL_LOAD_OFF
    dw KERNEL_LOAD_SEG
    dq KERNEL_LBA

; =====================================================================
;  Global Descriptor Table (GDT)
;  Two flat segments: base=0, limit=4GiB, 32-bit, 4 KiB granularity.
; =====================================================================
gdt_start:
gdt_null:                          ; null descriptor (required)
    dq 0x0000000000000000

gdt_code:                          ; 32-bit code segment
    dw 0xFFFF                      ; limit 15:0
    dw 0x0000                      ; base 15:0
    db 0x00                        ; base 23:16
    db 10011010b                   ; access: P=1 DPL=0 S=1 type=code(1010)
    db 11001111b                   ; flags: G=1 D=1 + limit 19:16=0xF
    db 0x00                        ; base 31:24

gdt_data:                          ; 32-bit data segment
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                   ; access: P=1 DPL=0 S=1 type=data(0010)
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1     ; limit (size - 1)
    dd gdt_start                    ; base address

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

; =====================================================================
;  Pad Stage 2 to exactly 16 KiB (32 sectors)
;  so the kernel LBA (33) is deterministic.
; =====================================================================
times 16384-($-$$) db 0
