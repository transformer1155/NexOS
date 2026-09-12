// =====================================================================
//  svga.h  -  VMware SVGA-II 2D accelerator + hardware cursor
// ---------------------------------------------------------------------
//  The first real GPU driver in NexOS.  SVGA-II is the pragmatic starting
//  point because it is the only accelerated device QEMU offers that has:
//    * a linear framebuffer (so the existing GOP/VBE model still holds),
//    * a documented FIFO with 2D commands (fill / copy / update), and
//    * a real hardware cursor with an ARGB image and a hotspot.
//
//  The driver deliberately does NOT set the display mode: the firmware has
//  already done that and the rest of the kernel draws into the linear
//  framebuffer.  SVGA-II is used purely as an accelerator on top of it, so
//  a machine without it loses nothing but speed.
//
//  Every entry point is fail-safe: if the device is absent, or the FIFO
//  cannot be brought up exactly as the spec requires, the functions become
//  no-ops and the caller keeps its CPU path.
// =====================================================================
#ifndef NEXOS_SVGA_H
#define NEXOS_SVGA_H

#ifdef __cplusplus
extern "C" {
#endif

// Probe the device and bring up the command FIFO.  Returns 1 when hardware
// acceleration is usable, 0 otherwise (and then every call below no-ops).
//
// svga_mode_hint() MUST be called first, with the mode the kernel is actually
// drawing.  While the device is enabled it owns the scanout and takes its
// surface size from its own registers, so enabling it while they still hold
// the power-on 640x480 blanks the host display even though the guest keeps
// drawing happily.  The hint is what keeps the two in step.
void svga_mode_hint(int w, int h, int bpp, int pitch);

// Whether svga_init() may actually ENABLE the device.  Default OFF.
//
// Enabling hands the scanout to SVGA, and it then takes the surface size and
// PIXEL FORMAT from its own registers.  On this guest the firmware left a
// 16 bpp VBE mode while the device insists on 32 bpp (a write of
// SVGA_REG_BITS_PER_PIXEL=16 reads back 32), so the host scans out 4 bytes
// per pixel against a 2 byte framebuffer: the picture comes back horizontally
// duplicated and striped.  Until that format ownership is resolved, the
// driver must audit and report WITHOUT touching the display.
int  svga_init(void);
void svga_allow_enable(int on);
int  svga_ready(void);

// ---- 2D acceleration ---------------------------------------------------
// Solid fill in the *screen's* 0x00RRGGBB colour, accelerating what the
// compositor otherwise does with a pixel loop.
void svga_fill_rect(int x, int y, int w, int h, unsigned int rgb);
// Publish a rectangle that was written through the linear framebuffer.
// SVGA keeps a shadow of VRAM, so CPU writes are invisible to the host
// until the damaged area is announced.
void svga_update(int x, int y, int w, int h);

// ---- Hardware cursor ---------------------------------------------------
// Define (or redefine) the cursor: width/height are the drawn size, hx/hy
// the hotspot, argb a row-major ARGB8888 bitmap of w*h pixels.
int  svga_cursor_define(int w, int h, int hx, int hy, const unsigned int* argb);
void svga_cursor_move(int x, int y);
void svga_cursor_show(int on);
// Define the built-in arrow and move it once, then report whether the device
// acknowledged (1) or not (0).  The return value is a safety interlock, not
// just a log line: selecting the hardware cursor REMOVES the painted arrow,
// so a caller must not switch on the strength of "the device exists" alone --
// an unacknowledged cursor plane would leave the user with no pointer at all.
int  svga_cursor_selftest(void);

#ifdef __cplusplus
}
#endif

#endif  // NEXOS_SVGA_H
