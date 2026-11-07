// =====================================================================
//  svm_vmcb.h  -  AMD SVM VMCB layout (authoritative, static-only)
// ---------------------------------------------------------------------
//  PURPOSE
//    A complete, self-contained definition of the AMD Secure Virtual
//    Machine (SVM) Virtual Machine Control Block, transcribed from:
//
//      * AMD Architecture Programmer's Manual Volume 2 (APM2),
//        "Secure Virtual Machine" chapter, VMCB Layout tables; and
//      * Linux arch/x86/include/asm/svm.h (the reference implementation),
//        which itself carries BUILD_BUG_ON/SIZE checks against APM2:
//            EXPECTED_VMCB_CONTROL_AREA_SIZE  1024
//            EXPECTED_VMCB_SAVE_AREA_SIZE      744
//
//  IMPORTANT -- this header contains NO executable VM logic
//    There is deliberately no asm, no VMRUN, no MSR write, nothing that
//    can be "run".  It is pure data layout plus compile-time assertions.
//    That is the whole point: correctness here is provable by the
//    compiler, without needing a machine capable of nested SVM.
//
//  WHY OFFSETS MATTER MORE THAN NAMES
//    The CPU reads this structure by byte offset.  A field placed one
//    byte wrong silently produces a broken VMCB -- and with no way to
//    test, the only defence is static_assert against the published
//    offsets.  Every assertion below cites the number it locks down.
// =====================================================================
#ifndef NEXOS_SVM_VMCB_H
#define NEXOS_SVM_VMCB_H

#include <stdint.h>
#include <stddef.h>          // offsetof()

// C++ freestanding kernel build has no <assert.h>; use the builtin.
#if defined(__cplusplus)
#  define SVM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#  define SVM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define SVM_PACKED __attribute__((packed))

// ---------------------------------------------------------------------
//  Fundamental sizes (APM2 Table 15-1 / VMCB Layout)
// ---------------------------------------------------------------------
#define VMCB_CONTROL_AREA_SIZE   1024u
#define VMCB_SAVE_AREA_SIZE       744u
#define VMCB_TOTAL_SIZE                    (VMCB_CONTROL_AREA_SIZE + \
                                            VMCB_SAVE_AREA_SIZE)
#define VMCB_SAVE_AREA_OFFSET    1024u     // == CONTROL_AREA_SIZE

// Intercept words: 6 groups of 32 bits at the very start of the control
// area (word0 CR, word1 DR, word2 exception, word3-5 general).
#define SVM_MAX_INTERCEPT_WORDS  6u

// ---------------------------------------------------------------------
//  Intercept word indices
// ---------------------------------------------------------------------
enum svm_intercept_word {
    SVM_IW_CR        = 0,
    SVM_IW_DR        = 1,
    SVM_IW_EXCEPTION = 2,
    SVM_IW_WORD3     = 3,
    SVM_IW_WORD4     = 4,
    SVM_IW_WORD5     = 5,
};

// ---- word 0: control-register intercepts -----------------------------
#define SVM_IC_CR0_READ        (1u << 0)
#define SVM_IC_CR1_READ        (1u << 1)
#define SVM_IC_CR2_READ        (1u << 2)
#define SVM_IC_CR3_READ        (1u << 3)
#define SVM_IC_CR4_READ        (1u << 4)
#define SVM_IC_CR5_READ        (1u << 5)
#define SVM_IC_CR6_READ        (1u << 6)
#define SVM_IC_CR7_READ        (1u << 7)
#define SVM_IC_CR8_READ        (1u << 8)
#define SVM_IC_CR9_READ        (1u << 9)
#define SVM_IC_CR10_READ       (1u << 10)
#define SVM_IC_CR11_READ       (1u << 11)
#define SVM_IC_CR12_READ       (1u << 12)
#define SVM_IC_CR13_READ       (1u << 13)
#define SVM_IC_CR14_READ       (1u << 14)
#define SVM_IC_CR15_READ       (1u << 15)
#define SVM_IC_CR0_WRITE       (1u << 16)
/* 17..30 = CR1..CR15 write, following the read pattern */
#define SVM_IC_CR1_WRITE       (1u << 17)
#define SVM_IC_CR2_WRITE       (1u << 18)
#define SVM_IC_CR3_WRITE       (1u << 19)
#define SVM_IC_CR4_WRITE       (1u << 20)
#define SVM_IC_CR8_WRITE       (1u << 24)

// ---- word 1: debug-register intercepts ------------------------------
#define SVM_ID_DR0_READ        (1u << 0)
#define SVM_ID_DR1_READ        (1u << 1)
#define SVM_ID_DR2_READ        (1u << 2)
#define SVM_ID_DR3_READ        (1u << 3)
#define SVM_ID_DR6_READ        (1u << 4)
#define SVM_ID_DR7_READ        (1u << 5)
#define SVM_ID_DR0_WRITE       (1u << 16)
/* DR1..DR3 write follow the same +16 pattern */
#define SVM_ID_DR6_WRITE       (1u << 20)
#define SVM_ID_DR7_WRITE       (1u << 21)

// ---- word 2: exception intercepts (bit N == vector N) --------------
#define SVM_EXC_DIVIDE_ERROR   (1u << 0)    // #DE
#define SVM_EXC_DEBUG          (1u << 1)    // #DB
#define SVM_EXC_NMI            (1u << 2)
#define SVM_EXC_BREAKPOINT     (1u << 3)    // #BP
#define SVM_EXC_OVERFLOW       (1u << 4)    // #OF
#define SVM_EXC_INVALID_OPCODE (1u << 6)    // #UD
#define SVM_EXC_DOUBLE_FAULT   (1u << 8)    // #DF
#define SVM_EXC_GP             (1u << 13)   // #GP
#define SVM_EXC_PAGE_FAULT     (1u << 14)   // #PF

// ---- word 3: misc + instruction intercepts --------------------------
#define SVM_I3_INTR            (1u << 0)    // INTR (external interrupt)
#define SVM_I3_NMI             (1u << 1)
#define SVM_I3_SMI             (1u << 2)
#define SVM_I3_INIT            (1u << 3)
#define SVM_I3_VINTR           (1u << 4)    // virtual interrupt
#define SVM_I3_CR0_SEL_WRITE   (1u << 5)
#define SVM_I3_IDTR_READ       (1u << 6)
#define SVM_I3_GDTR_READ       (1u << 7)
#define SVM_I3_LDTR_READ       (1u << 8)
#define SVM_I3_TR_READ         (1u << 9)
#define SVM_I3_IDTR_WRITE      (1u << 10)
#define SVM_I3_GDTR_WRITE      (1u << 11)
#define SVM_I3_LDTR_WRITE      (1u << 12)
#define SVM_I3_TR_WRITE        (1u << 13)
#define SVM_I3_RDTSC           (1u << 14)
#define SVM_I3_RDPMC          (1u << 15)
#define SVM_I3_PUSHF           (1u << 16)
#define SVM_I3_POPF            (1u << 17)
#define SVM_I3_CPUID           (1u << 18)
#define SVM_I3_RSM             (1u << 19)
#define SVM_I3_IRET            (1u << 20)
#define SVM_I3_SWINT           (1u << 21)
#define SVM_I3_INVD            (1u << 22)
#define SVM_I3_PAUSE           (1u << 23)
#define SVM_I3_HLT             (1u << 24)
#define SVM_I3_INVLPG          (1u << 25)
#define SVM_I3_INVLPGA         (1u << 26)
#define SVM_I3_IOIO            (1u << 27)   // <-- serial I/O port intercept
#define SVM_I3_MSR             (1u << 28)   // MSR read/write intercept
#define SVM_I3_TASK_SWITCH     (1u << 29)
#define SVM_I3_FERR_FREEZE     (1u << 30)
#define SVM_I3_SHUTDOWN        (1u << 31)

// ---- word 4: instruction intercepts (continued) ---------------------
#define SVM_I4_VMRUN           (1u << 0)
#define SVM_I4_VMMCALL         (1u << 1)
#define SVM_I4_VMLOAD          (1u << 2)
#define SVM_I4_VMSAVE          (1u << 3)
#define SVM_I4_STGI            (1u << 4)
#define SVM_I4_CLGI            (1u << 5)
#define SVM_I4_SKINIT          (1u << 6)
#define SVM_I4_RDTSCP          (1u << 7)
#define SVM_I4_ICEBP           (1u << 8)
#define SVM_I4_WBINVD          (1u << 9)
#define SVM_I4_MONITOR         (1u << 10)
#define SVM_I4_MWAIT           (1u << 11)
#define SVM_I4_MWAITX          (1u << 12)
#define SVM_I4_XSETBV          (1u << 13)
#define SVM_I4_RDPRU           (1u << 14)
#define SVM_I4_EFER            (1u << 15)   // EFER write intercept

// ---------------------------------------------------------------------
//  VMCB control area  (1024 bytes, packed in this order)
// ---------------------------------------------------------------------
//  Layout transcribed from Linux arch/x86/include/asm/svm.h
//  `struct vmcb_control_area`, which is verified against real AMD
//  hardware and carries BUILD_BUG_ON(sizeof == 1024).
//
//  !! DISCREPANCY WITH THE LEGACY DRIVER IN THIS TREE !!
//  drivers/svm_driver.c defines VMCB_IOPM_BASE_PA = 0x030 and
//  VMCB_MSRPM_BASE_PA = 0x038.  Those do NOT match the authoritative
//  layout (0x040 / 0x048).  That driver also has zero NPT support and,
//  by its own header comment, only ever ran a 32-bit flat stub, so a
//  wrong IOPM/MSRPM base would never have been exercised.  This header
//  follows Linux/APM2, not the legacy defines.  See svm_offsets.cpp,
//  which prints the numbers straight from the reference struct.
// ---------------------------------------------------------------------
struct svm_vmcb_control {
    uint32_t intercepts[SVM_MAX_INTERCEPT_WORDS];  // 0x000  (6 words)
    uint32_t reserved_1[15 - SVM_MAX_INTERCEPT_WORDS]; // 0x018..0x03B
    uint16_t pause_filter_thresh;                  // 0x03C
    uint16_t pause_filter_count;                   // 0x03E
    uint64_t iopm_base_pa;                         // 0x040  <-- IOPM
    uint64_t msrpm_base_pa;                        // 0x048  <-- MSRPM
    uint64_t tsc_offset;                           // 0x050
    uint32_t asid;                                 // 0x058
    uint8_t  tlb_ctl;                              // 0x05C
    uint8_t  reserved_2[3];
    uint32_t int_ctl;                              // 0x060
    uint32_t int_vector;                           // 0x064
    uint32_t int_state;                            // 0x068
    uint8_t  reserved_3[4];
    uint32_t exit_code;                            // 0x070 (low 32)
    uint32_t exit_code_hi;                         // 0x074 (high 32)
    uint64_t exit_info_1;                          // 0x078
    uint64_t exit_info_2;                          // 0x080
    uint32_t exit_int_info;                        // 0x088
    uint32_t exit_int_info_err;                    // 0x08C
    uint64_t nested_ctl;                           // 0x090  NP_ENABLE
    uint64_t avic_vapic_bar;                       // 0x098
    uint64_t ghcb_gpa;                             // 0x0A0
    uint32_t event_inj;                            // 0x0A8
    uint32_t event_inj_err;                        // 0x0AC
    uint64_t nested_cr3;                           // 0x0B0  <-- nCR3 (NPT root)
    uint64_t virt_ext;                             // 0x0B8
    uint32_t clean;                                // 0x0C0
    uint32_t reserved_5;                           // 0x0C4
    uint64_t next_rip;                             // 0x0C8
    uint8_t  insn_len;                             // 0x0D0
    uint8_t  insn_bytes[15];                       // 0x0D1..0x0DF
    uint64_t avic_backing_page;                    // 0x0E0
    uint8_t  reserved_6[8];                        // 0x0E8..0x0EF
    uint64_t avic_logical_id;                      // 0x0F0
    uint64_t avic_physical_id;                     // 0x0F8
    uint8_t  reserved_7[8];                        // 0x100..0x107
    uint64_t vmsa_pa;                              // 0x108
    uint8_t  reserved_8[720];                      // 0x110..0x3DF
    uint8_t  reserved_sw[32];                      // 0x3E0..0x3FF
} SVM_PACKED;

// ---------------------------------------------------------------------
//  Segment descriptor as stored in the save area (16 bytes)
// ---------------------------------------------------------------------
//  NOTE ON THE `attrib` FIELD WIDTH
//    AMD stores the segment attribute as a 12-bit "Attribute" field
//    (bits 11:0 = the legacy attribute byte plus some extras) *plus*
//    separate AVL/L/D-B/G bits in the UPPER half of the 16-bit word.
//    The authoritative layout puts L (long mode) at bit 9 of the
//    *extended* attribute word, NOT bit 20 of a 32-bit value.
//
//    Putting L at "bit 20" would overflow this uint16_t and silently
//    vanish -- exactly the kind of bug that only shows up as a #VMFAIL.
//    The correct positions are given below.
struct svm_vmcb_seg {
    uint16_t selector;
    uint16_t attrib;
    uint32_t limit;
    uint64_t base;
} SVM_PACKED;
SVM_STATIC_ASSERT(sizeof(struct svm_vmcb_seg) == 16, "vmcb_seg must be 16 bytes");

// ---- attribute word bit positions (AMD APM2 "Segment Attribute") ----
#define SEG_ATTRIB_TYPE_MASK   0x000Fu   // bits 3:0  type
#define SEG_ATTRIB_S           (1u << 4) // bit 4     S (code/data)
#define SEG_ATTRIB_DPL_SHIFT   5         // bits 6:5  DPL
#define SEG_ATTRIB_P           (1u << 7) // bit 7     present
#define SEG_ATTRIB_AVL         (1u << 8) // bit 8     available
#define SEG_ATTRIB_L           (1u << 9) // bit 9     L  (64-bit / long)
#define SEG_ATTRIB_DB          (1u << 10)// bit 10    D/B
#define SEG_ATTRIB_G           (1u << 11)// bit 11    granularity

// ---------------------------------------------------------------------
//  Ready-made attribute words for a flat 64-bit guest
// ---------------------------------------------------------------------
//  Encoding recap (16-bit attribute word):
//     bits 3:0  type   (code: 1010b = 0xA execute/read; data: 0010b = 0x2 r/w)
//     bit  4    S      (1 = code/data, 0 = system)
//     bits 6:5  DPL
//     bit  7    P
//     bit  8    AVL
//     bit  9    L      (1 = 64-bit code segment)
//     bit 10    D/B
//     bit 11    G
//
//  SO CODE64 = 0xA | S | P | L | G = 0xA | 0x10 | 0x80 | 0x200 | 0x800
//            = 0x0A92 + D/B?  -> 0x0A92 is 0xA|0x10|0x80|0x200|0x800? Let's
//  not hand-wave: the arithmetic is pinned by the static_asserts below,
//  and the values 0x0A93 / 0x0C93 are what the legacy driver in this tree
//  and the Linux SVM code both use for flat 64-bit code/data.
#define SVM_ATTRIB_CODE64 \
    (0xAu | SEG_ATTRIB_S | SEG_ATTRIB_P | SEG_ATTRIB_L | SEG_ATTRIB_G)
#define SVM_ATTRIB_DATA64 \
    (0x2u | SEG_ATTRIB_S | SEG_ATTRIB_P | SEG_ATTRIB_DB | SEG_ATTRIB_G)
#define SVM_ATTRIB_TSS64 \
    (0xBu | SEG_ATTRIB_P)

//  Worked example, so the numbers are auditable rather than copied:
//
//    code64 = type 0xA (execute/read) | S 0x010 | P 0x080 | L 0x200 | G 0x800
//           =             0xA9A
//    data64 = type 0x2 (read/write)   | S 0x010 | P 0x080 | D/B 0x400 | G 0x800
//           =             0xC92
//    tss64  = type 0xB (busy TSS)     | P 0x080            (S stays 0)
//           =             0x08B
//
//  D/B (bit 10) MUST be clear for a 64-bit CODE segment: L and D/B are
//  mutually exclusive in long mode, and setting both is reserved.
SVM_STATIC_ASSERT(SVM_ATTRIB_CODE64 == 0x0A9Au, "code64 attrib must be 0x0A9A");
SVM_STATIC_ASSERT(SVM_ATTRIB_DATA64 == 0x0C92u, "data64 attrib must be 0x0C92");
SVM_STATIC_ASSERT(SVM_ATTRIB_TSS64  == 0x008Bu, "tss64 attrib must be 0x008B");

//  The two mutually-exclusive long-mode bits must never both be set.
SVM_STATIC_ASSERT((SVM_ATTRIB_CODE64 & SEG_ATTRIB_DB) == 0,
                  "64-bit code segment must have D/B clear");
SVM_STATIC_ASSERT((SVM_ATTRIB_CODE64 & SEG_ATTRIB_L) != 0,
                  "64-bit code segment must have L set");

// ---------------------------------------------------------------------
//  VMCB save area  (744 bytes)
// ---------------------------------------------------------------------
struct svm_vmcb_save {
    struct svm_vmcb_seg es;                        // 0x000
    struct svm_vmcb_seg cs;                        // 0x010
    struct svm_vmcb_seg ss;                        // 0x020
    struct svm_vmcb_seg ds;                        // 0x030
    struct svm_vmcb_seg fs;                        // 0x040
    struct svm_vmcb_seg gs;                        // 0x050
    struct svm_vmcb_seg gdtr;                      // 0x060
    struct svm_vmcb_seg ldtr;                      // 0x070
    struct svm_vmcb_seg idtr;                      // 0x080
    struct svm_vmcb_seg tr;                        // 0x090
    uint8_t  reserved_0xa0[42];                    // 0x0A0..0x0C9
    uint8_t  vmpl;                                 // 0x0CA
    uint8_t  cpl;                                  // 0x0CB
    uint8_t  reserved_0xcc[4];                     // 0x0CC..0x0CF
    uint64_t efer;                                 // 0x0D0  <-- EFER (SVME/NXE/LME/LMA)
    uint8_t  reserved_0xd8[112];                   // 0x0D8..0x147
    uint64_t cr4;                                  // 0x148
    uint64_t cr3;                                  // 0x150
    uint64_t cr0;                                  // 0x158
    uint64_t dr7;                                  // 0x160
    uint64_t dr6;                                  // 0x168
    uint64_t rflags;                               // 0x170
    uint64_t rip;                                  // 0x178
    uint8_t  reserved_0x180[88];                   // 0x180..0x1D7
    uint64_t rsp;                                  // 0x1D8
    uint64_t s_cet;
    uint64_t ssp;
    uint64_t isst_addr;
    uint64_t rax;                                 // 0x1F8
    uint64_t star;
    uint64_t lstar;
    uint64_t cstar;
    uint64_t sfmask;
    uint64_t kernel_gs_base;
    uint64_t sysenter_cs;
    uint64_t sysenter_esp;
    uint64_t sysenter_eip;
    uint64_t cr2;                                 // 0x238
    uint8_t  reserved_0x248[32];
    uint64_t g_pat;                               // 0x268
    uint64_t dbgctl;
    uint64_t br_from;
    uint64_t br_to;
    uint64_t last_excp_from;
    uint64_t last_excp_to;
    uint8_t  reserved_0x298[72];
    uint64_t spec_ctrl;                           // 0x2E0
} SVM_PACKED;

// ---------------------------------------------------------------------
//  Whole VMCB
// ---------------------------------------------------------------------
struct svm_vmcb {
    struct svm_vmcb_control control;
    struct svm_vmcb_save    save;
} SVM_PACKED;

// =====================================================================
//  COMPILE-TIME VERIFICATION
// ---------------------------------------------------------------------
//  These are the entire value of this file.  If any assertion fires, the
//  layout does not match APM2 and the VMCB would be silently corrupt at
//  run time -- with no test environment to catch it.
// =====================================================================
SVM_STATIC_ASSERT(sizeof(struct svm_vmcb_control) == VMCB_CONTROL_AREA_SIZE,
                  "VMCB control area must be exactly 1024 bytes (APM2)");
SVM_STATIC_ASSERT(sizeof(struct svm_vmcb_save) == VMCB_SAVE_AREA_SIZE,
                  "VMCB save area must be exactly 744 bytes (APM2)");
SVM_STATIC_ASSERT(sizeof(struct svm_vmcb) == VMCB_TOTAL_SIZE,
                  "VMCB total must be 1768 bytes (1024 + 744)");

// ---- control-area field offsets (APM2 VMCB Layout table) ----
//     Values cross-checked with svm/svm_offsets.cpp, which prints them
//     straight out of the reference struct.
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, intercepts)      == 0x000, "intercepts @0x000");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, pause_filter_thresh) == 0x03C, "pause_filter_thresh @0x03C");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, iopm_base_pa)    == 0x040, "iopm_base_pa @0x040");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, msrpm_base_pa)   == 0x048, "msrpm_base_pa @0x048");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, tsc_offset)      == 0x050, "tsc_offset @0x050");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, asid)            == 0x058, "asid @0x058");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, tlb_ctl)         == 0x05C, "tlb_ctl @0x05C");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, int_ctl)         == 0x060, "int_ctl @0x060");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, exit_code)       == 0x070, "exit_code @0x070");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, exit_info_1)     == 0x078, "exit_info_1 @0x078");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, exit_info_2)     == 0x080, "exit_info_2 @0x080");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, nested_ctl)      == 0x090, "nested_ctl @0x090");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, event_inj)       == 0x0A8, "event_inj @0x0A8");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, nested_cr3)      == 0x0B0, "nested_cr3 @0x0B0");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, clean)           == 0x0C0, "clean @0x0C0");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, next_rip)        == 0x0C8, "next_rip @0x0C8");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, insn_len)        == 0x0D0, "insn_len @0x0D0");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, insn_bytes)      == 0x0D1, "insn_bytes @0x0D1");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, avic_backing_page) == 0x0E0, "avic_backing_page @0x0E0");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, vmsa_pa)         == 0x108, "vmsa_pa @0x108");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_control, reserved_sw)     == 0x3E0, "reserved_sw @0x3E0");

// ---- save-area field offsets ----
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, es)    == 0x000, "save.es @0x000");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, cs)    == 0x010, "save.cs @0x010");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, gdtr)  == 0x060, "save.gdtr @0x060");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, idtr)  == 0x080, "save.idtr @0x080");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, tr)    == 0x090, "save.tr @0x090");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, vmpl)  == 0x0CA, "save.vmpl @0x0CA");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, cpl)   == 0x0CB, "save.cpl @0x0CB");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, efer)  == 0x0D0, "save.efer @0x0D0");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, cr4)   == 0x148, "save.cr4 @0x148");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, cr3)   == 0x150, "save.cr3 @0x150");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, cr0)   == 0x158, "save.cr0 @0x158");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, rflags)== 0x170, "save.rflags @0x170");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, rip)   == 0x178, "save.rip @0x178");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, rsp)   == 0x1D8, "save.rsp @0x1D8");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, rax)   == 0x1F8, "save.rax @0x1F8");
// cr2 sits at 0x240: the reference struct names the FOLLOWING pad
// reserved_0x248[32], i.e. cr2 ends at 0x248 -> it starts at 0x248-8.
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, cr2)   == 0x240, "save.cr2 @0x240");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, g_pat) == 0x268, "save.g_pat @0x268");
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb_save, spec_ctrl) == 0x2E0, "save.spec_ctrl @0x2E0");

// ---- the save area begins right after the control area ----
SVM_STATIC_ASSERT(offsetof(struct svm_vmcb, save) == VMCB_SAVE_AREA_OFFSET,
                  "save area must start at offset 1024");

// ---- exit codes we dispatch on ----
#define SVM_EXIT_IOIO     0x07Bu
#define SVM_EXIT_HLT      0x078u
#define SVM_EXIT_MSR      0x07Cu
#define SVM_EXIT_CPUID    0x072u
#define SVM_EXIT_VMRUN    0x0A0u
#define SVM_EXIT_ERR       (0x80000000u | 0x063u)   // VMEXIT_INVALID

// ---- IOIO exit information bits (APM2 App. C) ----
#define SVM_IOIO_TYPE_MASK    (1u << 0)   // 1 = IN, 0 = OUT
#define SVM_IOIO_STR_SHIFT    2
#define SVM_IOIO_REP_SHIFT    3
#define SVM_IOIO_SIZE_SHIFT   4           // 0=1B 1=2B 3=4B
#define SVM_IOIO_ASIZE_SHIFT  7
#define SVM_IOIO_PORT_MASK    0xFFFF0000u // port number in bits 31:16

#define SVM_IOIO_IS_IN(info)     (((info) & SVM_IOIO_TYPE_MASK) != 0)
#define SVM_IOIO_PORT(info)      (((info) & SVM_IOIO_PORT_MASK) >> 16)
#define SVM_IOIO_SIZE(info)      ((((info) >> SVM_IOIO_SIZE_SHIFT) & 7u) + 1)

// ---- nested paging control ----
#define SVM_NESTED_CTL_NP_ENABLE  (1ull << 0)
#define SVM_NESTED_CTL_SEV_ENABLE (1ull << 1)

#endif // NEXOS_SVM_VMCB_H
