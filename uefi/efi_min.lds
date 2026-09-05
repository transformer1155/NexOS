/* Minimal linker script for a gnu-efi-free UEFI PE32+ application.
 * Mirrors the layout gnu-efi's elf_x86_64_efi.lds uses; gen_reloc.py
 * patches R_X86_64_RELATIVE fixups (from the .so) into the .reloc
 * section afterwards. */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : {
        *(.text .text.* .stub)
    }
    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }
    . = ALIGN(4096);
    .sdata : {
        *(.sdata .sdata.*)
    }
    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }
    .dynamic : { *(.dynamic) }
    .dynsym  : { *(.dynsym) }
    .dynstr  : { *(.dynstr) }
    . = ALIGN(4096);
    .bss : {
        *(.bss .bss.* COMMON)
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
