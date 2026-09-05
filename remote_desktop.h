#ifndef NEXOS_REMOTE_DESKTOP_H
#define NEXOS_REMOTE_DESKTOP_H

#include <stdint.h>

/* =====================================================================
 *  remote_desktop.h  -  NexOS remote-desktop ABI
 * ---------------------------------------------------------------------
 *  Shared between the kernel (gui.cpp / net.cpp / linux_compat.cpp) and
 *  guest Linux ELF32 applications.  A guest ELF queries the framebuffer
 *  via syscall 400 and blocks for input via syscall 401; the kernel
 *  serves the framebuffer as a compressed image over HTTP (/screen) and
 *  injects pointer/keyboard events received on /input into the guest.
 * ===================================================================== */

/* Framebuffer pixel formats (must match gui.cpp PXF_* constants). */
#define NEXOS_FB_BGRX32 0
#define NEXOS_FB_RGBX32 1
#define NEXOS_FB_RGB24  2
#define NEXOS_FB_RGB565 3

/* Linux syscall numbers used for the remote-desktop interface.
 * Dispatched inside linux_compat.cpp's int 0x80 handler. */
#define NEXOS_SYS_FB    410   /* query framebuffer info (renumbered from 400 to avoid clash with guest TCP socket bridge 400/401) */
#define NEXOS_SYS_INPUT 411   /* block until an input event arrives */

/* Framebuffer description returned by syscall 410 (nexos_fb_query).
 * 'phys' is an identity-mapped physical address the guest can write to
 * directly (the kernel page tables identity-map the LFB region). */
struct NexosFBInfo {
    uint32_t phys;        /* physical framebuffer base */
    uint16_t width;
    uint16_t height;
    uint16_t pitch;       /* bytes per scanline */
    uint8_t  bpp;         /* bits per pixel (32 for BGRX32) */
    uint8_t  format;      /* NEXOS_FB_* pixel format */
    uint8_t  _pad[2];
};

/* Shared input state.  Updated by the kernel when an HTTP /input request
 * arrives, read by the guest via syscall 411 (nexos_input_wait). */
struct NexosInput {
    uint32_t seq;         /* increments on every new input event */
    int32_t  mouse_x;     /* absolute, framebuffer pixels */
    int32_t  mouse_y;
    uint8_t  buttons;     /* bit0=left bit1=right bit2=middle */
    uint8_t  key;         /* last ASCII/keycode (0 = none) */
    uint8_t  key_down;    /* 1 on press edge, 0 otherwise */
    uint8_t  _pad;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel-side accessors (implemented in gui.cpp / linux_compat.cpp). */
void nexos_fb_query(struct NexosFBInfo* out);
void nexos_input_inject(int32_t mx, int32_t my, uint8_t buttons,
                        uint8_t key, uint8_t down);
void nexos_input_wait(struct NexosInput* out);   /* blocks (polls net_poll) */

/* Global shared input state (defined in linux_compat.cpp). */
extern struct NexosInput g_rd_input;

#ifdef __cplusplus
}
#endif

#endif /* NEXOS_REMOTE_DESKTOP_H */
