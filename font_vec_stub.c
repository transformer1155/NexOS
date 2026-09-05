/*
 * font_vec_stub.c - 32-bit kernel stub for the vector font API.
 *
 * The full stb_truetype rasterizer (font_vec.c) is linked only into the
 * 64-bit kernel, which has a large load window.  The 32-bit kernel must stay
 * under the 0x10000..0x9FC00 (EBDA) window, so it links this stub instead and
 * keeps using the baked multi-resolution Latin font (font_la.bin) + the 16x16
 * CJK bitmap (zfont.bin).  gui.cpp already falls back to those when
 * vec_ready() returns 0, so behaviour is unchanged on the 32-bit side.
 */
#include "font_vec.h"

int vec_init(int (*read_file)(int, const char*, uint8_t*, int)) {
    (void)read_file;
    return -1;          /* no vector font on the 32-bit kernel */
}

int vec_ready(void) { return 0; }

const uint8_t* vec_glyph(uint32_t cp, int px, int* w, int* h, int* xoff, int* yoff) {
    (void)cp; (void)px; (void)w; (void)h; (void)xoff; (void)yoff;
    return NULL;
}

int vec_advance(uint32_t cp, int px) {
    (void)cp;
    return px;          /* sensible fallback advance */
}

void vec_last_err(int* code, int* got, int* stb) {
    if (code) *code = -1;
    if (got)  *got  = 0;
    if (stb)  *stb  = 0;
}
