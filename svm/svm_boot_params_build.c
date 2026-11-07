// =====================================================================
//  svm_boot_params_build.c  -  fill the Linux boot zeropage
// ---------------------------------------------------------------------
//  Pure memory construction.  No VM instruction, no jump to the kernel.
//
//  ORDERING TRAP WORTH KNOWING
//    `hdr.ramdisk_image` and `hdr.cmd_line_ptr` are 32-BIT fields in the
//    classic protocol.  Modern kernels provide ext_ramdisk_image /
//    ext_cmd_line_ptr for above-4G addresses.  Our guest RAM is far below
//    4 GiB, so the 32-bit fields suffice -- but the caller must not place
//    the initrd or cmdline above 4 GiB, and bp_build() refuses if it tries.
// =====================================================================
#include "svm_boot_params_build.h"

static void bp_zero(void* p, uint64_t n)
{
    uint8_t* d = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

// ---------------------------------------------------------------------
uint32_t bp_add_standard_regions(bp_region* out, uint32_t max_out,
                                 uint64_t ram_top)
{
    if (!out) return 0;
    uint32_t n = 0;

    // Conventional low RAM: 0 .. 0x9FBFF (640 KiB less the last 1 KiB).
    // Linux relies on this exact boundary; EBDA begins at 0x9FC00.
    if (n < max_out) { out[n].addr = 0x00000000ull; out[n].size = 0x09FC00ull; out[n].type = E820_RAM; n++; }

    // EBDA / BIOS shadow: 0x9FC00 .. 0x9FFFF
    if (n < max_out) { out[n].addr = 0x0009FC00ull; out[n].size = 0x00000400ull; out[n].type = E820_RESERVED; n++; }

    // VGA/ROM hole: 0xF0000 .. 0xFFFFF
    if (n < max_out) { out[n].addr = 0x000F0000ull; out[n].size = 0x00010000ull; out[n].type = E820_RESERVED; n++; }

    // Main RAM from 1 MiB up to ram_top.
    if (ram_top > 0x00100000ull && n < max_out) {
        out[n].addr = 0x00100000ull;
        out[n].size = ram_top - 0x00100000ull;
        out[n].type = E820_RAM;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------
int bp_build(struct svm_boot_params* bp, const bpbuild_params* p)
{
    if (!bp || !p) return BPBUILD_ERR_NULL;

    if (p->region_count > E820_MAX_ENTRIES_ZEROPAGE)
        return BPBUILD_ERR_TOO_MANY;

    if (!p->regions && p->region_count > 0) return BPBUILD_ERR_NULL;

    // The classic 32-bit fields cannot address above 4 GiB.
    if (p->boot_params_paddr >= 0x100000000ull) return BPBUILD_ERR_ARG;
    if (p->initrd_size > 0 && p->initrd_paddr >= 0x100000000ull)
        return BPBUILD_ERR_ARG;
    // cmdline_paddr is a uint32_t by design (see the struct comment), so
    // there is nothing to validate here -- the type already forbids >=4 GiB.
    // A warning about an always-false comparison was the compiler telling
    // us exactly that.

    // ---- start from a clean page ----
    bp_zero(bp, sizeof(*bp));

    // ---- sentinel ----
    //  boot.rst: a bootloader that naively copies the whole region will
    //  leave this at 0xFF, telling the kernel "some fields are invalid,
    //  zero them".  Setting it explicitly is the honest signal that we
    //  built the page ourselves.
    bp->sentinel = 0xFF;

    // ---- E820 table ----
    //  Validate as we copy: a zero-size region or an end that wraps the
    //  64-bit space is a malformed map, and Linux will reject it.
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->region_count; i++) {
        uint64_t a = p->regions[i].addr;
        uint64_t s = p->regions[i].size;

        if (s == 0) return BPBUILD_ERR_OVERLAP;
        // end must not overflow 64 bits
        if (a + s < a) return BPBUILD_ERR_OVERLAP;

        bp->e820_table[n].addr = a;
        bp->e820_table[n].size = s;
        bp->e820_table[n].type = p->regions[i].type;
        n++;
    }
    bp->e820_entries = (uint8_t)n;

    // ---- setup_header ----
    struct setup_header* h = &bp->hdr;

    h->setup_sects = 0;                 // the caller relocates the kernel itself
    h->boot_flag   = 0xAA55;            // required marker
    h->header      = BP_HEADER_MAGIC_U32;
    h->version     = BP_PROTOCOL_VERSION;

    // type_of_loader 0xFF = "unknown bootloader".  There is no assigned
    // ID for NexOS, and inventing one would make the kernel print a
    // misleading loader name in dmesg.  0xFF is the documented option.
    h->type_of_loader = 0xFF;

    // Loaded high (above 1 MiB) and keep the segments we set up, because
    // our VMCB already loaded valid flat selectors.
    h->loadflags = (uint8_t)(BP_LOADED_HIGH | BP_KEEP_SEGMENTS);

    h->code32_start = (uint32_t)p->kernel_paddr;

    if (p->initrd_size > 0) {
        h->ramdisk_image = (uint32_t)p->initrd_paddr;
        h->ramdisk_size  = p->initrd_size;
    } else {
        h->ramdisk_image = 0;
        h->ramdisk_size  = 0;
    }

    // initrd_addr_max: the highest address the kernel may relocate the
    // initrd to.  0x37FFFFFF is the protocol's conventional cap just
    // below 1 GiB; giving the kernel this value is what a normal
    // bootloader does and avoids "initrd above initrd_addr_max" aborts.
    h->initrd_addr_max = 0x37FFFFFFu;

    // cmdline
    h->cmd_line_ptr  = p->cmdline_paddr;
    h->cmdline_size  = p->cmdline_max ? p->cmdline_max : BP_CMDLINE_SIZE;

    // kernel_alignment / relocatable: 0 means "not relocatable".  Since we
    // place the kernel ourselves at a fixed address and do NOT implement
    // relocation, being explicit here is important: claiming relocation
    // support without implementing it is a classic source of boot hangs.
    h->kernel_alignment   = 0;
    h->relocatable_kernel = 0;
    h->min_alignment      = 0;

    // init_size: how much memory the kernel's decompressor wants for its
    // own temporary buffer.  Zero means "unknown"; passing the caller's
    // figure is the safe choice.
    h->init_size = p->init_size;

    return BPBUILD_OK;
}

const char* bpbuild_str(int rc)
{
    switch (rc) {
        case BPBUILD_OK:           return "ok";
        case BPBUILD_ERR_NULL:     return "null zeropage or params pointer";
        case BPBUILD_ERR_TOO_MANY: return "more E820 regions than the zeropage holds";
        case BPBUILD_ERR_OVERLAP:  return "malformed E820 region (zero size or overflow)";
        case BPBUILD_ERR_ARG:      return "initrd/cmdline/zeropage above 4 GiB";
        default:                   return "unrecognised result";
    }
}