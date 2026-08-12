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

    /* -----------------------------------------------------------------
     *  .bss is relocated above 1 MiB.
     *
     *  The kernel image starts at 0x10000 and .text/.rodata/.data now
     *  occupy ~0x4A000 bytes.  If .bss followed them contiguously it grew
     *  past 0xA0000 straight into the legacy VGA MMIO hole
     *  (0xA0000-0xBFFFF), which is NOT RAM: writes are swallowed by the
     *  video adapter and reads come back as 0xFF.  Globals that landed
     *  there (win32's g_reader was the very last object in .bss) silently
     *  read back as 0xFFFFFFFF, and calling through such a "pointer"
     *  page-faulted the kernel.
     *
     *  .bss is NOBITS, so moving it costs nothing in the flat binary.
     *  0x120000 .. 0x200000 gives ~896 KiB of BSS head-room and stops
     *  right where the kernel heap (HEAP_START) begins.
     * ----------------------------------------------------------------- */
    . = 0x120000;

    .bss (NOLOAD) : {
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
