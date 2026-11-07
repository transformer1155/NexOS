// Print the REAL offsets of the reference VMCB control-area layout so the
// hand-written struct in svm_vmcb.h can be corrected against APM2.
//
// Why this exists: the first draft of svm_vmcb.h placed iopm_base_pa at
// 0x028, but the reference layout puts it at 0x040.  static_assert caught
// it.  This program derives the authoritative numbers so the fix is
// mechanical rather than guesswork.
//
// Build/run: g++ -o svm_offsets svm_offsets.cpp && ./svm_offsets
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define SVM_PACKED __attribute__((packed))

struct vmcb_control_area {
    uint32_t intercepts[6];
    uint32_t reserved_1[15 - 6];
    uint16_t pause_filter_thresh;
    uint16_t pause_filter_count;
    uint64_t iopm_base_pa;
    uint64_t msrpm_base_pa;
    uint64_t tsc_offset;
    uint32_t asid;
    uint8_t  tlb_ctl;
    uint8_t  reserved_2[3];
    uint32_t int_ctl;
    uint32_t int_vector;
    uint32_t int_state;
    uint8_t  reserved_3[4];
    uint32_t exit_code;
    uint32_t exit_code_hi;
    uint64_t exit_info_1;
    uint64_t exit_info_2;
    uint32_t exit_int_info;
    uint32_t exit_int_info_err;
    uint64_t nested_ctl;
    uint64_t avic_vapic_bar;
    uint64_t ghcb_gpa;
    uint32_t event_inj;
    uint32_t event_inj_err;
    uint64_t nested_cr3;
    uint64_t virt_ext;
    uint32_t clean;
    uint32_t reserved_5;
    uint64_t next_rip;
    uint8_t  insn_len;
    uint8_t  insn_bytes[15];
    uint64_t avic_backing_page;
    uint8_t  reserved_6[8];
    uint64_t avic_logical_id;
    uint64_t avic_physical_id;
    uint8_t  reserved_7[8];
    uint64_t vmsa_pa;
    uint8_t  reserved_8[720];
    uint8_t  reserved_sw[32];
} SVM_PACKED;

#define SHOW(f) \
    printf("  %-22s 0x%03X\n", #f, (unsigned)offsetof(struct vmcb_control_area, f))

int main() {
    printf("reference vmcb_control_area offsets (Linux svm.h layout):\n");
    SHOW(intercepts);
    SHOW(pause_filter_thresh);
    SHOW(iopm_base_pa);
    SHOW(msrpm_base_pa);
    SHOW(tsc_offset);
    SHOW(asid);
    SHOW(tlb_ctl);
    SHOW(int_ctl);
    SHOW(exit_code);
    SHOW(exit_info_1);
    SHOW(exit_info_2);
    SHOW(event_inj);
    SHOW(nested_ctl);
    SHOW(nested_cr3);
    SHOW(clean);
    SHOW(next_rip);
    SHOW(insn_len);
    SHOW(insn_bytes);
    SHOW(avic_backing_page);
    SHOW(vmsa_pa);
    SHOW(reserved_8);
    SHOW(reserved_sw);
    printf("\n  sizeof = %u (APM2 says 1024)\n",
           (unsigned)sizeof(struct vmcb_control_area));
    return 0;
}