/* =====================================================================
 *  linker.ld  -  link script for the C++ kernel
 * ---------------------------------------------------------------------
 *  The kernel is loaded at and linked to 0x10000. entry.o is linked
 *  first, so _start (the entry stub) is the very first byte of the
 *  flat binary -- exactly where Stage 2 jumps.
 * ===================================================================== */

ENTRY(_start)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text)          /* entry.o's _start is first (command-line order) */
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
