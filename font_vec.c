/*
 * font_vec.c - True vector font rasterizer for NexOS GUI.
 *
 * Wraps stb_truetype.h (public domain, Sean Barrett) to load a real
 * TrueType font (Microsoft YaHei subset: ASCII + GB2312 level-1 + punctuation)
 * from the SFS volume at GUI init, parse the glyf outlines, and rasterize any
 * codepoint to a 1-channel antialiased bitmap at an arbitrary pixel height.
 *
 * This replaces the old baked multi-res Latin bitmap (font_la.bin) and the
 * 16x16 CJK bitmap (zfont.bin) with a single runtime vector path that stays
 * crisp at every UI size -- the X-stage of the font modernization.
 *
 * Allocations are redirected to the kernel heap (kmalloc/kfree) and the
 * kernel's memcpy/memset so this compiles freestanding inside the OS kernel.
 */

#include "font_vec.h"

#include <stdint.h>
#include <stddef.h>

/* ---- redirect stb allocators to the kernel heap ---------------------- */
extern void* kmalloc(unsigned int size);
extern void  kfree(void* ptr);
extern void* memcpy(void* d, const void* s, unsigned int n);
extern void* memset(void* d, int v, unsigned int n);

#define STBTT_malloc(sz, u)  ((void)(u), kmalloc((unsigned)(sz)))
#define STBTT_free(p, u)     ((void)(u), kfree(p))
#define STBTT_memcpy         memcpy
#define STBTT_memset         memset
#define STBTT_assert(x)      ((void)0)

/* Provide strlen without pulling in <string.h> (freestanding build). */
static unsigned vec_strlen(const char* s) {
    unsigned n = 0;
    if (s) while (s[n]) n++;
    return n;
}
#define STBTT_strlen(x)      vec_strlen(x)

/* Math is supplied by tools/vecmath/math.h (on the include path), which maps
 * sqrt/pow/acos/cos/fmod/floor/ceil/fabs/sin onto real freestanding impls.
 * stb_truetype.h's #include <math.h> resolves to that shim. */



#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* ---- module state ---------------------------------------------------- */
static stbtt_fontinfo g_font;
static unsigned char* g_ttf_buf = NULL;   /* owned by kmalloc, lives whole boot */
static int            g_inited   = 0;

/* Last-error reporting for diagnosis (set in vec_init). */
static int g_err_code = 0;   /* our return code */
static int g_err_got  = 0;   /* bytes read back */
static int g_err_stb  = 0;   /* stbtt_InitFont raw result (0 = fail) */

void vec_last_err(int* code, int* got, int* stb) {
    if (code) *code = g_err_code;
    if (got)  *got  = g_err_got;
    if (stb)  *stb  = g_err_stb;
}

/* Static AA buffers (ring of 2) so callers can fetch the next glyph while
 * still drawing the previous one. Each vec_glyph call returns a pointer into
 * this ring; the buffer is valid until the next 2 vec_glyph calls. */
#define VEC_NBUF 2
#define VEC_BUFMAX (256 * 256)            /* up to 256x256 AA glyph */
static uint8_t g_buf[VEC_NBUF][VEC_BUFMAX];
static int     g_buf_idx = 0;

int (*g_read_file)(int fs_type, const char* name, uint8_t* buf, int bufsize) = NULL;

/* ----------------------------------------------------------------------
 * vec_init: load the TTF from SFS and parse tables.
 * read_file must be the kernel's SFS bridge (g_cb.read_file).
 * Returns 0 on success, <0 on failure.
 * -------------------------------------------------------------------- */
int vec_init(int (*read_file)(int, const char*, uint8_t*, int)) {
    g_err_code = 0; g_err_got = 0; g_err_stb = 0;
    if (g_inited) return 0;
    g_read_file = read_file;
    if (!g_read_file) { g_err_code = -1; return -1; }

    /* Peek size first. */
    uint8_t hdr[16];
    int n = g_read_file(1, "msyh.ttf", hdr, sizeof(hdr));
    if (n < 4) { g_err_code = -2; return -2; }

    /* Read whole file. 1.7MB worst case; our subset is ~380KB. */
    unsigned char* buf = (unsigned char*)kmalloc(2 * 1024 * 1024);
    if (!buf) { g_err_code = -3; return -3; }
    int got = g_read_file(1, "msyh.ttf", buf, 2 * 1024 * 1024);
    g_err_got = got;
    if (got < 100) { kfree(buf); g_err_code = -4; return -4; }

    int stb = stbtt_InitFont(&g_font, buf, 0);
    g_err_stb = stb;
    if (!stb) { kfree(buf); g_err_code = -5; return -5; }

    g_ttf_buf = buf;        /* keep alive for the whole session */
    g_inited  = 1;
    return 0;
}

int vec_ready(void) { return g_inited; }

/* ----------------------------------------------------------------------
 * vec_glyph: rasterize codepoint `cp` at pixel height `px`.
 * Fills *w/*h with the AA bitmap dimensions and *xoff/*yoff with the
 * (possibly negative) left/top bearing in pixels. Returns a pointer to a
 * 1-channel (coverage 0..255) buffer of size w*h, or NULL if not present.
 * The returned buffer is owned by the ring and must be consumed before the
 * next 2 vec_glyph calls.
 * -------------------------------------------------------------------- */
const uint8_t* vec_glyph(uint32_t cp, int px, int* w, int* h, int* xoff, int* yoff) {
    if (!g_inited) return NULL;

    int glyph = stbtt_FindGlyphIndex(&g_font, (int)cp);
    if (glyph == 0) return NULL;          /* .notdef */

    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int lsb, advance;
    /* We only need the bitmap; stb computes xoff/yoff internally. */
    (void)lsb; (void)advance;

    int rw = 0, rh = 0, rxo = 0, ryo = 0;
    unsigned char* bmp = stbtt_GetGlyphBitmap(&g_font, scale, scale, glyph,
                                              &rw, &rh, &rxo, &ryo);
    if (!bmp || rw <= 0 || rh <= 0) return NULL;
    if (rw * rh > VEC_BUFMAX) { STBTT_free(bmp, NULL); return NULL; }

    uint8_t* dst = g_buf[g_buf_idx];
    g_buf_idx = (g_buf_idx + 1) % VEC_NBUF;
    for (int i = 0; i < rw * rh; i++) dst[i] = bmp[i];
    STBTT_free(bmp, NULL);

    *w = rw; *h = rh; *xoff = rxo; *yoff = ryo;
    return dst;
}

/* Optional: advance width in pixels for a codepoint at height px. */
int vec_advance(uint32_t cp, int px) {
    if (!g_inited) return px;             /* sensible fallback */
    int glyph = stbtt_FindGlyphIndex(&g_font, (int)cp);
    if (glyph == 0) return px;
    float scale = stbtt_ScaleForPixelHeight(&g_font, (float)px);
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&g_font, glyph, &adv, &lsb);
    return (int)(adv * scale + 0.5f);
}
