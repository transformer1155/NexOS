/* =====================================================================
   linker64.ld  -  Linker script for 64-bit kernel
   ---------------------------------------------------------------------
   Load address: 0x100000 (1MB, page-aligned for 2MB pages)
   Entry point:  _start64 (entry64.o linked first => offset 0)
   ===================================================================== */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start64)

SECTIONS
{
    . = 0x100000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.*)
        *(.gnu.hash)
    }
}
