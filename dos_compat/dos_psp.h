#ifndef DOS_PSP_H
#define DOS_PSP_H

#include <stdint.h>

// =====================================================================
//  dos_psp.h  -  MS-DOS Program Segment Prefix
// ---------------------------------------------------------------------
//  For a .COM program loaded at CS=DS=ES=SS = segment of PSP, IP=0x100.
//  The PSP sits at offset 0 of that segment.  This header defines the
//  minimal layout NexOS needs to support INT 21h and a clean exit.
// =====================================================================

#pragma pack(push, 1)
struct DosPsp {
    uint16_t int20;        // 0x00: INT 20h (old exit)
    uint16_t next_seg;     // 0x02: next paragraph after program
    uint8_t  fill1;        // 0x04
    uint8_t  cpm_call;     // 0x05: CP/M CALL 5
    uint32_t cpm_farcall;  // 0x06
    uint16_t int22;        // 0x0A: terminate address
    uint16_t int22_seg;
    uint16_t int23;        // 0x0E: ctrl-c address
    uint16_t int23_seg;
    uint16_t int24;        // 0x12: critical error address
    uint16_t int24_seg;
    uint16_t parent_psp;   // 0x16: parent PSP segment
    uint8_t  files[20];    // 0x18: job file table
    uint16_t env_seg;      // 0x2C: environment segment
    uint32_t stack_ptr;    // 0x2E: SS:SP on entry (set by DOS)
    uint16_t file_handles;// 0x32
    uint32_t handle_ptr;  // 0x34: pointer to JFT
    uint32_t prev_psp;    // 0x38
    uint8_t  reserved1[4];// 0x3C
    uint8_t  dos_ver_major;//0x42
    uint8_t  dos_ver_minor;//0x43
    uint8_t  unused[14];   // 0x44
    uint8_t  int21_retf;   // 0x50: INT 21h entry
    uint8_t  int21_ret;    // 0x51
    uint8_t  int21_iretf;  // 0x52
    uint8_t  reserved2[9];// 0x53
    uint8_t  fcb1[16];     // 0x5C
    uint8_t  fcb2[16];     // 0x6C
    uint8_t  dta[128];     // 0x80: default DTA
    uint8_t  cmdtail[128]; // 0x80? actually at 0x80 in PSP is DTA area; command tail is at 0x81
    // DOS: 0x80 = length of command tail, 0x81 = command tail (up to 127 chars + CR)
};
#pragma pack(pop)

// PSP layout details per MS-DOS
#define PSP_CMDTAIL_LEN   0x80
#define PSP_CMDTAIL       0x81

// Build a minimal PSP at `base` (linear address of PSP segment start).
// `psp_seg` is the segment value (base >> 4).
// `parent_psp_seg` is parent PSP segment, or 0 for kernel.
// `cmdline` is copied to command tail (may be NULL).
void dos_psp_build(void *base, uint16_t psp_seg, uint16_t parent_psp_seg, const char *cmdline);

// Get a pointer to the current PSP (linear). Returns NULL if none.
void *dos_psp_current(void);

// Switch the current PSP pointer.
void dos_psp_set_current(void *base);

#endif // DOS_PSP_H