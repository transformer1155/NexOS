// =====================================================================
//  gfx_ime.cpp  -  Pinyin IME: composing state, candidate lookup, commit
// ---------------------------------------------------------------------
//  Split out of gui_legacy.cpp (Strangler Fig step 4).  This is the first
//  GUI module that lives in its own translation unit.  It needs the mforms
//  host API (ime_commit calls mforms_key), hence mforms_bridge.h -- see the
//  note in that header about the 64-bit stub trap.
// =====================================================================
#include <stdint.h>
#include "mforms_bridge.h"
#include "gfx_ime.h"
// The moved IME lookup consumes the pinyin dictionary TABLES from
// ime_dict.h (gui_legacy.cpp includes it at L24).  Without this the
// new TU cannot see ime_count/ime_pinyin/ime_unicode -- the exact
// cause of the second gfx_ime slot failure.
#include "ime_dict.h"

// ---- state --------------------------------------------------------------
char   g_ime_py[16];         // composing pinyin
int    g_ime_len = 0;
bool   g_ime_active = false;
int    g_ime_cands[9];       // candidate unicode codepoints
int    g_ime_cand_count = 0;
bool   g_ime_cn = false;     // language mode: true=涓枃, false=鑻辨枃

void ime_reset(){
    g_ime_len = 0;
    g_ime_active = false;
    g_ime_cand_count = 0;
    g_ime_py[0] = 0;
}

// Forward declaration: defined just below (see "Encode a Unicode codepoint").
void utf8_encode(uint32_t cp, char out[4]);
// Forward declarations for the serial debug helpers (defined later in this file)
// so ime_commit() can log without moving the definitions.

// Inject one committed Unicode codepoint into whichever input buffer is
// currently focused.  Returns the number of bytes appended.  `target_kind`
// selects where the glyph lands:
//   0 = native buffer (term_input / browser_url) passed via out_buf
//   1 = managed (C#) app, delivered through mforms_key(codepoint)
int ime_commit(uint32_t cp, int target_kind, int app_id,
                      char* out_buf, int* out_len, int out_max){
    char u8[4];
    utf8_encode(cp, u8);
    if (target_kind == 1) {
        // Managed (C#) app: deliver the FULL Unicode codepoint as a single
        // key event.  Sending the raw UTF-8 bytes would split one glyph into
        // 2-3 byte events that the managed text box discards (its OnKey only
        // accepts ASCII).  The C# side re-inserts it via Host.CharStr(cp).
        mforms_key(app_id, (int)cp);
        serial_puts("[IME] commit U+");
        serial_putdec((int)cp);
        serial_puts(" mode=");
        serial_puts(g_ime_cn ? "CN" : "EN");
        serial_puts(" -> managed\n");
        return 1;
    }
    // Native buffer (terminal input line / browser URL): append the UTF-8
    // bytes directly.  Log once so a headless test can prove the CJK glyph
    // actually reached a native input box.
    serial_puts("[IME] commit U+");
    serial_putdec((int)cp);
    serial_puts(" -> native\n");
    int n = 0;
    for (int i = 0; u8[i] && *out_len < out_max - 1; i++)
        out_buf[(*out_len)++] = u8[i], n++;
    out_buf[*out_len] = 0;
    return n;
}

// Prefix match against the sorted pinyin dictionary.
void ime_lookup(){
    g_ime_cand_count = 0;
    if (g_ime_len == 0) return;
    for (int i = 0; i < ime_count && g_ime_cand_count < 9; i++) {
        // prefix match: ime_pinyin[i] starts with g_ime_py
        const char* a = ime_pinyin[i];
        const char* b = g_ime_py;
        int n = g_ime_len;
        bool match = true;
        while (n > 0 && *a) { if (*a != *b) { match = false; break; } a++; b++; n--; }
        if (match && n == 0)
            g_ime_cands[g_ime_cand_count++] = ime_unicode[i];
    }
}

void utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp; out[1] = 0;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
    }
}
