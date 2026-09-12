// =====================================================================
//  intel_cursor.h  -  Intel iGPU hardware-cursor backend (32-bit kernel)
// ---------------------------------------------------------------------
//  The desktop picks its pointer backend at startup and falls back when a
//  backend is unavailable, so the pointer is never lost:
//
//      HW   (this file) -> HOST -> SOFT
//
//  HW   = the GPU scans out its own cursor plane; the guest never writes a
//         cursor pixel, it only moves the plane.
//  HOST = the hypervisor owns the cursor (needs an absolute pointer).
//  SOFT = the GUI paints the arrow into the backbuffer (always possible).
//
//  intel_cursor_probe() audits the display and returns 1 only when the
//  hardware cursor is actually usable; otherwise it returns 0 and the caller
//  keeps the software cursor.  It never writes to the GPU.
// =====================================================================
#ifndef NEXOS_INTEL_CURSOR_H
#define NEXOS_INTEL_CURSOR_H

#ifdef __cplusplus
extern "C" {
#endif

// Audit the display.  1 = hardware cursor ready (the GUI must stop painting
// its own arrow), 0 = not available (keep the software cursor).
int  intel_cursor_probe(void);

// Move / show / hide the hardware cursor.  No-ops unless probe() returned 1.
void intel_cursor_move(int x, int y);
void intel_cursor_show(int on);

#ifdef __cplusplus
}
#endif

#endif  // NEXOS_INTEL_CURSOR_H
