; =====================================================================
;  svm_host_test.asm - standalone AMD-V (SVM) bring-up test
; =====================================================================
;
;  WHAT THIS IS
;  ------------
;  A multi-sector boot image containing a small hypervisor that exercises
;  EXACTLY the same VMCB layout, VMRUN sequence, and #VMEXIT dispatch that
;  plugins/vmm.cpp uses for the NexOS built-in VMM -- standalone, no NexOS.
;
;  It boots under QEMU TCG with -cpu +svm (QEMU emulates AMD-V in software),
;  enables EFER.SVME, builds a VMCB for the minimal guest
;  (plugins/guest_smoke.asm), VMRUNs it, services the I/O exits (forwarding
;  the guest's UART bytes to the real serial port), and prints SVMOK when
;  the guest halts.
;
;  WHY A SEPARATE MIRROR
;  ---------------------
;  A mistake in a VMCB offset or a segment attribute bit produces an
;  immediate #VMFAIL with zero output, which is miserable to debug inside a
;  full kernel.  This test isolates the plumbing: if SVMOK appears on
;  serial, the VMCB layout and the exit loop are proven, and the same
;  constants can be trusted in vmm.cpp.  Every constant here MUST match
;  vmm.cpp.
;
;  LAYOUT (loaded from disk by the BIOS via int 0x13)
;    sector 1..8   -> 0x2000   this 32-bit SVM host
;    sector 9..10  -> 0x1000   guest payload (guest_smoke.bin)
;
;  Test-image memory map:
;    0x01000  guest payload, runs there
;    0x06000  guest GDT
;    0x08000  guest stack
;    0x09000  SVM host save area (VM_HSAVE_PA)
;    0x0A000  VMCB (control area 0x000-0x3FF, save area 0x400-0xFFF)
;    0x0B000  IOPM + MSRPM (zeroed: intercept nothing beyond the masks)
; =====================================================================

; =====================================================================
;  Sector 0: 16-bit loader (loads the rest of the image, then jumps)
; =====================================================================
BITS 16
org 0x7C00

SECTORS_TO_LOAD equ 10              ; sectors 2..11 of the image

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti

    ; ---- load the SVM host (10 sectors) to 0x2000 ------------------
    mov     ax, 0x0200
    mov     es, ax
    xor     bx, bx
    mov     ah, 0x02
    mov     al, SECTORS_TO_LOAD
    mov     ch, 0
    mov     cl, 2                   ; start at sector 2 (1-based)
    mov     dh, 0
    mov     dl, 0x80
    int     0x13
    jc      fail

    ; ---- load the guest payload (2 sectors) to 0x1000 --------------
    mov     ax, 0x0100
    mov     es, ax
    xor     bx, bx
    mov     ah, 0x02
    mov     al, 2
    mov     ch, 0
    mov     cl, 12                  ; payload starts at sector 12 (1-based)
    mov     dh, 0
    mov     dl, 0x80
    int     0x13
    jc      fail

    ; enable A20
    in      al, 0x92
    or      al, 2
    out     0x92, al

    ; flat GDT, protected mode
    lgdt    [gdt_desc]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     0x08:0x2000             ; into the SVM host

fail:
    mov     dx, 0x3F8
    mov     al, '!'
    out     dx, al
.h: hlt
    jmp     .h

; ---- host GDT (16-bit trampoline only) ------------------------------
align 8
gdt_start:
    dq 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

times 510-($-$$) db 0
dw 0xAA55

; =====================================================================
;  Sector 1.. : 32-bit SVM host, loaded at 0x2000
;  NOTE: no second `org` -- NASM allows only one per file.  All host
;  addressing below is absolute (VMCB_BASE, 0x6000, ...), and the only
;  relative reference (okmsg) is loaded into a register via a label,
;  so the code is position-independent enough to run at 0x2000.
; =====================================================================
BITS 32

; ---- SVM constants (MUST mirror plugins/vmm.cpp) ---------------------
VMCB_BASE       equ 0xA000
IOIO_EXIT       equ 0x7B
HLT_EXIT        equ 0x78
IOIO_PORT_SHIFT equ 16
IOIO_TYPE_IN    equ 0x08            ; exitinfo1 bit 3 = IN

host_start:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, 0x8000

    ; announce: 'H' = host entered protected mode
    mov     dx, 0x3F8
    mov     al, 'H'
    out     dx, al

    ; ---- 1) EFER.SVME = 1 -----------------------------------------
    mov     ecx, 0xC0000080         ; MSR_EFER
    rdmsr
    or      eax, 0x1000             ; SVME (bit 12)
    wrmsr

    ; ---- 2) host save area (VM_HSAVE_PA) --------------------------
    mov     ecx, 0xC0010117         ; MSR_VM_HSAVE_PA
    mov     eax, 0x9000
    xor     edx, edx
    wrmsr

    ; ---- 3) zero the VMCB page and the IOPM/MSRPM page ------------
    mov     edi, VMCB_BASE
    xor     eax, eax
    mov     ecx, 1024
    rep     stosd
    mov     edi, 0xB000
    mov     ecx, 1024
    rep     stosd

    ; ---- 4) VMCB control area -------------------------------------
    ; intercept vector3 byte2: bit7 (23) = all port IO, bit4 (20) = HLT
    mov     dword [VMCB_BASE+0x0C], 0x00A00000
    mov     dword [VMCB_BASE+0x30], 0xB000  ; IOPM base  (low dword)
    mov     dword [VMCB_BASE+0x38], 0xB000  ; MSRPM base (low dword)
    mov     dword [VMCB_BASE+0x48], 1       ; guest ASID

    ; ---- 5) guest GDT (flat code/data) at 0x6000 ------------------
    mov     dword [0x6000], 0
    mov     dword [0x6004], 0
    mov     dword [0x6008], 0xFFFF0000
    mov     dword [0x600C], 0x00CF9A00      ; code: base 0, limit 4G, 32-bit
    mov     dword [0x6010], 0xFFFF0000
    mov     dword [0x6014], 0x00CF9300      ; data: base 0, limit 4G, 32-bit

    ; ---- 6) VMCB save area: guest segments ------------------------
    ; layout: u16 selector; u16 attrib; u32 limit; u64 base
    ; attrib: bits3:0 type | bit4 S | bit7 P | bit10 D/B | bit11 G
    ;   flat code = 0x0C9B   flat data = 0x0C93
    ; ES
    mov     word [VMCB_BASE+0x400], 0x10
    mov     word [VMCB_BASE+0x402], 0x0C93
    mov     dword [VMCB_BASE+0x404], 0xFFFFFFFF
    ; CS
    mov     word [VMCB_BASE+0x410], 0x08
    mov     word [VMCB_BASE+0x412], 0x0C9B
    mov     dword [VMCB_BASE+0x414], 0xFFFFFFFF
    ; SS
    mov     word [VMCB_BASE+0x420], 0x10
    mov     word [VMCB_BASE+0x422], 0x0C93
    mov     dword [VMCB_BASE+0x424], 0xFFFFFFFF
    ; DS
    mov     word [VMCB_BASE+0x430], 0x10
    mov     word [VMCB_BASE+0x432], 0x0C93
    mov     dword [VMCB_BASE+0x434], 0xFFFFFFFF
    ; FS
    mov     word [VMCB_BASE+0x440], 0x10
    mov     word [VMCB_BASE+0x442], 0x0C93
    mov     dword [VMCB_BASE+0x444], 0xFFFFFFFF
    ; GS
    mov     word [VMCB_BASE+0x450], 0x10
    mov     word [VMCB_BASE+0x452], 0x0C93
    mov     dword [VMCB_BASE+0x454], 0xFFFFFFFF
    ; GDTR: limit @+0, base @+8 (APM packed layout)
    mov     dword [VMCB_BASE+0x460], 0x17
    mov     dword [VMCB_BASE+0x468], 0x6000
    ; IDTR: left zeroed (base 0, limit 0)
    ; TR: present, busy 32-bit TSS
    mov     word [VMCB_BASE+0x490], 0
    mov     word [VMCB_BASE+0x492], 0x008B
    mov     dword [VMCB_BASE+0x494], 0xFFFF

    ; ---- 7) guest control registers and entry state ---------------
    mov     dword [VMCB_BASE+0x548], 0          ; CR4
    mov     dword [VMCB_BASE+0x550], 0          ; CR3
    mov     dword [VMCB_BASE+0x560], 0x11       ; CR0 = PE | ET
    mov     dword [VMCB_BASE+0x568], 0x400      ; DR7
    mov     dword [VMCB_BASE+0x578], 0x2        ; RFLAGS (bit1 reserved 1)
    mov     dword [VMCB_BASE+0x580], 0x1000     ; RIP  = GUEST_CODE_PHYS
    mov     dword [VMCB_BASE+0x5E0], 0x8000     ; RSP
    mov     dword [VMCB_BASE+0x5F8], 0          ; RAX

; ---- VMRUN loop ------------------------------------------------------
.run:
    mov     eax, VMCB_BASE
    db      0x0F, 0x01, 0xD8            ; VMRUN (implicit rAX)
    ; on #VMEXIT: exit code / info in the VMCB; host state restored
    mov     eax, [VMCB_BASE+0x50]       ; EXITCODE
    cmp     eax, IOIO_EXIT
    je      .ioio
    cmp     eax, HLT_EXIT
    je      .hlt

    ; unexpected exit: print 'X' + exit code in hex, then stop
    mov     dx, 0x3F8
    mov     al, 'X'
    out     dx, al
    mov     eax, [VMCB_BASE+0x50]
    mov     ebx, 8
.hex:
    mov     ecx, ebx
    dec     ecx
    shl     ecx, 2
    mov     edx, eax
    shr     edx, cl
    and     edx, 0xF
    cmp     edx, 10
    jb      .digit
    add     edx, 'A' - 10
    jmp     .emit
.digit:
    add     edx, '0'
.emit:
    mov     dx, 0x3F8
    mov     al, dl
    out     dx, al
    dec     ebx
    jnz     .hex
    jmp     .hang

.ioio:
    mov     eax, [VMCB_BASE+0x58]       ; EXITINFO1
    mov     ebx, eax
    shr     ebx, IOIO_PORT_SHIFT        ; port number
    test    eax, IOIO_TYPE_IN
    jnz     .isin
    ; OUT: forward the guest's byte (save.RAX low byte) to the same port
    mov     eax, [VMCB_BASE+0x5F8]      ; save.RAX
    mov     dx, bx
    out     dx, al
    jmp     .resume
.isin:
    mov     dword [VMCB_BASE+0x5F8], 0  ; IN returns 0
.resume:
    ; advance guest RIP past the I/O instruction and re-enter
    mov     eax, [VMCB_BASE+0xA0]       ; NEXT_RIP
    mov     [VMCB_BASE+0x580], eax      ; save.RIP = next_rip
    jmp     .run

.hlt:
    ; No second `org` is allowed in one NASM file, so labels in this host
    ; section assemble to their FILE offsets.  The host runs at 0x2000 but
    ; lives at file offset 512, so every data reference needs the fixup
    ; RUN_BASE - FILE_BASE = 0x2000 - 512 = 0x1E00.
    mov     esi, okmsg + 0x1E00
.prt:
    mov     al, [esi]
    test    al, al
    jz      .hang
    mov     dx, 0x3F8
    out     dx, al
    inc     esi
    jmp     .prt

.hang:
    hlt
    jmp     .hang

okmsg: db "SVMOK", 10

; pad the host to its load size (10 sectors = 5120 bytes)
times 5120 - ($ - $$) db 0

; =====================================================================
;  Guest payload sector area (sectors 11.. of the image, i.e. 1-based
;  sector 12 as the loader expects)
; =====================================================================
TIMES_ALIGN:
times (11*512) - ($ - $$) db 0     ; skip to offset 11*512 (0-based)
payload_src:
    incbin PAYLOAD_FILE
payload_end:

times (13*512) - ($ - $$) db 0     ; total image: 13 sectors
