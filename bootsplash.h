// =====================================================================
//  bootsplash.h  -  earliest-possible boot animation (32-bit kernel)
// ---------------------------------------------------------------------
//  Draws an animated "NexOS" boot screen straight into the linear
//  framebuffer from the first moment the kernel can address it (right
//  after vmm_init() has mapped any >4 GiB GOP framebuffer into its
//  <4 GiB window), and keeps it moving until the desktop takes over.
//
//  Every entry point is a safe no-op when there is no usable linear
//  framebuffer (BltOnly GOP, VGA-only boot, text-boot build), so the
//  caller can invoke them unconditionally.
// =====================================================================
#ifndef NEXOS_BOOTSPLASH_H
#define NEXOS_BOOTSPLASH_H

#ifdef __cplusplus
extern "C" {
#endif

// Probe the framebuffer and paint the first frame.  Returns 1 when the
// splash owns the screen, 0 when it is unavailable.  Idempotent: a second
// call after it is already up returns 1 without repainting.
int  boot_splash_init(void);

// Advance to a boot milestone (values come straight from boot_stage()).
// Unknown values are ignored; the progress bar never moves backwards.
void boot_splash_stage(int stage);

// Non-blocking.  Advances the animation when its frame interval has
// elapsed; cheap enough to call from a tight polling loop.
void boot_splash_tick(void);

// 1 while the splash owns the screen.  The framebuffer text console uses
// this to stay quiet so its 80x25 output cannot scribble over the art.
int  boot_splash_active(void);

// Hand the screen over (called just before the desktop starts).
void boot_splash_finish(void);

#ifdef __cplusplus
}
#endif

#endif  // NEXOS_BOOTSPLASH_H
