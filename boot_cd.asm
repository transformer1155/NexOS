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
;  Layout of this file:
;    offset 0   - 0x1FE : first 512 bytes = real boot sector.  SeaBIOS peeks
;                            at offset 0x1FE for the 0x55AA signature on
;                            no-emulation media, so we keep a valid 512-byte
;                            boot record here (0xAA55 at 0x1FE).  Only the
;                            kernel load loop, the SFS-stream trigger, the
;                            A20/PM handoff and the minimal variables live
;                            here.
;    offset 0x200-0x7EF : SFS stream loop + copy trampoline + helpers
;                            (16-bit real mode + 32-bit PM snippet) + data
;                            (SFS vars, messages, DAP, GDT).  Reached via
;                            near calls from main.
;    offset 0x7F0/0x7F4 : patched by tools/make_cd_boot.py (SFS/kernel CD
;                            sector counts).
;
;  Build:  nasm -f bin boot_cd.asm -o boot_cd.bin  (exactly 2048 bytes)
; =====================================================================

[BITS 16]
[ORG 0x7C00]

KERNEL_CD_START   equ 1              ; kernel at CD sector 1 (relative to boot image)
KERNEL_ENTRY      equ 0x10000        ; kernel linked address
CHUNK_SECTORS     equ 16             ; 16 CD sectors = 32KB per read
KERNEL_CHUNKS     equ 16            ; 16 chunks = 256 CD sectors = 512KB max.
                                    ; Load address stays 0x10000-0x90000 (segment 0x1000..0x9000),
                                    ; never touching VGA text buffer at 0xB8000 / graphics 0xA0000.
; --- CD-boot texture-free SFS handoff ---
; The bootloader streams the (texture-free) SFS image off the CD into high
; RAM at SFS_RAM_TARGET so the 32-bit kernel can mount it without an ATA
; disk (none exists on a CD/ISO boot).  A flag at 0x0900 tells the kernel
; where the image landed.
SFS_RAM_MAGIC     equ 0xC0DE5A5F    ; "CD SFS" handoff marker
SFS_RAM_TARGET    equ 0x01400000    ; 20 MiB: free window (heap ends at 19 MiB, identity-mapped to 32 MiB)

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

    ; ===== Stream the (texture-free) SFS image into high RAM (tail routine) =====
    call sfs_stream
    mov al, 'M'
    mov dx, 0x3F8
    out dx, al

    ; ===== Write the CD-boot SFS handoff flag at 0x0900 =====
    mov dword [0x0900], SFS_RAM_MAGIC
    mov dword [0x0904], SFS_RAM_TARGET
    mov eax, [sfs_cd_sects]
    shl eax, 11                    ; * 2048 -> byte size
    mov dword [0x0908], eax

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
    mov al, 'X'
    mov dx, 0x3F8
    out dx, al
    jmp CODE_SEG:init_pm

; =====================================================================
[BITS 32]
init_pm:
    mov al, 'Y'
    mov dx, 0x3F8
    out dx, al
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov al, 'Z'
    mov dx, 0x3F8
    out dx, al
    jmp KERNEL_ENTRY

; =====================================================================
;  Minimal data that main touches directly (first 512 bytes)
; =====================================================================
boot_drive:    db 0
err_ah:        db 0
cd_kernel_lba: dd 0
load_seg:      dw 0

msg_loading:   db '[CDBoot] Loading...', 0x0D, 0x0A, 0
msg_loaded:    db '[CDBoot] OK, entering PM', 0x0D, 0x0A, 0

; Keep the first 512 bytes a valid boot record (0xAA55 at 0x1FE).  SeaBIOS
; on no-emulation media still inspects the signature at offset 0x1FE.
times (0x1FE - ($ - $$)) db 0
dw 0xAA55

; =====================================================================
;  Tail (512-2048): SFS stream loop, copy trampoline, helpers + data.
;  Loaded by -boot-load-size 4 and reachable from main via near calls.
; =====================================================================

; ---------------------------------------------------------------------
;  sfs_stream - stream the (texture-free) SFS image off the CD into high
;  RAM at SFS_RAM_TARGET.  Real mode cannot address >1 MiB, so each
;  CHUNK_SECTORS (16) CD-sector chunk is read via INT 13h into a
;  0x9000:0000 bounce buffer (below the VGA hole at 0xA0000) and then
;  copied to high RAM in a brief protected-mode session (sfs_copy_hi).
;  On CD boot no ATA disk exists, so this is the only way the kernel can
;  reach SFS.
; ---------------------------------------------------------------------
[BITS 16]
sfs_stream:
    ; SFS CD LBA = boot_image_LBA + 1 (boot sector) + kernel_cd_sects
    mov eax, [bi_boot_lba]
    add eax, 1
    add eax, [kernel_cd_sects]
    mov [sfs_cd_lba], eax
    mov ecx, [sfs_cd_sects]        ; CD sectors remaining
    xor ebx, ebx                   ; CD sectors copied so far
.sfs_loop:
    test ecx, ecx
    jz .sfs_done
    mov eax, CHUNK_SECTORS
    cmp ecx, eax
    jge .sfs_chunk_ok
    mov eax, ecx
.sfs_chunk_ok:
    mov [sfs_chunk], eax           ; sectors to read this pass
    mov eax, [sfs_cd_lba]
    mov dword [dap + 8], eax       ; DAP LBA low
    mov dword [dap + 12], 0        ; DAP LBA high
    mov ax, [sfs_chunk]
    mov word [dap + 2], ax         ; DAP sector count
    mov word [dap + 4], 0x0000     ; DAP offset
    mov word [dap + 6], 0x9000     ; DAP segment -> 0x090000 (bounce)
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    mov al, 'R'
    mov dx, 0x3F8
    out dx, al
    mov [sfs_off], ebx
    ; Preserve ecx across sfs_copy_hi: the brief PM session uses rep movsd,
    ; which leaves ecx=0.  Without this, the loop's `sub ecx,eax` wraps to a
    ; huge value and the stream never terminates (reads past end of CD).
    ; Pushing here is safe because sfs_copy_hi saves [rm_sp] AFTER this push,
    ; so .rm16 restores the exact stack and this pop pairs up correctly.
    push ecx
    call sfs_copy_hi
    pop ecx
    mov al, 'Q'
    mov dx, 0x3F8
    out dx, al
    mov eax, [sfs_chunk]
    add [sfs_cd_lba], eax
    add ebx, eax
    sub ecx, eax
    jmp .sfs_loop
.sfs_done:
    ret

; ---------------------------------------------------------------------
;  sfs_copy_hi - copy one SFS chunk from the 0x090000 bounce buffer to
;  SFS_RAM_TARGET (high RAM) via a brief protected-mode session, then
;  return to real mode.  Real mode cannot address >1 MiB, so this tramp
;  is required to place the SFS image where the 32-bit kernel can find it.
; ---------------------------------------------------------------------
[BITS 16]
sfs_copy_hi:
    cli
    mov al, 'm'
    mov dx, 0x3F8
    out dx, al
    ; Save the real-mode SP so .rm16 can restore the exact stack (including
    ; this call's return address) after clearing PE.  Restoring SP to a fixed
    ; value like 0x7C00 would clobber the return address pushed by
    ; `call sfs_copy_hi` and make the final `ret` jump to garbage.
    mov [rm_sp], sp
    lgdt [gdt_desc]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    ; CRITICAL: right after `cr0 |= 1`, CS still caches the real-mode
    ; descriptor (CS.D = 0).  The far jump must therefore be encoded as a
    ; 16:16 (2-byte offset + 2-byte selector) far jump, EXACTLY like
    ; stage2.asm's `jmp CODE_SEG:init_pm`.  A 16:32 (32-bit offset) encoding
    ; would be mis-decoded as 16:16 here and load a garbage EIP -> triple
    ; fault (this was the CD-boot white-screen root cause).  The kernel /
    ; .pm_copy both live below 64 KiB, so the 16-bit offset resolves
    ; correctly under the flat (base=0) GDT.
    jmp CODE_SEG:.pm_copy

[BITS 32]
.pm_copy:
    mov esp, 0x90000
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov ecx, 8
    mov esi, 0x00090000
    mov edi, 0x01400000
    cld
    rep movsd
    jmp RM16_SEG:.rm16

[BITS 16]
.rm16:
    mov eax, cr0
    and eax, ~1
    mov cr0, eax
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, [rm_sp]
    mov al, 'b'
    mov dx, 0x3F8
    out dx, al
    ret

; ---------------------------------------------------------------------
;  puts - print NUL-terminated string at DS:SI (BIOS INT 10h)
; ---------------------------------------------------------------------
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

; ---------------------------------------------------------------------
;  print_hex - print AL as 2 hex digits (BIOS INT 10h)
; ---------------------------------------------------------------------
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

; ---------------------------------------------------------------------
;  disk_error - halt with AH code
; ---------------------------------------------------------------------
disk_error:
    mov al, 'E'
    mov dx, 0x3F8
    out dx, al
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
;  Tail data (SFS vars, messages, DAP, GDT)
; =====================================================================
sfs_cd_lba:    dd 0              ; current SFS CD read LBA
sfs_chunk:     dd 0              ; sectors read in current pass
sfs_off:       dd 0              ; CD sectors already copied to RAM
rm_sp:         dw 0              ; saved real-mode SP across a PM/RM round-trip

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

; ----- GDT -----
gdt_start:
    dq 0
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
; 16-bit code segment used ONLY to return to real mode.  Jumped while still
; in PM (so the GDT sets D=0/base=0); then PE is cleared.  Returning via
; selector 0 (jmp 0:.rm_ret) leaves CS's D bit = 1, which makes the 16-bit
; real-mode code at .rm_ret mis-decode (a 16-bit near call reads as 32-bit)
; and hang.  This descriptor fixes it.
gdt_rm16:
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 00000000b, 0x00
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start
RM16_SEG equ gdt_rm16 - gdt_start

; =====================================================================
;  Patched by tools/make_cd_boot.py at the following file offsets
;  (within the 2048-byte boot sector that -boot-load-size 4 loads):
;    0x7F0  sfs_cd_sects    (uint32)  -> SFS image size in CD sectors
;    0x7F4  kernel_cd_sects (uint32)  -> kernel image size in CD sectors
;  These sit in the tail of the boot sector, past the 56-byte boot-info
;  table window (xorriso patches offsets 8-63) so they are never clobbered.
; =====================================================================
times (0x7F0 - ($ - $$)) db 0
sfs_cd_sects:     dd 0
kernel_cd_sects:  dd 0
times (2048 - ($ - $$)) db 0
