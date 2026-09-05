; =====================================================================
;  entry64.asm  -  64-bit kernel entry point
; ---------------------------------------------------------------------
;  NASM ELF64 format, linked as first object in kernel64.elf
;  Entry address: 0x100000 (1MB), page-aligned for 2MB pages
;
;  Responsibilities:
;    1. Debug output ('6' on serial)
;    2. Set up 64-bit stack
;    3. Enable FPU + SSE
;    4. Load permanent GDT (with 64-bit + 32-bit compat segments)
;    5. Clear BSS
;    6. Call kmain64()
; =====================================================================

[BITS 64]

%macro DBG 1
    mov dx, 0x3FD
%%w:
    in al, dx
    test al, 0x20             ; THRE
    jz %%w
    mov dx, 0x3F8
    mov al, %1
    out dx, al
%endmacro

global _start64
extern kmain64
extern __bss_start
extern __bss_end

section .text

_start64:
    ; ---- Serial debug: 'E' for entry64 reached (immediate, no wait so it
    ;      can never be dropped/delayed before a possible early crash) ----
    mov dx, 0x3F8
    mov al, 0x45           ; 'E'
    out dx, al

    ; ---- Set up 64-bit stack ----
    mov rsp, 0x1F0000

    ; ---- Enable FPU (x87) + SSE ----
    mov rax, cr0
    and rax, ~(1 << 2)     ; Clear EM
    and rax, ~(1 << 3)     ; Clear TS
    or rax, (1 << 1)       ; Set MP
    or rax, (1 << 5)       ; Set NE
    mov cr0, rax
    DBG 0x46               ; 'F' FPU/cr0 done

    mov rax, cr4
    or rax, (1 << 9)       ; OSFXSR (SSE)
    or rax, (1 << 10)      ; OSXMMEXCPT
    mov cr4, rax
    fninit                  ; Init x87 FPU
    DBG 0x47               ; 'G' cr4+fninit done

    ; ---- Load permanent GDT ----
    lgdt [rel gdt64_desc]
    DBG 0x48               ; 'H' lgdt done

    ; Reload segment registers with new GDT selectors
    mov ax, 0x10            ; 64-bit data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    DBG 0x49               ; 'I' segs reloaded

    ; ---- Clear BSS ----
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi            ; byte count
    DBG 0x4A               ; 'J' about to clear bss (rcx below)
    xor rax, rax
    cld
    rep stosb
    DBG 0x4B               ; 'K' bss cleared

    ; ---- Call C++ kernel ----
    DBG 0x4C               ; 'L' about to call kmain64
    call kmain64

.hang:
    cli
    hlt
    jmp .hang

; =====================================================================
;  GDT for 64-bit kernel
;  Contains both 64-bit and 32-bit compat segments (for switch back)
; =====================================================================
section .data
align 8

gdt64:
    dq 0x0000000000000000       ; 0x00: Null descriptor
    dq 0x00AF9A000000FFFF       ; 0x08: 64-bit code (L=1, D=0, G=1)
    dq 0x00CF92000000FFFF       ; 0x10: 64-bit data (flat, G=1, D=1)
    dq 0x00CF9A000000FFFF       ; 0x18: 32-bit compat code (L=0, D=1)
    dq 0x00CF92000000FFFF       ; 0x20: 32-bit data (flat)
gdt64_end:

; 64-bit GDT descriptor (10 bytes: 2 limit + 8 base)
gdt64_desc:
    dw gdt64_end - gdt64 - 1
    dq gdt64

; =====================================================================
;  64-bit IDT + fault ISRs (diagnostic)
;  Generated so that ANY exception / fault (#PF, #GP, #UD, #DE, ...) lands
;  in fault_common() (C, kernel64.cpp), which prints CR2/RIP/step to the
;  serial port and writes a 512-byte record to LBA34 on disk.  This replaces
;  the old "paint a full-screen color and guess where we hung" approach.
;
;  Safety: the 64-bit kernel NEVER enables hardware interrupts (no STI
;  anywhere in kernel64.cpp), so loading a real IDT only catches synchronous
;  CPU faults (page fault / GPF / #UD / ...).  It CANNOT trigger an IRQ storm.
;  The table is identity-mapped already (link base 0x100000 == physical).
; =====================================================================
section .text

; --- Per-vector stubs -------------------------------------------------
; Vectors the CPU pushes an error code for: push only the vector number.
; All other vectors: push a dummy errcode (0) then the vector number.
; Both branches fall through to isr_common with a uniform 7-qword frame:
;   [intno, errcode, rip, cs, rflags, rsp, ss]
%assign v 0
%rep 256
    %assign iserr 0
    %if (v == 8) || (v == 10) || (v == 11) || (v == 12) || (v == 13) || (v == 14) || (v == 17) || (v == 30)
        %assign iserr 1
    %endif
    isr_stub_%+ v:
    %if iserr
        push v                      ; error code already on stack -> just vector#
    %else
        push 0                      ; dummy error code
        push v                      ; vector number
    %endif
        jmp isr_common
    %assign v v+1
%endrep

; --- Common fault entry ----------------------------------------------
; Stack at entry: [intno(8), errcode(8), rip(8), cs(8), rflags(8), rsp(8), ss(8)]
; Pass to fault_common(intno, errcode, rip, cs, rflags, cr2) per SysV ABI
; (rdi, rsi, rdx, rcx, r8, r9).  CR2 is read before any other faulting
; access so it stays accurate for #PF.
global isr_common
global isr_stub_0
isr_common:
    mov rdi, [rsp + 0]             ; intno
    mov rsi, [rsp + 8]             ; errcode
    mov rdx, [rsp + 16]            ; rip
    mov rcx, [rsp + 24]            ; cs
    mov r8,  [rsp + 32]            ; rflags
    mov rax, cr2
    mov r9,  rax                   ; cr2
    call fault_common
    ; fault_common never returns (it halts).  If it does, halt hard.
.halt:
    cli
    hlt
    jmp .halt

section .data
align 8
; Table of 256 stub entry points.  C-side build_idt() reads this table and
; packs each address into a gate.  Addresses are identity-mapped (physical
; == virtual at 0x100000 base), so they can be split into offlow/offmid/offhigh
; directly.
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 256
    dq isr_stub_%+ v
    %assign v v+1
%endrep

extern fault_common
