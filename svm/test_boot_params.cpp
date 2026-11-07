// =====================================================================
//  test_boot_params.cpp  -  verify the zeropage against the boot protocol
// ---------------------------------------------------------------------
//  Cross-checks two things at once:
//
//    1. every field lands at the offset boot.rst / bootparam.h requires;
//    2. the E820 map we generate matches the memory map an actual Linux
//       kernel printed when it booted under QEMU (captured in phase 0):
//
//         BIOS-e820: [mem 0x0000000000000000-0x000000000009fbff] usable
//         BIOS-e820: [mem 0x000000000009fc00-0x000000000009ffff] reserved
//         BIOS-e820: [mem 0x00000000000f0000-0x00000000000fffff] reserved
//         BIOS-e820: [mem 0x0000000000100000-...]            usable
//
//    If our builder reproduces that shape, the kernel will accept it.
// =====================================================================
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "svm/svm_boot_params.h"

extern "C" {
#include "svm/svm_boot_params_build.h"
}

static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) { printf("FAIL: %s\n", msg); g_fail++; } \
        else { printf("ok  : %s\n", msg); }                  \
    } while (0)

static const char* e820_name(uint32_t t)
{
    switch (t) {
        case E820_RAM:      return "usable";
        case E820_RESERVED: return "reserved";
        case E820_ACPI:     return "ACPI";
        case E820_NVS:      return "NVS";
        case E820_UNUSABLE: return "unusable";
        default:            return "?";
    }
}

int main(void)
{
    printf("=== Linux boot zeropage verification ===\n\n");

    static struct svm_boot_params bp;

    // ---- Build an E820 map for a 128 MiB guest ----
    const uint64_t RAM_TOP = 128ull * 1024 * 1024;
    bp_region regions[8];
    uint32_t nr = bp_add_standard_regions(regions, 8, RAM_TOP);

    printf("[E820 regions generated for %llu MB guest]\n",
           (unsigned long long)(RAM_TOP / (1024 * 1024)));
    for (uint32_t i = 0; i < nr; i++) {
        uint64_t end = regions[i].addr + regions[i].size - 1;
        printf("  [mem 0x%08llX-0x%08llX] %s\n",
               (unsigned long long)regions[i].addr,
               (unsigned long long)end,
               e820_name(regions[i].type));
    }
    printf("\n");

    CHECK(nr == 4, "four standard regions produced");

    // ---- Compare against the real kernel's printed map ----
    printf("[cross-check vs the kernel's own boot log]\n");
    CHECK(regions[0].addr == 0x00000000ull && regions[0].size == 0x0009FC00ull
          && regions[0].type == E820_RAM,
          "region 0: 0x0-0x9FBFF usable (matches BIOS-e820 log)");
    CHECK(regions[1].addr == 0x0009FC00ull && regions[1].size == 0x00000400ull
          && regions[1].type == E820_RESERVED,
          "region 1: 0x9FC00-0x9FFFF reserved (matches BIOS-e820 log)");
    CHECK(regions[2].addr == 0x000F0000ull && regions[2].size == 0x00010000ull
          && regions[2].type == E820_RESERVED,
          "region 2: 0xF0000-0xFFFFF reserved (matches BIOS-e820 log)");
    CHECK(regions[3].addr == 0x00100000ull && regions[3].size == RAM_TOP - 0x100000ull
          && regions[3].type == E820_RAM,
          "region 3: 1MB-128MB usable (matches BIOS-e820 log)");

    // ---- Build the zeropage ----
    bpbuild_params p;
    memset(&p, 0x00, sizeof(p));
    p.regions           = regions;
    p.region_count      = nr;
    p.kernel_paddr      = 0x0000000000100000ull;
    p.kernel_size       = 12u * 1024 * 1024;      // ~12 MB bzImage decompressed
    p.init_size         = 32u * 1024 * 1024;
    p.initrd_paddr      = 0x0000000004000000ull;  // 64 MiB
    p.initrd_size       = 3u * 1024 * 1024;       // our cpio.gz is ~3.5 MB
    p.cmdline_paddr     = 0x0000000000010000ull;  // low memory, safe
    p.cmdline_max       = BP_CMDLINE_SIZE;
    p.boot_params_paddr = 0x0000000000001000ull;

    int rc = bp_build(&bp, &p);
    printf("[build]\n  result = %d (%s)\n\n", rc, bpbuild_str(rc));
    CHECK(rc == BPBUILD_OK, "bp_build succeeded");

    // ---- Layout: every field at its protocol offset ----
    printf("[zeropage layout]\n");
    printf("  sizeof(zeropage)  = %u\n", (unsigned)sizeof(bp));
    printf("  e820_entries @0x%03X = %u\n",
           (unsigned)offsetof(struct svm_boot_params, e820_entries), bp.e820_entries);
    printf("  hdr          @0x%03X\n", (unsigned)offsetof(struct svm_boot_params, hdr));
    printf("  e820_table   @0x%03X\n", (unsigned)offsetof(struct svm_boot_params, e820_table));
    printf("  sizeof(setup_header) = %u\n\n", (unsigned)sizeof(struct setup_header));

    CHECK(sizeof(bp) == 4096, "zeropage is exactly one page");
    CHECK(offsetof(struct svm_boot_params, e820_entries) == 0x1E8, "e820_entries @0x1E8");
    CHECK(offsetof(struct svm_boot_params, hdr) == 0x1F1, "setup_header @0x1F1");
    CHECK(offsetof(struct svm_boot_params, e820_table) == 0x2D0, "e820_table @0x2D0");
    CHECK(sizeof(struct setup_header) == 123, "setup_header is 123 bytes");

    // ---- Header contents ----
    printf("[setup_header]\n");
    printf("  header         = 0x%08X (must be 0x%08X)\n", bp.hdr.header, BP_HEADER_MAGIC_U32);
    printf("  version        = 0x%04X\n", bp.hdr.version);
    printf("  boot_flag      = 0x%04X\n", bp.hdr.boot_flag);
    printf("  type_of_loader = 0x%02X\n", bp.hdr.type_of_loader);
    printf("  loadflags      = 0x%02X\n", bp.hdr.loadflags);
    printf("  code32_start   = 0x%08X\n", bp.hdr.code32_start);
    printf("  ramdisk_image  = 0x%08X\n", bp.hdr.ramdisk_image);
    printf("  ramdisk_size   = 0x%08X (%u MB)\n", bp.hdr.ramdisk_size, bp.hdr.ramdisk_size/(1024*1024));
    printf("  cmd_line_ptr   = 0x%08X\n", bp.hdr.cmd_line_ptr);
    printf("  initrd_addr_max= 0x%08X\n", bp.hdr.initrd_addr_max);
    printf("  relocatable    = %u\n", bp.hdr.relocatable_kernel);
    printf("  init_size      = 0x%08X (%u MB)\n\n", bp.hdr.init_size, bp.hdr.init_size/(1024*1024));

    CHECK(bp.hdr.header == BP_HEADER_MAGIC_U32, "'HdrS' magic present");
    CHECK(bp.hdr.boot_flag == 0xAA55, "boot_flag is 0xAA55");
    CHECK(bp.hdr.version >= 0x0200, "protocol version >= 2.00");
    CHECK((bp.hdr.loadflags & BP_LOADED_HIGH) != 0, "LOADED_HIGH set (kernel above 1MB)");
    CHECK((bp.hdr.loadflags & BP_KEEP_SEGMENTS) != 0, "KEEP_SEGMENTS set (flat selectors valid)");
    CHECK(bp.hdr.type_of_loader == 0xFF, "type_of_loader 0xFF (unknown, documented)");
    CHECK(bp.hdr.code32_start == 0x100000, "code32_start = kernel load address");
    CHECK(bp.hdr.ramdisk_image == 0x04000000, "ramdisk_image = initrd address");
    CHECK(bp.hdr.ramdisk_size == 3u*1024*1024, "ramdisk_size = initrd size");
    CHECK(bp.hdr.cmd_line_ptr == 0x10000, "cmd_line_ptr = command line address");
    CHECK(bp.hdr.initrd_addr_max == 0x37FFFFFF, "initrd_addr_max = protocol cap");
    CHECK(bp.hdr.relocatable_kernel == 0,
          "relocatable_kernel 0 (we do NOT implement relocation)");

    // ---- sentinel ----
    CHECK(bp.sentinel == 0xFF, "sentinel is 0xFF (declares page as bootloader-built)");

    // ---- E820 table as stored ----
    printf("[e820_table[] as the kernel will read it]\n");
    for (uint32_t i = 0; i < bp.e820_entries; i++) {
        printf("  [%u] 0x%08llX len 0x%08llX type %u (%s)\n", i,
               (unsigned long long)bp.e820_table[i].addr,
               (unsigned long long)bp.e820_table[i].size,
               bp.e820_table[i].type, e820_name(bp.e820_table[i].type));
    }
    printf("\n");
    CHECK(bp.e820_entries == 4, "e820_entries == 4");
    CHECK(bp.e820_table[0].type == E820_RAM, "entry 0 is usable RAM");
    CHECK(bp.e820_table[3].type == E820_RAM, "entry 3 is usable RAM (high memory)");
    CHECK(bp.e820_table[3].addr + bp.e820_table[3].size == RAM_TOP,
          "high RAM ends exactly at the guest's RAM top");

    // ---- fail-closed paths ----
    printf("[fail-closed]\n");
    {
        bpbuild_params bad = p;
        bad.region_count = E820_MAX_ENTRIES_ZEROPAGE + 1;
        CHECK(bp_build(&bp, &bad) == BPBUILD_ERR_TOO_MANY, "129 regions rejected");
    }
    {
        bp_region zero = { 0, 0, E820_RAM };
        bpbuild_params bad = p;
        bad.regions = &zero; bad.region_count = 1;
        CHECK(bp_build(&bp, &bad) == BPBUILD_ERR_OVERLAP, "zero-size region rejected");
    }
    {
        bp_region wrap = { 0xFFFFFFFFFFFFF000ull, 0x0000000000002000ull, E820_RAM };
        bpbuild_params bad = p;
        bad.regions = &wrap; bad.region_count = 1;
        CHECK(bp_build(&bp, &bad) == BPBUILD_ERR_OVERLAP, "64-bit overflow region rejected");
    }
    {
        bpbuild_params bad = p;
        bad.initrd_paddr = 0x100000000ull;
        bad.initrd_size  = 1;
        CHECK(bp_build(&bp, &bad) == BPBUILD_ERR_ARG, "initrd above 4 GiB rejected");
    }
    CHECK(bp_build(NULL, &p) == BPBUILD_ERR_NULL, "NULL zeropage rejected");
    CHECK(bp_build(&bp, NULL) == BPBUILD_ERR_NULL, "NULL params rejected");

    // ---- defensiveness: a caller reading uninitialised fields ----
    printf("\n[no-initrd path]\n");
    {
        bpbuild_params q = p;
        q.initrd_paddr = 0; q.initrd_size = 0;
        struct svm_boot_params bp2;
        memset(&bp2, 0xCD, sizeof(bp2));      // poison
        CHECK(bp_build(&bp2, &q) == BPBUILD_OK, "build with no initrd succeeds");
        CHECK(bp2.hdr.ramdisk_image == 0, "ramdisk_image zeroed when no initrd");
        CHECK(bp2.hdr.ramdisk_size == 0, "ramdisk_size zeroed when no initrd");
    }

    printf("\n=== %s ===\n", g_fail == 0 ? "ALL PASS" : "TEST FAILED");
    return g_fail == 0 ? 0 : 1;
}