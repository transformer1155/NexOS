/* =====================================================================
 *  enter_kernel.S  -  Transition from x86-64 long mode to kernel entry
 * ---------------------------------------------------------------------
 *  After UEFI ExitBootServices, the CPU is in 64-bit long mode with
 *  paging enabled (UEFI identity-maps all RAM).
 *
 *  Strategy (simplified, robust):
 *    1. Load a flat 32-bit GDT (RIP-relative to survive PE relocation).
 *    2. Build and load a safety-net IDT (256 entries, all pointing to
 *       a 64-bit halt handler).  This prevents triple-faults if any
 *       unexpected exception fires before the kernel sets up its own.
 *    3. Far-return into a 32-bit *compatibility-mode* segment.
 *       (We stay inside long mode but execute 32-bit code.  UEFI's
 *        identity-mapped page tables remain active, so all physical
 *        addresses the kernel uses are directly accessible.)
 *    4. Load flat data segments + stack.
 *    5. Jump to the C++ kernel at 0x10000 (same entry as BIOS path).
 *
 *  This avoids the fragile CR0.PG / CR4.PAE / EFER.LME teardown that
 *  can triple-fault if any page table or code address isn't perfectly
 *  identity-mapped.  The 32-bit kernel runs perfectly in compat mode:
 *  all I/O, VGA, and memory accesses behave identically.
 *
 *  Serial debug markers are sent to port 0x3F8 at each stage so the
 *  boot log shows exactly how far execution gets.
 * ===================================================================== */

    .text
    .code64

    .global enter_kernel

    /* I/O port for QEMU serial (already initialised by OVMF) */
    .equ SERIAL_PORT, 0x3F8

enter_kernel:
    cli

    /* ---- Debug: 'E' = entered enter_kernel ---- */
    movb    $0x45, %al
    movw    $SERIAL_PORT, %dx
    outb    %al, %dx

    /* ---- 1. Build GDTR on stack and load GDT ----
       Use RIP-relative LEA to get the GDT base at runtime,
       avoiding absolute-address relocations that the PE loader may
       not apply correctly. */
    leaq    gdt_boot_base(%rip), %rsi

    subq    $16, %rsp
    movzwq  gdt_limit(%rip), %rax       /* limit */
    movw    %ax, 0(%rsp)                /* store limit (2 bytes) */
    movq    %rsi, 2(%rsp)               /* store base (8 bytes) */
    lgdt    (%rsp)
    addq    $16, %rsp

    /* ---- Debug: 'G' = GDT loaded ---- */
    movb    $0x47, %al
    movw    $SERIAL_PORT, %dx
    outb    %al, %dx

    /* ---- 2. Build and load safety-net IDT ----
       Fill all 256 entries with a pointer to fault_handler (64-bit
       interrupt gate).  This ensures that if any exception fires
       before the kernel initialises its own IDT, the CPU jumps to
       our handler instead of triple-faulting. */
    leaq    idt_table(%rip), %rdi       /* RDI = IDT base */
    leaq    fault_handler(%rip), %rsi   /* RSI = handler address */
    movl    $256, %ecx
.fill_idt:
    movw    %si, 0(%rdi)               /* offset 15:0 */
    movw    $CODE64_SEL, 2(%rdi)       /* segment selector (64-bit code) */
    movb    $0, 4(%rdi)                /* IST = 0 */
    movb    $0x8E, 5(%rdi)             /* P=1 DPL=0 S=0 type=0xE (64-bit intr gate) */
    movq    %rsi, %rax
    shrq    $16, %rax
    movw    %ax, 6(%rdi)               /* offset 31:16 */
    shrq    $16, %rax
    movl    %eax, 8(%rdi)             /* offset 63:32 */
    movl    $0, 12(%rdi)              /* reserved */
    addq    $16, %rdi
    decl    %ecx
    jnz     .fill_idt

    /* Load IDTR */
    subq    $16, %rsp
    movw    $(256 * 16 - 1), 0(%rsp)   /* limit */
    leaq    idt_table(%rip), %rax
    movq    %rax, 2(%rsp)              /* base */
    lidt    (%rsp)
    addq    $16, %rsp

    /* ---- Debug: 'I' = IDT loaded ---- */
    movb    $0x49, %al
    movw    $SERIAL_PORT, %dx
    outb    %al, %dx

    /* ---- 3. Far jump to 32-bit compatibility mode via retf ----
       Push new CS (CODE32_SEL) and RIP, then lretq. */
    movl    $CODE32_SEL, %ecx
    pushq   %rcx                        /* new CS (zero-extended) */
    leaq    compat32(%rip), %rax
    pushq   %rax                        /* new RIP */
    lretq                                /* far return = far jump */

    /* ---------------------------------------------------------------
     *  64-bit fault handler — halts with debug marker '!'
     *  This runs in 64-bit mode (selector CODE64_SEL) even if the
     *  fault occurs while executing 32-bit compat-mode code.
     * --------------------------------------------------------------- */
    .code64
fault_handler:
    movb    $0x21, %al                 /* '!' = fault caught */
    movw    $SERIAL_PORT, %dx
    outb    %al, %dx
.fh_hang:
    hlt
    jmp     .fh_hang

    /* ---------------------------------------------------------------
     *  32-bit compatibility mode (still inside IA-32e / long mode,
     *  but executing 32-bit instructions with a flat segment model).
     * --------------------------------------------------------------- */
    .code32
compat32:
    /* ---- 4. Load flat data segments + stack ---- */
    movw    $DATA32_SEL, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    $0x90000, %esp

    /* ---- Debug: '3' = now in 32-bit compat mode ---- */
    movb    $0x33, %al
    movw    $SERIAL_PORT, %dx
    outb    %al, %dx

    /* ---- 5. Jump to the C++ kernel at 0x10000 ----
       The kernel is linked at and copied to 0x10000.  UEFI identity-
       maps this address, so the far jump lands on the real code. */
    ljmp    $CODE32_SEL, $0x10000

.hang:
    hlt
    jmp     .hang

/* =====================================================================
 *  GDT  -  flat segments (base=0, limit=4 GiB)
 *  0x00: null
 *  0x08: 32-bit code (compat mode)
 *  0x10: 32-bit data
 *  0x18: 64-bit code (for IDT fault handler)
 * ===================================================================== */
    .align 16
gdt_boot_base:
    .quad   0x0000000000000000           /* 0x00: null descriptor */

gdt_boot_code32:                          /* 0x08: 32-bit code */
    .word   0xFFFF                        /* limit 15:0  */
    .word   0x0000                        /* base  15:0  */
    .byte   0x00                          /* base  23:16 */
    .byte   0x9A                          /* P=1 DPL=0 S=1  type=code */
    .byte   0xCF                          /* G=1 D=1 L=0  limit 19:16=0xF */
    .byte   0x00                          /* base  31:24 */

gdt_boot_data32:                          /* 0x10: 32-bit data */
    .word   0xFFFF
    .word   0x0000
    .byte   0x00
    .byte   0x92                          /* P=1 DPL=0 S=1  type=data */
    .byte   0xCF
    .byte   0x00

gdt_boot_code64:                          /* 0x18: 64-bit code */
    .word   0xFFFF                        /* limit 15:0  */
    .word   0x0000                        /* base  15:0  */
    .byte   0x00                          /* base  23:16 */
    .byte   0x9A                          /* P=1 DPL=0 S=1  type=code */
    .byte   0xAF                          /* G=1 D=0 L=1  limit 19:16=0xF */
    .byte   0x00                          /* base  31:24 */
gdt_boot_end:

    .equ CODE32_SEL, gdt_boot_code32 - gdt_boot_base     /* 0x08 */
    .equ DATA32_SEL, gdt_boot_data32 - gdt_boot_base     /* 0x10 */
    .equ CODE64_SEL, gdt_boot_code64 - gdt_boot_base     /* 0x18 */

/* Limit value used when building the GDTR on the stack */
gdt_limit:
    .word   gdt_boot_end - gdt_boot_base - 1

/* =====================================================================
 *  IDT table — 256 entries × 16 bytes = 4096 bytes (zero-filled,
 *  populated at runtime by the .fill_idt loop above).
 * ===================================================================== */
    .data
    .align 16
idt_table:
    .fill 256 * 16, 1, 0
