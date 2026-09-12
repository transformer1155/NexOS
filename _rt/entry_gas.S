# entry_gas.S - GAS (AT&T/Intel) port of entry.asm so the 32-bit kernel can be
# built with the i686-elf cross assembler (no nasm on the Windows side).
# Functionally identical to the NASM version: serial 'S', set up the 32-bit
# stack, enable FPU/SSE/AVX, zero .bss, serial 'F'/'B', call kmain.
.intel_syntax noprefix
.code32

.globl _start
.globl fpu_init
.extern kmain
.extern __bss_start
.extern __bss_end
.extern __k32_stack_top

.text
_start:
    mov  dx, 0x3F8
    mov  al, 0x53               # 'S' = kernel _start reached
    out  dx, al

    lea  esp, [__k32_stack_top]   # valid 32-bit stack above .bss (load ADDRESS)

    mov  eax, cr0
    and  eax, ~0x04             # clear EM
    and  eax, ~0x08             # clear TS
    or   eax, 0x02              # set MP
    or   eax, 0x20              # set NE
    mov  cr0, eax

    mov  eax, cr4
    or   eax, 0x200            # OSFXSR (bit 9)
    or   eax, 0x400            # OSXMMEXCPT (bit 10)
    mov  cr4, eax

    mov  eax, 1
    cpuid
    test ecx, 0x10000000       # CPUID.1:ECX.AVX
    jz   .no_avx
    mov  eax, 7
    xor  ecx, ecx
    cpuid
    test ebx, 0x20             # CPUID.7:EBX.AVX2
    jz   .no_avx
    mov  eax, cr4
    or   eax, 0x40000          # CR4.OSXSAVE (bit 18)
    mov  cr4, eax
    xor  edx, edx
    mov  eax, 7                # XCR0 = x87 | SSE | AVX
    xor  ecx, ecx
    xsetbv
.no_avx:
    fninit

    mov  dx, 0x3F8
    mov  al, 0x46               # 'F' = FPU initialised
    out  dx, al

    lea  edi, [__bss_start]
    lea  ecx, [__bss_end]
    sub  ecx, edi
    xor  eax, eax
    cld
    rep  stosb

    mov  dx, 0x3F8
    mov  al, 0x42               # 'B' = BSS cleared
    out  dx, al

    call kmain
.hang:
    cli
    hlt
    jmp  .hang

fpu_init:
    fninit
    ret
