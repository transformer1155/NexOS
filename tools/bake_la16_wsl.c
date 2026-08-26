#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_h = 16, g_base = 0x20, g_count = 96;

int main(int argc, char** argv) {
    const char* ttf = argc > 1 ? argv[1] : "sfs_files/msyh.ttf";
    const char* out = argc > 2 ? argv[2] : "sfs_files/font_la16.bin";

    FILE* f = fopen(ttf, "rb");
    if (!f) { fprintf(stderr, "open ttf fail\n"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read ttf fail\n"); return 1; }
    fclose(f);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, buf, 0)) { fprintf(stderr, "init font fail\n"); return 1; }

    float scale = stbtt_ScaleForPixelHeight(&font, (float)g_h);

    unsigned char hdr[8];
    hdr[0] = 'L'; hdr[1] = 'A'; hdr[2] = '1'; hdr[3] = '6';
    hdr[4] = (unsigned char)g_base;
    hdr[5] = (unsigned char)g_count;
    hdr[6] = (unsigned char)(g_h & 0xFF);
    hdr[7] = (unsigned char)((g_h >> 8) & 0xFF);

    FILE* o = fopen(out, "wb");
    if (!o) { fprintf(stderr, "open out fail\n"); return 1; }
    fwrite(hdr, 1, 8, o);

    for (int i = 0; i < g_count; i++) {
        int cp = g_base + i, w = 0, h = 0, xo = 0, yo = 0;
        unsigned char* bmp = stbtt_GetCodepointBitmap(&font, scale, scale, cp, &w, &h, &xo, &yo);
        int cw = w < g_h ? w : g_h;
        int ch = h < g_h ? h : g_h;
        unsigned char cell[256];
        memset(cell, 0, 256);
        if (bmp) {
            int oy = (g_h - h) / 2; if (oy < 0) oy = 0;
            int ox = g_h / 2 - w / 2; if (ox < 0) ox = 0;
            for (int y = 0; y < ch; y++)
                for (int x = 0; x < cw; x++) {
                    int sx = x - ox, sy = y - oy;
                    if (sx >= 0 && sx < w && sy >= 0 && sy < h)
                        cell[y * g_h + x] = bmp[sy * w + sx];
                }
            stbtt_FreeBitmap(bmp, 0);
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        int advpx = (int)(adv * scale + 0.5f);
        if (advpx < 1) advpx = 1;
        if (cw > 16) cw = 16;
        fputc(cw, o);
        fputc(advpx & 0xFF, o);
        fwrite(cell, 1, g_h * g_h, o);
    }
    fclose(o);
    printf("baked %d glyphs -> %s\n", g_count, out);
    return 0;
}
