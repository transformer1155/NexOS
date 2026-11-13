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

    /* .bss is relocated to 0x300000 -- deliberately NOT contiguous with .data.
       It is NOBITS, so moving it costs nothing in the flat binary, and it keeps
       the 0x100000..0x240000 window free for the 64-bit blit target (see
       K64_STAGE_FLOOR in kernel.cpp, which must match this address). */
    . = 0x240000;

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Link-time capacity assertion, symmetric to the 32-bit linker.ld
       (ASSERT(__kimage_end <= 0x9FC00)).

       Why it matters: without it an over-large .bss simply runs past the heap
       start, the heap then overlaps kernel data, and the machine dies silently
       at the first big allocation -- no fault, no diagnostic.  That is exactly
       what happened when the AI modules pushed .bss past HEAP_START: a
       200-second mystery instead of a one-line build error.

       SECOND SOURCE OF TRUTH: HEAP_START is a C++ constant in kernel64.cpp:2381
       (0x500000), not a linker symbol.  If you change one, change BOTH.  The
       durable fix (have the constant reference this symbol) is deliberate
       technical debt, not an oversight. */
    ASSERT(__bss_end <= 0x600000,
        "kernel64 .bss overruns HEAP_START (see kernel64.cpp:2381) -- raise HEAP_START or shrink .bss")

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.*)
        *(.gnu.hash)
    }
}
