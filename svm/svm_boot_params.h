// =====================================================================
//  svm_boot_params.h  -  x86 Linux boot protocol (boot_params / zero page)
// ---------------------------------------------------------------------
//  AUTHORITATIVE SOURCES
//    Linux arch/x86/include/asm/bootparam.h      (struct boot_params)
//    Linux arch/x86/include/asm/setup_data.h     (struct boot_e820_entry)
//    Linux arch/x86/include/asm/e820.h           (E820 type codes)
//    Linux Documentation/x86/boot.rst            (the protocol itself)
//
//  WHAT THIS IS FOR
//    A Linux bzImage cannot be started by jumping to it.  The bootloader
//    must place a "zero page" (struct boot_params) at a known location,
//    point the kernel's 16-bit setup code at it, and describe memory via
//    an E820 table.  For our SVM guest we construct that page ourselves.
//
//  Layout strategy -- and why the padding fields exist
//    struct boot_params is a large ABI struct whose early members
//    (screen_info, apm_bios_info, edd, edid, ...) we deliberately leave
//    zeroed: this guest has no BIOS, no APM, no EDD, no EDID.  Rather
//    than pull in half of the kernel's video and BIOS headers just to
//    leave them empty, we occupy those ranges with explicit padding arrays
//    and then place the members we DO need at their exact protocol
//    offsets.  Every one of those offsets is pinned by a static_assert
//    below, so the padding cannot silently drift.
//
//  NO EXECUTABLE LOGIC
//    Same project rule as the rest of svm/: pure layout and constants.
// =====================================================================
#ifndef NEXOS_SVM_BOOT_PARAMS_H
#define NEXOS_SVM_BOOT_PARAMS_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#  define BP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define BP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define BP_PACKED __attribute__((packed))

// ---------------------------------------------------------------------
//  Protocol constants
// ---------------------------------------------------------------------
#define BP_HEADER_MAGIC        "HdrS"      // 0x53726448, marks a valid setup_header
#define BP_HEADER_MAGIC_U32    0x53726448u
#define BP_PROTOCOL_VERSION    0x020Eu     // 2.14 is safely modern
#define BP_LOADED_HIGH         (1u << 0)   // kernel loaded above 0x10000
#define BP_KEEP_SEGMENTS       (1u << 6)   // don't reload segments
#define BP_CAN_USE_HEAP        (1u << 7)
#define BP_KASLR_FLAG          (1u << 1)

#define BP_CMDLINE_SIZE        2048u       // max command line the kernel expects

// Legacy entry setup-code offsets (real mode), documented in boot.rst:
#define BP_SETUP_SECTS_OFFSET      0x1F1u
#define BP_HDR_OFFSET              0x1F1u   // setup_header begins here
#define BP_E820_ENTRIES_OFFSET     0x1E8u
#define BP_E820_TABLE_OFFSET       0x2D0u
#define BP_SENTINEL_OFFSET         0x1EFu
#define BP_ZEROPAGE_SIZE           0x1000u  // one page

// ---------------------------------------------------------------------
//  E820 memory types (e820.h)
// ---------------------------------------------------------------------
#define E820_RAM        1u
#define E820_RESERVED   2u
#define E820_ACPI       3u
#define E820_NVS        4u
#define E820_UNUSABLE   5u
#define E820_PMEM       7u

#define E820_MAX_ENTRIES_ZEROPAGE  128u

// ---------------------------------------------------------------------
//  One E820 entry, exactly as the boot protocol ABI defines it (20 bytes)
// ---------------------------------------------------------------------
struct boot_e820_entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
} BP_PACKED;

BP_STATIC_ASSERT(sizeof(struct boot_e820_entry) == 20,
                 "boot_e820_entry must be 20 bytes (8+8+4, packed)");

// ---------------------------------------------------------------------
//  setup_header (boot.rst "THE REAL-MODE KERNEL HEADER")
// ---------------------------------------------------------------------
//  Field order and widths transcribed verbatim from bootparam.h.
struct setup_header {
    uint8_t  setup_sects;        // +0
    uint16_t root_flags;         // +1
    uint32_t syssize;            // +3
    uint16_t ram_size;           // +7
    uint16_t vid_mode;           // +9
    uint16_t root_dev;           // +11
    uint16_t boot_flag;          // +13  (must be 0xAA55 at file offset 0x1FE)
    uint16_t jump;               // +15
    uint32_t header;             // +17  ("HdrS")
    uint16_t version;            // +21
    uint32_t realmode_swtch;     // +23
    uint16_t start_sys_seg;      // +27
    uint16_t kernel_version;     // +29
    uint8_t  type_of_loader;     // +31
    uint8_t  loadflags;          // +32
    uint16_t setup_move_size;    // +33
    uint32_t code32_start;       // +35
    uint32_t ramdisk_image;      // +39
    uint32_t ramdisk_size;       // +43
    uint32_t bootsect_kludge;    // +47
    uint16_t heap_end_ptr;       // +51
    uint8_t  ext_loader_ver;     // +53
    uint8_t  ext_loader_type;    // +54
    uint32_t cmd_line_ptr;       // +55
    uint32_t initrd_addr_max;    // +59
    uint32_t kernel_alignment;   // +63
    uint8_t  relocatable_kernel; // +67
    uint8_t  min_alignment;      // +68
    uint16_t xloadflags;         // +69
    uint32_t cmdline_size;       // +71
    uint32_t hardware_subarch;   // +75
    uint64_t hardware_subarch_data; // +79
    uint32_t payload_offset;     // +87
    uint32_t payload_length;     // +91
    uint64_t setup_data;         // +95
    uint64_t pref_address;       // +103
    uint32_t init_size;          // +111
    uint32_t handover_offset;    // +115
    uint32_t kernel_info_offset; // +119
} BP_PACKED;

// Total: 119 + 4 = 123 bytes.  This number is load-bearing: the zeropage
// padding below is calculated from it.
BP_STATIC_ASSERT(sizeof(struct setup_header) == 123u,
                 "setup_header must be 123 bytes per the boot protocol");

// Individual offsets that a bootloader MUST get right.
BP_STATIC_ASSERT(offsetof(struct setup_header, header)         == 17u,  "hdr.header @17");
BP_STATIC_ASSERT(offsetof(struct setup_header, loadflags)      == 32u,  "hdr.loadflags @32");
BP_STATIC_ASSERT(offsetof(struct setup_header, code32_start)   == 35u,  "hdr.code32_start @35");
BP_STATIC_ASSERT(offsetof(struct setup_header, ramdisk_image)  == 39u,  "hdr.ramdisk_image @39");
BP_STATIC_ASSERT(offsetof(struct setup_header, ramdisk_size)   == 43u,  "hdr.ramdisk_size @43");
BP_STATIC_ASSERT(offsetof(struct setup_header, cmd_line_ptr)   == 55u,  "hdr.cmd_line_ptr @55");
BP_STATIC_ASSERT(offsetof(struct setup_header, initrd_addr_max)== 59u,  "hdr.initrd_addr_max @59");
BP_STATIC_ASSERT(offsetof(struct setup_header, kernel_alignment)==63u,  "hdr.kernel_alignment @63");
BP_STATIC_ASSERT(offsetof(struct setup_header, xloadflags)     == 69u,  "hdr.xloadflags @69");
BP_STATIC_ASSERT(offsetof(struct setup_header, cmdline_size)   == 71u,  "hdr.cmdline_size @71");
BP_STATIC_ASSERT(offsetof(struct setup_header, init_size)      == 111u, "hdr.init_size @111");

// ---------------------------------------------------------------------
//  The zero page (struct boot_params)
// ---------------------------------------------------------------------
//  Only the tail of the structure matters to us; everything before 0x1E8
//  is BIOS-provided hardware description that this guest does not have.
//  The padding arrays make that explicit rather than accidental.
struct svm_boot_params {
    uint8_t  pad_to_e820_entries[BP_E820_ENTRIES_OFFSET]; // 0x000..0x1E7

    uint8_t  e820_entries;        // 0x1E8  count of valid E820 entries
    uint8_t  eddbuf_entries;      // 0x1E9
    uint8_t  edd_mbr_sig_entries; // 0x1EA
    uint8_t  kbd_status;          // 0x1EB
    uint8_t  secure_boot;         // 0x1EC
    uint8_t  pad5[2];             // 0x1ED..0x1EE
    uint8_t  sentinel;            // 0x1EF  must be 0xFF (see below)
    uint8_t  pad6[1];             // 0x1F0

    struct setup_header hdr;      // 0x1F1

    // pad7 fills from end-of-hdr to the edd signature buffer at 0x290.
    uint8_t  pad7[0x290u - BP_HDR_OFFSET - sizeof(struct setup_header)];

    uint32_t edd_mbr_sig_buffer[16];   // 0x290 (64 bytes)

    struct boot_e820_entry e820_table[E820_MAX_ENTRIES_ZEROPAGE]; // 0x2D0

    uint8_t  tail[BP_ZEROPAGE_SIZE - BP_E820_TABLE_OFFSET
                  - sizeof(struct boot_e820_entry) * E820_MAX_ENTRIES_ZEROPAGE];
} BP_PACKED;

// =====================================================================
//  COMPILE-TIME VERIFICATION of the zeropage layout
// =====================================================================
BP_STATIC_ASSERT(sizeof(struct svm_boot_params) == BP_ZEROPAGE_SIZE,
                 "zeropage must be exactly 4096 bytes");

BP_STATIC_ASSERT(offsetof(struct svm_boot_params, e820_entries) == 0x1E8u,
                 "e820_entries must be at 0x1E8");
BP_STATIC_ASSERT(offsetof(struct svm_boot_params, sentinel)     == 0x1EFu,
                 "sentinel must be at 0x1EF");
BP_STATIC_ASSERT(offsetof(struct svm_boot_params, hdr)          == 0x1F1u,
                 "setup_header must begin at 0x1F1");
BP_STATIC_ASSERT(offsetof(struct svm_boot_params, edd_mbr_sig_buffer) == 0x290u,
                 "edd signature buffer must be at 0x290");
BP_STATIC_ASSERT(offsetof(struct svm_boot_params, e820_table)   == 0x2D0u,
                 "e820_table must begin at 0x2D0");

// The E820 table must fit inside the page.
BP_STATIC_ASSERT(0x2D0u + sizeof(struct boot_e820_entry) * E820_MAX_ENTRIES_ZEROPAGE
                 <= BP_ZEROPAGE_SIZE,
                 "128 E820 entries must fit in the zeropage");

#endif // NEXOS_SVM_BOOT_PARAMS_H
