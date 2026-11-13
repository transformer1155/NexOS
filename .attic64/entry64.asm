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

    ; ---- Set up 64-bit stack --------------------------------------------
    ; The stack must sit ABOVE the 64-bit image AND its .bss, because it
    ; grows downward into whatever lies below it.
    ;
    ; This used to be a literal 0x1F0000, which was safe while kernel64.bin
    ; ended near 0x1B5510.  The image has since grown past 0x1CA8B8 and its
    ; .bss now ends at 0x48ACA8, so 0x1F0000 landed INSIDE the running kernel:
    ; the first push overwrote 64-bit code/data, which is why the serial
    ; markers came out scrambled and kmain64 was never reached.
    ;
    ; 0x1828000 is __lm_stack_top from linker.ld: the 32-bit side reserves a
    ; 64 KiB boot stack at 0x1800000..0x1828000 in the .lmboot region, which
    ; vmm_init() identity-maps and PMM marks used.  It sits above kernel64's
    ; .bss and below the 32 MiB identity-map ceiling, so it is valid from the
    ; instant long mode is entered.
    ;
    ; NOTE: entry64.o links against linker64.ld, which does not define
    ; __lm_stack_top, so this cannot be an extern -- keep it in sync with
    ; linker.ld's __lm_stack_top by hand.
    ; 64-bit stack.  It used to be 0x1828000, which sits inside the .lmboot region
; (0x1800000..0x2000000) -- the same region the long-mode page tables live in
; (CR3 = 0x1810000), so a stack there can be silently overwritten and then even
; the #PF handler triple-faults with no diagnostic output.  3 MiB is clear of
; kernel64 (0x100000..~0x1F3000) and of .lmboot.
; 64-bit stack.  8 MiB keeps it clear of both kernel images (32-bit at
; 0x10000..0x9FC00, kernel64 at 0x100000..~0x1F3000), of the .lmboot region
; (0x1800000..0x2000000) and of the ATA bus-master PRDT area (~0x3477B0).
; The earlier 0x1828000 overlapped .lmboot; 0x300000 sat next to the DMA
; buffers; 0x90000 (in the switch stub) was inside the 32-bit kernel image.
mov rsp, 0x800000

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
align 16

; ---------------------------------------------------------------------
;  64-bit TSS
;  Layout (IA-32e):  reserved(8) rsp0 rsp1 rsp2 reserved(8) ist[7]
;  reserved(8) reserved(2) iomap(2).  Total 108 bytes.
;
;  ist[0] is the stack the CPU uses for exception vector 1 (IST=1).  The
;  kernel patches it at boot with the address of a dedicated fault stack,
;  so a fault taken while a PE's own stack is exhausted still lands on a
;  known-good stack instead of double-faulting.
; ---------------------------------------------------------------------
align 16
gdt64_tss:
    dq 0                   ; reserved0
    dq 0                   ; rsp0
    dq 0                   ; rsp1
    dq 0                   ; rsp2
    dq 0                   ; reserved1
    dq 0                   ; ist[0]  <- fault stack (patched by the kernel)
    dq 0, 0, 0, 0, 0, 0    ; ist[1..6]
    dq 0                   ; reserved2
    dw 0                   ; reserved3
    dw 0                   ; iomap base
gdt64_tss_end:
global gdt64_tss
global gdt64_tss_end

align 8
gdt64:
    dq 0x0000000000000000       ; 0x00: Null descriptor
    dq 0x00AF9A000000FFFF       ; 0x08: 64-bit code (L=1, D=0, G=1)
    dq 0x00CF92000000FFFF       ; 0x10: 64-bit data (flat, G=1, D=1)
    dq 0x00CF9A000000FFFF       ; 0x18: 32-bit compat code (L=0, D=1)
    dq 0x00CF92000000FFFF       ; 0x20: 32-bit data (flat)
    ; 0x28: 64-bit TSS descriptor (16 bytes).  Needed for IST.  The bytes
    ;       are a zero placeholder; kernel64.cpp::gdt_install_tss() fills
    ;       them at boot because NASM cannot encode the TSS base relocation
    ;       (which spans the whole 16-byte descriptor).
gdt64_tss_desc:
    dq 0
    dq 0
gdt64_end:
global gdt64
global gdt64_tss_desc
global gdt64_end

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

; =====================================================================
;  V2.1 headless: IRQ0 (PIT) stub + preemptive context switch
; ---------------------------------------------------------------------
;  The 256 generic stubs above all funnel into fault_common(), which halts.
;  A hardware interrupt must NEVER take that path: it has to return with
;  iretq so the hlt loop (or the next thread) can resume.
;
;  On an interrupt gate with no CPL change the CPU pushes only
;      [rsp+0]=rip  [rsp+8]=cs  [rsp+16]=rflags
;  and clears IF.  We save the 15 GP registers below that frame, then hand
;  the frame pointer to a C scheduler.  Calling C on the interrupted stack
;  would overwrite the CPU's iret frame, so the stub first switches to a
;  dedicated 8 KiB IRQ stack (symbol exported by kernel64.cpp).
;
;  headless_irq0_c(uint64_t frame_rsp) returns the frame pointer of the
;  context to run next -- the SAME one for stage 2 (no switch), or another
;  thread's for stage 3.  Restoring rsp, popping the registers and issuing
;  iretq then lands in the selected context.
; =====================================================================
section .text
global irq0_stub
extern headless_irq0_c

section .bss
align 16
hs_irq_stack:     resb 8192
hs_irq_stack_top:

section .text
irq0_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    mov rsp, hs_irq_stack_top
    cld
    call headless_irq0_c
    mov rsp, rax
    mov al, 0x20
    out 0x20, al             ; master PIC EOI (only IRQ0 is unmasked)
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq
