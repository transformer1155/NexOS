// =====================================================================
//  svm_boot_params_build.h  -  construct the Linux boot zeropage
// ---------------------------------------------------------------------
//  NO EXECUTABLE VM LOGIC: this fills ordinary memory.  Nothing here
//  jumps to the kernel; the caller decides when (and whether) to start it.
// =====================================================================
#ifndef NEXOS_SVM_BOOT_PARAMS_BUILD_H
#define NEXOS_SVM_BOOT_PARAMS_BUILD_H

#include <stdint.h>
#include "svm_boot_params.h"

typedef enum {
    BPBUILD_OK            =  0,
    BPBUILD_ERR_NULL      = -1,
    BPBUILD_ERR_TOO_MANY  = -2,   // more E820 entries than the zeropage holds
    BPBUILD_ERR_OVERLAP   = -3,   // an E820 region is malformed / overflows
    BPBUILD_ERR_ARG       = -4,   // bad initrd / cmdline argument
} bpbuild_result;

// ---------------------------------------------------------------------
//  A memory region to describe to the guest, in GUEST-PHYSICAL terms.
// ---------------------------------------------------------------------
typedef struct {
    uint64_t addr;
    uint64_t size;
    uint32_t type;              // E820_RAM / E820_RESERVED / ...
} bp_region;

// ---------------------------------------------------------------------
//  Everything the zeropage needs.
// ---------------------------------------------------------------------
typedef struct {
    const bp_region* regions;   // array of regions to publish
    uint32_t         region_count;

    // Kernel load parameters
    uint64_t kernel_paddr;      // where the protected-mode kernel is loaded
    uint32_t kernel_size;       // bytes of kernel payload
    uint32_t init_size;         // boot protocol: kernel's preferred alloc size

    // initrd (0 / 0 if none)
    uint64_t initrd_paddr;
    uint32_t initrd_size;

    // command line: NUL-terminated string, placed by the caller, addressed
    // through cmd_line_ptr.
    // NOTE: this is uint32_t ON PURPOSE.  hdr.cmd_line_ptr is a 32-bit
    // protocol field, so a command line above 4 GiB simply cannot be
    // expressed in the classic header (modern kernels need
    // ext_cmd_line_ptr, which we do not use).  Making it 64-bit here
    // would invite a caller to pass an address we could never encode.
    uint32_t cmdline_paddr;
    uint32_t cmdline_max;

    // where this zeropage itself lives (must be < 4 GiB for the 32-bit
    // cmd_line_ptr / ramdisk_image fields to address it)
    uint64_t boot_params_paddr;
} bpbuild_params;

// Fill `bp` (a zeropage in guest memory) from `p`.
int bp_build(struct svm_boot_params* bp, const bpbuild_params* p);

const char* bpbuild_str(int rc);

// Convenience: append the conventional "low memory + high RAM + reserved"
// layout the kernel expects for a simple guest.  Returns entries added.
//   * 0x000000-0x09FBFF  usable RAM   (conventional, 640 KiB boundary)
//   * 0x09FC00-0x09FFFF  reserved     (EBDA / BIOS shadow)
//   * 0x0F0000-0x0FFFFF  reserved     (BIOS ROM area)
//   * 0x100000-<top>     usable RAM
uint32_t bp_add_standard_regions(bp_region* out, uint32_t max_out,
                                 uint64_t ram_top);

#endif // NEXOS_SVM_BOOT_PARAMS_BUILD_H
