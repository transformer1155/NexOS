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

    /* Global-constructor tables.  Listed explicitly (instead of leaving
     * them to ld's orphan placement) so __kimage_end below is exact. */
    .ctors : {
        *(.ctors)
        *(.ctors.*)
    }
    .dtors : {
        *(.dtors)
        *(.dtors.*)
    }

    /* -----------------------------------------------------------------
     *  __kimage_end  -  one past the last byte of the flat binary.
     *
     *  GUARD RAILS (added 2026-08-19).  Everything the boot path touches
     *  below 1 MiB is a hard-coded address, and the image quietly growing
     *  into one of them corrupts the kernel with no diagnostic at all:
     *
     *    - 0x9FC00 is the EBDA.
     *    - stage2.asm reads KERNEL_SECTORS (1152) sectors = 576 KiB to
     *      0x10000, so bytes past 0x10000+576 KiB are simply never loaded
     *      and the kernel executes absent memory.
     * ----------------------------------------------------------------- */
    __kimage_end = .;
    ASSERT(__kimage_end <= 0x9FC00,
        "kernel.bin grew into the EBDA at 0x9FC00 -- shrink the image")
    ASSERT(__kimage_end - 0x10000 <= 1152 * 512,
        "kernel.bin exceeds stage2.asm KERNEL_SECTORS (1152) -- raise it there AND KERNEL32_SECTORS in .attic64/kernel64.cpp")

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

    /* =================================================================
     *  Boot-critical scratch region  (added 2026-08-19)
     * -----------------------------------------------------------------
     *  ROOT CAUSE THIS FIXES
     *
     *  The 32-bit boot stack (entry.asm) and the long-mode page tables
     *  (.attic64/switch32to64.asm) were both hard-coded at 0x90000:
     *  the stack grew DOWN from 0x90000 and the tables occupied
     *  0x90000..0x94FFF.  That was only ever safe while the flat image
     *  stayed well below 0x90000.
     *
     *  It stopped being safe.  With .text+.rodata at ~0x80000 the .data
     *  section landed at 0x8FE00..0x90560, which means:
     *
     *    1. gdt64 / gdt64_code / gdt64_data / gdt64_desc -- the GDT that
     *       switch_to_64bit's `lgdt` loads -- sat at 0x8FE00..0x8FE18,
     *       i.e. 512 BYTES below the stack top.  Any call chain deeper
     *       than 512 bytes overwrote the whole 64-bit GDT.  The far jump
     *       still limped through on a half-plausible code descriptor and
     *       then `mov ss, 0x10` #GP'd -- triple fault, endless reboot,
     *       serial stopping right after the 'g' marker.
     *
     *    2. .data past 0x90000 (g_current, g_kernel_proc, g_skills, ...)
     *       was overwritten by the PML4/PDPT entries themselves.
     *
     *  Both are now linker-reserved instead of hard-coded, so the region
     *  moves automatically as the image grows and can never overlap it.
     *  PMM marks __bss_start..__lmboot_end as used, so pmm_alloc_page()
     *  cannot hand out the live page tables either.
     * -----------------------------------------------------------------
     *  WHY 24 MiB AND NOT "JUST AFTER .bss"
     *
     *  CR3 keeps pointing at these tables for the entire life of the
     *  64-bit kernel, so the region has to be clear of BOTH kernels'
     *  allocators, not just the 32-bit one:
     *
     *      32-bit .bss          0x00120000 .. 0x00452A14
     *      kernel64 heap        0x00400000 .. 0x01400000   (kernel64.cpp)
     *      32-bit heap          0x00500000 .. 0x01300000   (kernel.cpp)
     *      32-bit RAM-SFS       0x01400000 .. 0x01800000   (SFS_RAM_RESERVE)
     *
     *  Placing it right after .bss (0x453000) put the live PML4 inside
     *  kernel64's heap -- it booted only because the allocator had not
     *  yet handed out that far.  0x1800000 is the first address above
     *  every one of those windows.  It stays inside the 32-bit kernel's
     *  32 MiB PSE identity map and inside PMM's 256 MiB bitmap, so it
     *  needs ~24.2 MiB of RAM.
     *
     *  COUPLING: this must stay >= SFS_RAM_TARGET + SFS_RAM_RESERVE from
     *  kernel.cpp.  If either changes, move this to match.
     * ================================================================= */
    . = 0x1800000;
    __lmboot_start = .;

    /* 32-bit kernel boot stack (grows DOWN from __k32_stack_top) */
    __k32_stack_bottom = .;
    . = . + 0x10000;                    /* 64 KiB */
    __k32_stack_top = .;

    /* Long-mode page tables.  switch32to64.asm clears 4 pages starting at
     * __lm_pml4 in one `rep stosd`, so pml4/pdpt/pd/pd2 must stay
     * contiguous and in this order.  __lm_pdpt2 is cleared separately. */
    . = ALIGN(0x1000);
    __lm_pml4  = .;   . = . + 0x1000;
    __lm_pdpt  = .;   . = . + 0x1000;
    __lm_pd    = .;   . = . + 0x1000;
    __lm_pd2   = .;   . = . + 0x1000;
    __lm_pdpt2 = .;   . = . + 0x1000;
    __lm_pt_spare = .; . = . + 0x3000;  /* head-room: 8 pages total */

    /* 64-bit kernel boot stack (grows DOWN from __lm_stack_top).
     * Previously hard-coded 0x1F0000, which sits INSIDE kernel64's .bss
     * (0x19C000..0x3A2C60) -- the 64-bit stack and 64-bit globals were
     * silently sharing memory. */
    __lm_stack_bottom = .;
    . = . + 0x10000;                    /* 64 KiB */
    __lm_stack_top = .;

    __lmboot_end = .;

    /* Lower bound: must start at or above the top of the RAM-SFS window,
     * SFS_RAM_TARGET (0x1400000) + SFS_RAM_RESERVE (0x400000) in kernel.cpp.
     * Below that it lands in the 32-bit heap or the CD/UEFI RAM-SFS image. */
    ASSERT(__lmboot_start >= 0x1800000,
        "boot scratch region overlaps the RAM-SFS window (SFS_RAM_TARGET + SFS_RAM_RESERVE in kernel.cpp)")

    /* Upper bound: vmm_init() identity-maps only the first 32 MiB with 4 MiB
     * PSE pages.  Past that the 32-bit kernel cannot even reach its own page
     * tables once paging comes on. */
    ASSERT(__lmboot_end <= 0x2000000,
        "boot scratch region runs past the 32 MiB PSE identity map in vmm_init() (kernel.cpp) -- extend that map first")

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.note.*)
        *(.gnu.hash)
    }
}
