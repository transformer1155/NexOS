// =====================================================================
//  gfx_ime.h  -  Pinyin IME state machine (declarations)
// ---------------------------------------------------------------------
//  Extracted from gui_legacy.cpp (Strangler Fig step 4).  The definitions
//  live in gfx_ime.cpp.  Symbols that used to be file-private ('static') are
//  declared here with external linkage; they are DEFINED exactly once.
// =====================================================================
#ifndef NEXOS_GUI_GFX_IME_H
#define NEXOS_GUI_GFX_IME_H

#include <stdint.h>

// ---- IME state (bound to the active terminal window) --------------------
extern char g_ime_py[16];
extern int g_ime_len;
extern bool g_ime_active;
extern int g_ime_cands[9];
extern int g_ime_cand_count;
extern bool g_ime_cn;

// ---- state machine ------------------------------------------------------
// signatures DERIVED from the definitions (see sig_of) -- do not retype:
void ime_reset();
int ime_commit(uint32_t cp, int target_kind, int app_id, char* out_buf, int* out_len, int out_max);
void ime_lookup();
void utf8_encode(uint32_t cp, char out[4]);

// ---- serial debug helpers: DEFINED in gui_legacy.cpp, exported for this TU
void serial_puts(const char* s);
void serial_putdec(int v);
void serial_puthex32(uint32_t v);

#endif // NEXOS_GUI_GFX_IME_H
