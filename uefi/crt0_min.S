/* =====================================================================
 *  crt0_min.S  -  Minimal UEFI entry stub (gnu-efi-free)
 * ---------------------------------------------------------------------
 *  UEFI jumps to the PE entry point with (RCX=ImageHandle,
 *  RDX=SystemTable) using the Microsoft x64 calling convention.
 *  Because the loader objects are compiled with -mabi=ms, efi_main
 *  already uses that convention, so this stub just transfers control.
 * ===================================================================== */
    .text
    .code64
    .global _start
_start:
    jmp efi_main
