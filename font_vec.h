/*
 * font_vec.h - public API for the NexOS runtime vector font rasterizer.
 * See font_vec.c for implementation notes.
 */
#ifndef FONT_VEC_H
#define FONT_VEC_H

#include <stdint.h>
#include <stddef.h>   /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

/* Load the TTF (msyh.ttf) from SFS via the kernel's read_file bridge.
 * Returns 0 on success, <0 on failure. */
int  vec_init(int (*read_file)(int fs_type, const char* name,
                               uint8_t* buf, int bufsize));

/* True once vec_init succeeded. */
int  vec_ready(void);

/* Rasterize codepoint `cp` at pixel height `px`.
 * w,h        : AA bitmap dimensions (out)
 * xoff,yoff  : left/top bearing (may be negative, out)
 * Returns a 1-channel coverage buffer (0..255), or NULL if glyph absent.
 * Buffer is valid until the next 2 vec_glyph() calls. */
const uint8_t* vec_glyph(uint32_t cp, int px, int* w, int* h,
                         int* xoff, int* yoff);

/* Horizontal advance (pixels) for a codepoint at height px. */
int  vec_advance(uint32_t cp, int px);

/* Last-error details for diagnosis (code/got/stb). May be NULL. */
void vec_last_err(int* code, int* got, int* stb);

#ifdef __cplusplus
}
#endif

#endif /* FONT_VEC_H */
