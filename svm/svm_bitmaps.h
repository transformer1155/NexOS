// =====================================================================
//  svm_bitmaps.h  -  IOPM and MSRPM intercept bitmaps (static layout)
// ---------------------------------------------------------------------
//  These two bitmaps tell the CPU which guest accesses must cause a
//  #VMEXIT:
//
//    IOPM  (I/O Permission Map)   1 bit  per I/O port   (65536 ports)
//    MSRPM (MSR Permission Map)   2 bits per MSR number (0..0xFFFFF)
//
//  SIZES (APM2):
//    IOPM  = 65536 bits =  8 KiB = 2 pages
//    MSRPM = (0x1000 + 0x1000) MSR zones * 2 bits
//            -> 12 KiB = 3 pages (see note below)
//
//  *** IMPORTANT CORRECTION TO THE LEGACY DRIVER ***
//    drivers/svm_driver.c defines:
//        SVM_IOPM_PAGES  4   (-> 16 KiB)
//        SVM_MSRPM_PAGES 2   (->  8 KiB)
//    and comments that these match "plugins/vmm.cpp L368-374".  They do
//    not match APM2.  The canonical sizes are 8 KiB for IOPM and 12 KiB
//    for MSRPM.  Both MUST be 4 KiB-page aligned, and both bases go into
//    the VMCB as PHYSICAL addresses.
//
//  MSRPM structure, per APM2:
//    The MSR space is split into 6 zones of 0x1000 registers each for
//    three ranges (0000_0000-0000_1FFF, C000_0000-C000_1FFF,
//    C001_0000-C001_1FFF).  Each range uses 2 zones: one for reads and
//    one for writes, each zone being 0x1000 registers * 2 bits.
//    Total = 3 ranges * 2 (read/write) * 0x1000 * 2 bits = 12 KiB.
//
//  BIT ORDER for MSRPM: 2 bits per MSR, and the FIRST MSR in a zone
//  occupies the LOW bits of the first byte.  Bit order is
//  little-endian within each byte (APM2 Appendix C).
//
//  NO EXECUTABLE LOGIC
//    Pure layout + constants + compile-time checks, per the project
//    constraint.  Nothing here programs the CPU.
// =====================================================================
#ifndef NEXOS_SVM_BITMAPS_H
#define NEXOS_SVM_BITMAPS_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#  define BM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define BM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define BM_PACKED __attribute__((packed))

#define BM_PAGE_SIZE   4096u

// ---------------------------------------------------------------------
//  IOPM geometry
// ---------------------------------------------------------------------
#define IOPM_PORT_COUNT      65536u          // 16-bit port space
#define IOPM_BITS_PER_PORT   1u
#define IOPM_BYTES           (IOPM_PORT_COUNT / 8u)   // 8192
#define IOPM_PAGES           (IOPM_BYTES / BM_PAGE_SIZE)  // 2

// ---------------------------------------------------------------------
//  MSRPM geometry
// ---------------------------------------------------------------------
//  Each "zone" covers 0x1000 MSR numbers, 2 bits each => 1024 bytes.
#define MSRPM_ZONE_MSRS      0x1000u
#define MSRPM_BITS_PER_MSR   2u
#define MSRPM_ZONE_BYTES     (MSRPM_ZONE_MSRS * MSRPM_BITS_PER_MSR / 8u)  // 1024
#define MSRPM_ZONES          12u             // 3 address ranges * 2 (R/W)
#define MSRPM_BYTES          (MSRPM_ZONE_BYTES * MSRPM_ZONES)            // 12288
#define MSRPM_PAGES          (MSRPM_BYTES / BM_PAGE_SIZE)                // 3

// The three MSR address ranges covered, each split into read + write zones.
#define MSRPM_RANGE0_BASE    0x00000000u      // 0000_0000 - 0000_1FFF
#define MSRPM_RANGE1_BASE    0xC0000000u      // C000_0000 - C000_1FFF
#define MSRPM_RANGE2_BASE    0xC0010000u      // C001_0000 - C001_1FFF

// ---------------------------------------------------------------------
//  MSRPM bit encoding: 2 bits per MSR
//    bits (0,1) at that position select read/write intercept:
//      00 = allow  read, allow  write
//      01 = allow  read, intercept write
//      10 = intercept read, allow  write
//      11 = intercept both
// ---------------------------------------------------------------------
#define MSRPM_READ_ALLOW_WRITE_ALLOW  0x0u
#define MSRPM_READ_ALLOW_WRITE_INT    0x1u
#define MSRPM_READ_INT_WRITE_ALLOW    0x2u
#define MSRPM_READ_INT_WRITE_INT      0x3u

// ---------------------------------------------------------------------
//  Backing storage types.  These are the memory that the VMCB's
//  iopm_base_pa / msrpm_base_pa point at.
// ---------------------------------------------------------------------
struct svm_iopm {
    uint8_t bytes[IOPM_BYTES];
} BM_PACKED;

struct svm_msrpm {
    uint8_t bytes[MSRPM_BYTES];
} BM_PACKED;

// =====================================================================
//  COMPILE-TIME VERIFICATION
// =====================================================================
BM_STATIC_ASSERT(IOPM_BYTES == 8192u,  "IOPM must be 8 KiB (65536 bits)");
BM_STATIC_ASSERT(IOPM_PAGES == 2u,     "IOPM must occupy 2 pages");
BM_STATIC_ASSERT(sizeof(struct svm_iopm) == 8192u, "iopm storage must be 8 KiB");

BM_STATIC_ASSERT(MSRPM_ZONE_BYTES == 1024u, "one MSRPM zone is 0x1000*2bits = 1 KiB");
BM_STATIC_ASSERT(MSRPM_BYTES == 12288u,     "MSRPM must be 12 KiB");
BM_STATIC_ASSERT(MSRPM_PAGES == 3u,         "MSRPM must occupy 3 pages");
BM_STATIC_ASSERT(sizeof(struct svm_msrpm) == 12288u, "msrpm storage must be 12 KiB");

// Both bases must be page-aligned; the VMCB stores physical addresses and
// the low 12 bits are reserved.
BM_STATIC_ASSERT((IOPM_BYTES % BM_PAGE_SIZE) == 0u,  "IOPM must be page-multiple");
BM_STATIC_ASSERT((MSRPM_BYTES % BM_PAGE_SIZE) == 0u, "MSRPM must be page-multiple");

// ---------------------------------------------------------------------
//  Port / MSR number -> bit position helpers (pure arithmetic)
// ---------------------------------------------------------------------
//  IOPM: 1 bit per port, port N lives at byte N/8, bit N%8 (LSB first).
#define IOPM_BYTE_FOR_PORT(port)   ((port) >> 3)
#define IOPM_BIT_FOR_PORT(port)    ((port) & 7u)

//  MSRPM: 2 bits per MSR within its zone; zone chosen by which range the
//  MSR falls into and whether the access is a read or a write.
#define MSRPM_BYTE_FOR_OFFSET(off) ((off) >> 2)   // 4 MSRs per byte
#define MSRPM_SHIFT_FOR_OFFSET(off)(((off) & 3u) * 2u)

#endif // NEXOS_SVM_BITMAPS_H
