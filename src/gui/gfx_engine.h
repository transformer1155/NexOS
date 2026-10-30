// =====================================================================
//  gfx_engine.h  -  rendering data tables shared by the GUI modules
// ---------------------------------------------------------------------
//  Extracted from gui_legacy.cpp (Strangler Fig, step 2a phase 1).
//  These tables used to have INTERNAL linkage (static / const), so they are
//  re-declared here and defined with EXTERNAL linkage in gfx_engine.cpp.
//  Phase 1 moves data only -- no code path changes, .text is cmp-verified.
// =====================================================================
#ifndef NEXOS_GUI_GFX_ENGINE_H
#define NEXOS_GUI_GFX_ENGINE_H

#include <stdint.h>

// sRGB <-> linear (gamma 2.4) lookup tables, used by blend_pixel_lin().
extern const uint8_t G_TO_LIN[256];
extern const uint8_t G_TO_SRGB[256];

// Legacy 8x16 ASCII bitmap font (baked); draw_char()/fb console fallback.
extern const uint8_t font8x16[256][16];

#endif // NEXOS_GUI_GFX_ENGINE_H
