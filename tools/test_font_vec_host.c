/* Host-side unit test for font_vec.c (X-stage vector rasterizer).
 * Stubs the kernel heap/string calls with libc, loads sfs_files/msyh.ttf
 * from disk, and verifies vec_init + vec_glyph scale glyphs at two sizes.
 * This validates the rasterization logic WITHOUT needing the 64-bit VM
 * disk path (which is unreliable under QEMU BIOS).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- kernel heap/string stubs (font_vec.c expects these) ---------- */
void* kmalloc(unsigned int size) { return malloc(size); }
void  kfree(void* ptr) { free(ptr); }
/* memcpy/memset come from libc via <string.h> */

/* ---- provide the read_file bridge font_vec uses -------------------- */
#include <sys/stat.h>
static unsigned char* g_ttf = NULL;
static int g_ttf_len = 0;

static int host_read_file(int fs_type, const char* name, uint8_t* buf, int bufsize) {
    (void)fs_type;
    /* only serve msyh.ttf from the loaded buffer */
    if (strcmp(name, "msyh.ttf") != 0) return -1;
    int n = g_ttf_len < bufsize ? g_ttf_len : bufsize;
    memcpy(buf, g_ttf, n);
    return n;
}

/* ---- pull in font_vec.c (which #includes stb_truetype.h) ---------- */
#define FONT_VEC_HOST_TEST
#include "font_vec.c"

static int load_ttf(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    g_ttf_len = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_ttf = (unsigned char*)malloc(g_ttf_len);
    if (fread(g_ttf, 1, g_ttf_len, f) != (size_t)g_ttf_len) {
        fprintf(stderr, "read fail\n"); fclose(f); return -1;
    }
    fclose(f);
    return 0;
}

static int count_coverage(const uint8_t* bmp, int w, int h) {
    int n = 0;
    for (int i = 0; i < w*h; i++) if (bmp[i] > 8) n++;
    return n;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "sfs_files/msyh.ttf";
    if (load_ttf(path) != 0) return 2;
    printf("[TEST] msyh.ttf loaded: %d bytes\n", g_ttf_len);

    int vr = vec_init(host_read_file);
    printf("[TEST] vec_init -> %d (0=ok)\n", vr);
    if (vr != 0) {
        int c, g, s; vec_last_err(&c, &g, &s);
        printf("[TEST] last_err code=%d got=%d stb=%d\n", c, g, s);
        return 3;
    }

    /* scale test: 'A' at 16 and 32 px should produce ~16x16 and ~32x32 */
    int w16,h16,xo16,yo16, w32,h32,xo32,yo32;
    const uint8_t* g16 = vec_glyph('A', 16, &w16, &h16, &xo16, &yo16);
    const uint8_t* g32 = vec_glyph('A', 32, &w32, &h32, &xo32, &yo32);
    printf("[TEST] 'A' px16: %dx%d  px32: %dx%d\n", w16,h16,w32,h32);

    /* CJK test: '中' (U+4E2D) should rasterize to a non-empty bitmap */
    int wc,hc,xoc,yoc;
    const uint8_t* gc = vec_glyph(0x4E2D, 24, &wc, &hc, &xoc, &yoc);
    int cov_c = gc ? count_coverage(gc, wc, hc) : 0;
    printf("[TEST] '中' px24: %dx%d coverage=%d\n", wc, hc, cov_c);

    int cov16 = g16 ? count_coverage(g16, w16, h16) : 0;
    int cov32 = g32 ? count_coverage(g32, w32, h32) : 0;
    printf("[TEST] 'A' coverage px16=%d px32=%d\n", cov16, cov32);

    int ok = 1;
    if (!g16 || !g32) { printf("[FAIL] glyph NULL\n"); ok = 0; }
    if (w32 <= w16)    { printf("[FAIL] no scaling (w32<=w16)\n"); ok = 0; }
    if (cov16 < 10 || cov32 < 10) { printf("[FAIL] latin coverage too low\n"); ok = 0; }
    if (!gc || cov_c < 10)        { printf("[FAIL] CJK glyph empty\n"); ok = 0; }

    printf(ok ? "[PASS] X-stage vector rasterizer works (scaling + CJK OK)\n"
              : "[FAIL] X-stage vector rasterizer issues\n");
    return ok ? 0 : 1;
}
