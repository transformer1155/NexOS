/* aiinfer.ld - link script for the 32-bit AI inference module.
 * The module is a flat binary loaded at a fixed address by the kernel,
 * so every section is anchored at AIINFER_BASE and nothing may be
 * position-dependent.  .modhdr (the export table) must be the first
 * thing in the image so the kernel can find it at offset 0. */
OUTPUT_FORMAT("elf32-i386")
SECTIONS
{
  . = 0x18000000;

  .text : {
    KEEP(*(.modhdr))
    *(.text .text.*)
  }

  .rodata : { *(.rodata .rodata.*) }

  .data : { *(.data .data.* .sdata .sdata.*) }

  .bss : {
    __bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __bss_end = .;
  }

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) *(.eh_frame_hdr) *(.got) *(.got.plt) }
}
