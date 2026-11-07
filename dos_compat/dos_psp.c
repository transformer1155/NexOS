// dos_psp.c - MS-DOS Program Segment Prefix builder
#include <stdint.h>
#include "dos_psp.h"

static void *g_psp_current = 0;

void *dos_psp_current(void){ return g_psp_current; }
void dos_psp_set_current(void *base){ g_psp_current = base; }

void dos_psp_build(void *base, uint16_t psp_seg, uint16_t parent_psp_seg, const char *cmdline){
    if (!base) return;
    // Zero the 256-byte PSP.  This is a freestanding translation unit, so it
    // must not pull <string.h>: on a host without 32-bit libc headers (e.g. a
    // VM with only libc6-dev:amd64) that header fails to resolve and the whole
    // build dies here.  A plain loop has no such dependency.
    {
        unsigned char *z = (unsigned char *)base;
        for (int i = 0; i < 256; i++) z[i] = 0;
    }

    uint8_t *b = (uint8_t *)base;

    // INT 20h at entry
    b[0] = 0xCD; b[1] = 0x20;

    // Next segment (unknown, leave 0)
    *(uint16_t *)(b + 0x02) = 0;

    // CP/M CALL 5 style
    b[0x05] = 0x9A;

    // Terminate/CTRL-C/critical error handlers (set to 0, kernel will patch)
    *(uint16_t *)(b + 0x0A) = 0; *(uint16_t *)(b + 0x0C) = psp_seg;
    *(uint16_t *)(b + 0x0E) = 0; *(uint16_t *)(b + 0x10) = psp_seg;
    *(uint16_t *)(b + 0x12) = 0; *(uint16_t *)(b + 0x14) = psp_seg;

    // Parent PSP
    *(uint16_t *)(b + 0x16) = parent_psp_seg;

    // File handles: stdin/stdout/stderr pre-opened
    for (int i = 0; i < 20; i++) b[0x18 + i] = 0xFF;
    b[0x18 + 0] = 0; // stdin
    b[0x18 + 1] = 1; // stdout
    b[0x18 + 2] = 2; // stderr

    // Environment segment
    *(uint16_t *)(b + 0x2C) = 0;

    // DOS version 7.00 (conventional)
    b[0x42] = 7;
    b[0x43] = 0;

    // Default DTA at 0x80, command tail starts at 0x81
    if (cmdline) {
        int len = 0;
        while (cmdline[len] && cmdline[len] != '\r' && cmdline[len] != '\n' && len < 126) len++;
        b[PSP_CMDTAIL_LEN] = (uint8_t)len;
        for (int i = 0; i < len; i++) b[PSP_CMDTAIL + i] = (uint8_t)cmdline[i];
        if (len < 126) b[PSP_CMDTAIL + len] = '\r';
    } else {
        b[PSP_CMDTAIL_LEN] = 0;
    }

    g_psp_current = base;
}