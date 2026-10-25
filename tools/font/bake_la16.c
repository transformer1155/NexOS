/*
 * bake_la16.c - Host tool: rasterize the ASCII range (0x20-0x7F) of a TrueType
 * font into a 16x16 grayscale (anti-aliased, coverage) bitmap pack.
 *
 * This is the "modern" Latin text source for the 32-bit kernel, which cannot
 * link the full stb_truetype rasterizer (EBDA load-window size limit).  We
 * rasterize once at build time using the same stb_truetype engine the 64-bit
 * kernel uses, and the 32-bit runtime simply alpha-blends the pre-baked
 * coverage bitmaps.  Result: real 16px anti-aliased Latin text on the 32-bit
 * OS without growing the kernel image.
 *
 * Output format (font_la16.bin):
 *   [0..3]  "LA16" magic
 *   [4]     base codepoint (0x20)
 *   [5]     glyph count (96)
 *   [6..7]  cell height in pixels (16, little-endian)
 *   then per glyph (count times), each exactly (1 + h*h) bytes:
 *     [0]   cell width (== h, i.e. 16)
 *     [1..] h*h grayscale bytes, row-major, MSB-left, value 0..255 coverage
 *
 * Build: gcc bake_la16.c -I tools/stb -o bake_la16  (needs system <math.h>)
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

int main(int argc, char** argv) {
    const char* ttfpath = "sfs_files/msyh.ttf";
    const char* outpath = "sfs_files/font_la16.bin";
    if (argc > 1) ttfpath = argv[1];
    if (argc > 2) outpath = argv[2];

    int px = 16;                 /* target cell size */
    int base = 0x20, count = 96;

    FILE* f = fopen(ttfpath, "rb");
    if (!f) { fprintf(stderr, "bake_la16: cannot open %s\n", ttfpath); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(n ? n : 1);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "bake_la16: read %s\n", ttfpath); return 1; }
    fclose(f);

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, buf, 0)) { fprintf(stderr, "bake_la16: InitFont failed\n"); return 1; }

    float scale = stbtt_ScaleForPixelHeight(&info, (float)px);
    long outsize = 8 + (long)count * (1 + (long)px * px);
    unsigned char* out = (unsigned char*)malloc(outsize);
    memcpy(out, "LA16", 4);
    out[4] = (unsigned char)base;
    out[5] = (unsigned char)count;
    out[6] = (unsigned char)(px & 0xFF);
    out[7] = (unsigned char)((px >> 8) & 0xFF);

    long pos = 8;
    int baked = 0;
    for (int i = 0; i < count; i++) {
        int cp = base + i;
        int gl = stbtt_FindGlyphIndex(&info, cp);
        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* bmp = NULL;
        if (gl) bmp = stbtt_GetGlyphBitmap(&info, scale, scale, gl, &w, &h, &xoff, &yoff);
        if (bmp) baked++;

        out[pos++] = (unsigned char)px;   /* cell width */
        int ty = -yoff;                   /* glyph top within the cell */
        for (int r = 0; r < px; r++) {
            int br = r - ty;              /* source bitmap row */
            for (int c = 0; c < px; c++) {
                int v = 0;
                if (bmp && br >= 0 && br < h && c < w) v = bmp[br * w + c];
                out[pos++] = (unsigned char)v;
            }
        }
        if (bmp) stbtt_FreeBitmap(bmp, NULL);
    }
    free(buf);

    FILE* o = fopen(outpath, "wb");
    if (!o) { fprintf(stderr, "bake_la16: cannot write %s\n", outpath); return 1; }
    fwrite(out, 1, pos, o);
    fclose(o);
    printf("bake_la16: baked %d/%d glyphs -> %s (%ld bytes)\n", baked, count, outpath, pos);
    free(out);
    return 0;
}
