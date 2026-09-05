/* =====================================================================
 *  get_embedded.S  -  RIP-relative access to embedded kernel binary
 * ---------------------------------------------------------------------
 *  The kernel.bin is embedded into the UEFI PE32+ image via the linker
 *  (ld -r -b binary).  However, the PE32+ .reloc section is empty,
 *  so the UEFI firmware cannot apply base relocations to the
 *  _binary_build_* symbols.  Using LEA with RIP-relative addressing
 *  avoids the GOT entirely and works correctly regardless of the
 *  image's load address.
 *
 *  These functions are called from bootuefi.c to get the source
 *  pointer for CopyMem when copying the kernel to 0x10000.
 * ===================================================================== */

    .text
    .code64

    .global get_embedded_kernel_start
    .global get_embedded_kernel_end

get_embedded_kernel_start:
    lea    _binary_build_kernel_blob_start(%rip), %rax
    ret

get_embedded_kernel_end:
    lea    _binary_build_kernel_blob_end(%rip), %rax
    ret