; =====================================================================
;  ap_trampoline.asm  -  Application Processor (AP) start code
; ---------------------------------------------------------------------
;  This 16-bit real-mode blob is copied by the BSP (smp_init) to a
;  low-memory page (default 0x7000) and then APs are released via the
;  INIT-SIPI-SIPI sequence with the SIPI vector pointing at it.
;
;  It must be POSITION-INDEPENDENT in 16-bit mode (only relative
;  jumps / segment-relative data), because it is relocated at runtime.
;
;  Flow:
;    1. real mode: set up a temporary 32-bit stack, load a 64-bit GDT
;       (kept in this blob), enable PAE + long mode using the BSP's
;       page tables (CR3 value patched in by the BSP at tramp_cr3),
;       far-jump into 64-bit code at tramp_ap64 (patched by BSP).
;    2. long mode: set segments, per-CPU stack, call ap_main().
;
;  The BSP patches three dwords inside this blob before sending SIPI:
;    tramp_cr3   - BSP CR3 (physical PML4 base, identity-mapped)
;    tramp_ap64  - linear address of ap_long_entry
;    tramp_cpu   - logical CPU index this AP should assume
; =====================================================================

[BITS 16]
[ORG 0x7000]

ap_trampoline_start:
    ; NOTE: the BSP patches two dwords at the very start of the blob BEFORE
    ; releasing the APs:
    ;   offset +2 : tramp_cr3  (BSP PML4 physical base, identity-mapped)
    ;   offset +6 : tramp_cpu  (logical CPU index this AP assumes)
    ; The first instruction (jmp short) jumps over this scratch area so the
    ; AP does not execute the patch data as code.
    jmp short enter64
    tramp_cr3:      dd 0
    tramp_cpu:      dd 0
    tramp_ap_entry: dq 0        ; 64-bit address of C ap_main(), patched by BSP
enter64:
    ; --- disable interrupts, use our own segment (CS==0x700 after SIPI
    ;     but we were loaded at ORG 0, so just use CS-relative) ---
    cli
    ; DEBUG marker: '1' on COM1 (AP started executing trampoline)
    mov dx, 0x3F8
    mov al, '1'
    out dx, al
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; temporary stack just below the blob (0x7000 + 0x1000)
    mov sp, 0x1000

    ; --- load a 64-bit-capable GDT living inside this blob ---
    lgdt [gdtr]

    ; --- enter 32-bit protected mode (flat 4GiB) ---
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x18:.pm32         ; far jump using 32-bit compat code sel (index 3)

[BITS 32]
.pm32:
    ; DEBUG marker: '2' on COM1
    mov dx, 0x3F8
    mov al, '2'
    out dx, al
    mov ax, 0x20           ; 32-bit data sel (index 4)
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x9000        ; temp 32-bit stack (low mem, safe)

    ; --- enable PAE ---
    mov eax, cr4
    or  eax, (1 << 5)      ; CR4.PAE
    mov cr4, eax

    ; --- load BSP page tables (patched by BSP at tramp_cr3) ---
    mov eax, [tramp_cr3]
    mov cr3, eax

    ; --- set EFER.LME (MSR 0xC0000080) ---
    mov ecx, 0xC0000080
    rdmsr
    or  eax, (1 << 8)      ; EFER.LME
    wrmsr

    ; --- enable paging -> long mode active ---
    mov eax, cr0
    or  eax, (1 << 31)     ; CR0.PG
    mov cr0, eax

    ; DEBUG marker: '3' on COM1 (just before the 64-bit far jump)
    mov dx, 0x3F8
    mov al, '3'
    out dx, al

    ; --- far jump into the 64-bit code segment (sel 0x08).  ap_long_entry's
    ;     offset (identity-mapped) is the destination.  The CPU switches to
    ;     64-bit mode on this far branch. ---
    jmp 0x08:ap_long_entry

; ---- 64-bit landing ----
[BITS 64]
ap_long_entry:
    ; DEBUG marker: '4' on COM1 (in long mode now)
    mov dx, 0x3F8
    mov al, '4'
    out dx, al
    ; reload segments with 64-bit data selector (index 2 = 0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; per-CPU stack inside the BSP-mapped kernel region (identity-mapped by
    ; the PML4 we inherited): 0x180000 + cpu*0x4000, top of a 16KiB stack.
    ; (CPU index patched by BSP.)
    mov eax, [tramp_cpu]
    and eax, 0xFF                ; 0..MAX_CPUS-1
    mov rax, 0
    mov eax, eax
    shl rax, 14                  ; *0x4000
    add rax, 0x180000
    add rax, 0x4000             ; top of this AP's 16KiB stack
    mov rsp, rax

    ; Pass the logical cpu index to ap_main() in rdi (System V AMD64 ABI:
    ; first integer argument).  This avoids needing a shared global.
    mov edi, [tramp_cpu]

    ; Jump to the C ap_main() whose address the BSP patched into tramp_ap_entry.
    mov rax, [tramp_ap_entry]
    ; DEBUG marker 'Z' just before the jump into C ap_main
    mov dx, 0x3F8
    mov al, 'Z'
    out dx, al
    jmp rax

.halt:
    cli
    hlt
    jmp .halt

; ---- 64-bit GDT (inside blob) ----
;   0 null
;   1 64-bit code  (L=1)   sel 0x08   <- final long mode
;   2 64-bit data  (L=1)   sel 0x10
;   3 32-bit code  (L=0,D=1) sel 0x18 <- compat mode on the way up
;   4 32-bit data  (L=0,D=1) sel 0x20
align 8
gdt64:
    dq 0x0000000000000000        ; 0 null
    dq 0x00AF9A000000FFFF        ; 1 64-bit code (L=1)
    dq 0x00CF92000000FFFF        ; 2 64-bit data
    dq 0x00CF9A000000FFFF        ; 3 32-bit code (L=0, D=1)
    dq 0x00CF92000000FFFF        ; 4 32-bit data
gdt64_end:

gdtr:
    dw gdt64_end - gdt64 - 1
    dd gdt64                     ; 32-bit offset (blob is at 0x7000, org 0)

; ---- end marker ----
ap_trampoline_end:
