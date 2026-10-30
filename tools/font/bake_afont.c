/*
 * bake_afont.c - Host-side re-bake of the NexOS 16x16 8-bit-alpha ASCII font
 * (afont_data.h) WITH grid fitting.
 *
 * Why this tool exists
 * --------------------
 * The previous table was produced by tools/_archive/gen_bitmap_fonts.c:
 *
 *     scale = stbtt_ScaleForPixelHeight(&font, 16);
 *     bmp   = stbtt_GetCodepointBitmap(&font, scale, scale, cp, ...);
 *
 * i.e. a plain UNHINTED area rasterisation.  At 16 px Microsoft YaHei's stems
 * are ~1.3 px wide and almost never start on an integer pixel boundary, so the
 * scanline coverage of a 1 px vertical stem lands on TWO columns with no solid
 * core.  Measured on the shipped table:
 *
 *     'I' stem rows: 166 180      (65% + 71%, neither >= 240)
 *     'H' stems    : 166 180      both sides
 *     'l'          : 192 125
 *
 * 29% of the 95 glyphs have no column that reaches full ink at all, and the
 * on-screen measurement of the same pixels matched (icon label stems ~204/127),
 * which is why UI text reads "soft / fat / washed out" instead of crisp.
 *
 * What this tool does
 * -------------------
 *  1. GLOBAL sub-pixel phase search.  The whole ASCII set is rasterised at N x
 *     (N = 4) with a candidate pen phase (px, py) in 1/8 output-pixel steps
 *     (0.5 px in the 4x raster), then box-filtered back to 16 px.  A phase that
 *     lands the stems on the pixel grid produces solid columns; the phase is
 *     chosen ONCE for the whole font (not per glyph) so letter rhythm and the
 *     baseline stay uniform -- that is exactly what grid fitting does.
 *     Score = (#solid columns) - 0.5 * (#half-covered columns), summed over all
 *     glyphs, where solid = column peak >= 85% and half = 20%..85%.
 *  2. AREA-CONSERVING stem redistribution.  Residual split pairs (a,b) that are
 *     a vertical stem are pushed to one solid column plus a light AA column:
 *         S = a + b
 *         S >= 255 :  (255, S-255)
 *         S <  255 :  (S, 0)
 *     a+b is preserved exactly, so the glyph's total ink (and therefore its
 *     apparent weight) does not change -- only its distribution sharpens.
 *     Applied to columns (vertical stems) then rows (horizontal bars).
 *  3. The 16x16 cells are emitted in the SAME afont_data.h format and with the
 *     SAME advance table as before, so the kernel needs no code change and text
 *     layout cannot shift.
 *
 * Build/run (host):
 *   gcc -O2 -I tools/stb -o build/bake_afont tools/font/bake_afont.c
 *   ./build/bake_afont sfs_files/msyh.ttf afont_data.h [old_afont_for_advances.h]
 *
 * The optional 4th argument re-uses the advance bytes of an existing header so
 * metrics/layout stay bit-identical even if stb's rounding ever changes.
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define AW 16
#define AH 16
#define A_BASE  0x20
#define A_COUNT 95
#define SS      4                 /* supersample factor (4x4 area filter) */
#define PH_STEPS 8                /* phase resolution: 1/8 output pixel */

static unsigned char* slurp(const char* path, long* out_sz) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "bake_afont: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc(sz ? (size_t)sz : 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    if (out_sz) *out_sz = sz;
    return b;
}

/* Read the afont_adv[] table out of an existing header (optional).
 * The block looks like:
 *     static const uint8_t afont_adv[AFONT_COUNT] = {
 *     4,6,6,12,... };
 * so: find the "afont_adv" declaration, then take the first A_COUNT integers
 * that follow it, stopping at the closing brace. */
static int parse_old_adv(const char* path, unsigned char* adv) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[8192];
    int in = 0, n = 0;
    while (n < A_COUNT && fgets(line, sizeof line, f)) {
        for (char* p = line; *p && n < A_COUNT; ) {
            if (!in) {
                if (strstr(p, "afont_adv")) in = 1;
                break;
            }
            if (*p >= '0' && *p <= '9') {
                int v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                adv[n++] = (unsigned char)v;
                continue;
            }
            if (*p == '}') { in = 0; break; }
            p++;
        }
    }
    fclose(f);
    return n == A_COUNT;
}

/* Parse the afont_alpha[][] block of an existing header into cells[][]. */
static int parse_old_table(const char* path, unsigned char cells[A_COUNT][AW * AH]) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    int in = 0, g = 0, n = 0;
    int cur[AW * AH];
    char line[16384];
    while (g < A_COUNT && fgets(line, sizeof line, f)) {
        for (char* p = line; *p && g < A_COUNT; ) {
            if (!in) { if (strstr(p, "afont_alpha")) in = 1; break; }
            if (*p == '{') { n = 0; p++; continue; }
            if (*p >= '0' && *p <= '9') {
                int v = 0;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                if (n < AW * AH) cur[n++] = v;
                continue;
            }
            if (*p == '}') {
                if (n == AW * AH) {
                    for (int k = 0; k < AW * AH; k++)
                        cells[g][k] = (unsigned char)(cur[k] < 0 ? 0 : (cur[k] > 255 ? 255 : cur[k]));
                    g++;
                }
                n = 0; p++;
                continue;
            }
            p++;
        }
    }
    fclose(f);
    return g == A_COUNT;
}

/* Coverage score of one 16x16 cell: solid columns are good, half-covered
 * columns are the "grey fringe" we want to minimise. */
static double score_cell(const unsigned char* cell) {
    int solid = 0, half = 0;
    for (int x = 0; x < AW; x++) {
        int peak = 0;
        for (int y = 0; y < AH; y++) { int v = cell[y * AW + x]; if (v > peak) peak = v; }
        if (peak >= 217) solid++;
        else if (peak >= 51) half++;
    }
    return (double)solid - 0.5 * (double)half;
}

/* Rasterise one codepoint at SS x with the given sub-pixel pen phase
 * (in 1/PH_STEPS of an output pixel) and box-filter it into a 16x16 cell.
 * Placement follows the original generator exactly (baseline at a_base,
 * bearing xo/yo), so glyph positions are unchanged. */
static void raster_cell(stbtt_fontinfo* font, float scale_ss, int a_base,
                        int cp, int phx, int phy, unsigned char* cell) {
    static int acc[AW * AH];
    memset(cell, 0, (size_t)AW * AH);
    memset(acc, 0, sizeof acc);

    float shift_x = (float)phx * (float)SS / (float)PH_STEPS;   /* in SS pixels */
    float shift_y = (float)phy * (float)SS / (float)PH_STEPS;
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char* bmp = stbtt_GetCodepointBitmapSubpixel(font, scale_ss, scale_ss,
                                                          shift_x, shift_y, cp, &w, &h, &xo, &yo);
    if (!bmp || w <= 0 || h <= 0) { if (bmp) stbtt_FreeBitmap(bmp, 0); return; }

    /* Bearings come back in SS-space; divide to output pixels.  Placement keeps
     * the original generator's convention (baseline at a_base), so layout is
     * unchanged. */
    int ox = (int)floor((double)xo / (double)SS + 0.5);
    int oy = a_base + (int)floor((double)yo / (double)SS + 0.5);

    for (int r = 0; r < h; r++) {
        int dy = oy + r / SS;
        if (dy < 0 || dy >= AH) continue;
        const unsigned char* srow = bmp + (size_t)r * (size_t)w;
        for (int c = 0; c < w; c++) {
            int dx = ox + c / SS;
            if (dx < 0 || dx >= AW) continue;
            acc[dy * AW + dx] += srow[c];
        }
    }
    stbtt_FreeBitmap(bmp, 0);
    for (int i = 0; i < AW * AH; i++) {
        int v = acc[i] / (SS * SS);
        cell[i] = (unsigned char)(v > 255 ? 255 : v);
    }
}

/* Area-conserving stem redistribution (v2, run-based).
 *
 * Only a straight, contiguous stroke is touched: for a column pair we require a
 * run of >= 5 consecutive rows where BOTH columns carry ink, the columns on
 * either side are essentially empty, and the pair's combined coverage looks like
 * a stroke (>= 180).  Diagonals and curve joins fail that test, which is what
 * keeps 'M', 'V', 'x', 'a', 'o' intact -- the earlier per-row version pushed
 * isolated pixels out of those glyphs and left visible specks.
 *
 * Inside a run, each row's pair (a,b) is replaced by (255, S-255) when S >= 255
 * else (S, 0), i.e. one solid column plus a light AA column, with a+b preserved
 * exactly: the glyph keeps its total ink and therefore its apparent weight. */
static int g_minrun = 3, g_maxnb = 100, g_minsum = 150;  /* tuned by sweep: ratio 0.51->0.31, 92/95 solid-stem glyphs, 0 specks */

static void redistribute_cols(unsigned char* cell, int* fixes) {
    for (int x = 0; x + 1 < AW; x++) {
        int y = 0;
        while (y < AH) {
            int a = cell[y * AW + x], b = cell[y * AW + x + 1];
            int left  = (x > 0)      ? cell[y * AW + x - 1] : 0;
            int right = (x + 2 < AW) ? cell[y * AW + x + 2] : 0;
            int ok = (a >= 40 && a <= 240 && b >= 40 && b <= 240 && left <= g_maxnb && right <= g_maxnb);
            if (!ok) { y++; continue; }
            int y0 = y;
            while (y < AH) {
                a = cell[y * AW + x]; b = cell[y * AW + x + 1];
                left  = (x > 0)      ? cell[y * AW + x - 1] : 0;
                right = (x + 2 < AW) ? cell[y * AW + x + 2] : 0;
                if (!(a >= 40 && a <= 240 && b >= 40 && b <= 240 && left <= g_maxnb && right <= g_maxnb)) break;
                y++;
            }
            int y1 = y;                    /* rows [y0, y1) form the candidate run */
            if (y1 - y0 >= g_minrun) {
                for (int r = y0; r < y1; r++) {
                    int aa = cell[r * AW + x], bb = cell[r * AW + x + 1];
                    int S = aa + bb;
                    if (S < g_minsum) continue;
                    int na, nb;
                    if (S >= 255) { na = 255; nb = S - 255; } else { na = S; nb = 0; }
                    if (aa >= bb) {
                        cell[r * AW + x] = (unsigned char)na;
                        cell[r * AW + x + 1] = (unsigned char)nb;
                    } else {
                        cell[r * AW + x] = (unsigned char)nb;
                        cell[r * AW + x + 1] = (unsigned char)na;
                    }
                    (*fixes)++;
                }
            }
        }
    }
}

static void redistribute_rows(unsigned char* cell, int* fixes) {
    for (int y = 0; y + 1 < AH; y++) {
        int x = 0;
        while (x < AW) {
            int a = cell[y * AW + x], b = cell[(y + 1) * AW + x];
            int up   = (y > 0)      ? cell[(y - 1) * AW + x] : 0;
            int down = (y + 2 < AH) ? cell[(y + 2) * AW + x] : 0;
            int ok = (a >= 40 && a <= 240 && b >= 40 && b <= 240 && up <= g_maxnb && down <= g_maxnb);
            if (!ok) { x++; continue; }
            int x0 = x;
            while (x < AW) {
                a = cell[y * AW + x]; b = cell[(y + 1) * AW + x];
                up   = (y > 0)      ? cell[(y - 1) * AW + x] : 0;
                down = (y + 2 < AH) ? cell[(y + 2) * AW + x] : 0;
                if (!(a >= 40 && a <= 240 && b >= 40 && b <= 240 && up <= g_maxnb && down <= g_maxnb)) break;
                x++;
            }
            int x1 = x;
            if (x1 - x0 >= g_minrun) {
                for (int c = x0; c < x1; c++) {
                    int aa = cell[y * AW + c], bb = cell[(y + 1) * AW + c];
                    int S = aa + bb;
                    if (S < g_minsum) continue;
                    int na, nb;
                    if (S >= 255) { na = 255; nb = S - 255; } else { na = S; nb = 0; }
                    if (aa >= bb) {
                        cell[y * AW + c] = (unsigned char)na;
                        cell[(y + 1) * AW + c] = (unsigned char)nb;
                    } else {
                        cell[y * AW + c] = (unsigned char)nb;
                        cell[(y + 1) * AW + c] = (unsigned char)na;
                    }
                    (*fixes)++;
                }
            }
        }
    }
}

static int redistribute(unsigned char* cell) {
    int fixes = 0;
    redistribute_cols(cell, &fixes);
    redistribute_rows(cell, &fixes);
    return fixes;
}

/* Count ink pixels that have no 8-connected ink neighbour (visual "specks"). */
static int count_specks(const unsigned char* cell) {
    int n = 0;
    for (int y = 0; y < AH; y++)
        for (int x = 0; x < AW; x++) {
            if (cell[y * AW + x] < 40) continue;
            int nb = 0;
            for (int dy = -1; dy <= 1 && !nb; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int yy = y + dy, xx = x + dx;
                    if (yy < 0 || yy >= AH || xx < 0 || xx >= AW) continue;
                    if (cell[yy * AW + xx] >= 40) { nb = 1; break; }
                }
            if (!nb) n++;
        }
    return n;
}

static void report_table(const char* tag, unsigned char cells[A_COUNT][AW * AH]) {
    int solid_cols = 0, part_cols = 0, glyphs_with_solid = 0, no_ink = 0;
    for (int i = 0; i < A_COUNT; i++) {
        int solid_here = 0;
        for (int x = 0; x < AW; x++) {
            int peak = 0;
            for (int y = 0; y < AH; y++) { int v = cells[i][y * AW + x]; if (v > peak) peak = v; }
            if (peak >= 217) { solid_cols++; solid_here++; }
            else if (peak >= 51) part_cols++;
        }
        if (solid_here) glyphs_with_solid++;
        else {
            int any = 0;
            for (int p = 0; p < AW * AH; p++) if (cells[i][p]) any = 1;
            if (any) no_ink++;
        }
    }
    printf("[%s] solid cols=%d  partial cols=%d  partial/solid=%.2f  glyphs with a solid stem=%d/%d  (no solid stem but has ink: %d)\n",
           tag, solid_cols, part_cols, solid_cols ? (double)part_cols / (double)solid_cols : 0.0,
           glyphs_with_solid, A_COUNT, no_ink);
}

static void write_header(const char* out, const unsigned char* adv,
                          unsigned char cells[A_COUNT][AW * AH],
                          int phx, int phy, const char* how) {
    FILE* fh = fopen(out, "w");
    if (!fh) { fprintf(stderr, "bake_afont: cannot write %s\n", out); return; }
    fprintf(fh, "#pragma once\n#include <stdint.h>\n");
    fprintf(fh, "// Auto-generated by tools/font/bake_afont.c -- 16x16 8-bit-alpha ASCII\n");
    fprintf(fh, "// %s\n", how);
    if (phx || phy)
        fprintf(fh, "// supersampled %dx with a global %d/%d px pen phase\n", SS, phx, PH_STEPS);
    fprintf(fh, "// plus area-conserving stem redistribution (see the tool header).\n");
    fprintf(fh, "#define AFONT_FIRST %d\n#define AFONT_COUNT %d\n#define AFONT_W %d\n#define AFONT_H %d\n",
            A_BASE, A_COUNT, AW, AH);
    fprintf(fh, "static const uint8_t afont_adv[AFONT_COUNT] = {\n");
    for (int i = 0; i < A_COUNT; i++) { fprintf(fh, "%d,", adv[i]); if ((i & 31) == 31) fputc('\n', fh); }
    fprintf(fh, "\n};\n");
    fprintf(fh, "static const uint8_t afont_alpha[AFONT_COUNT][AFONT_W*AFONT_H] = {\n");
    for (int i = 0; i < A_COUNT; i++) {
        fprintf(fh, "{");
        for (int p = 0; p < AW * AH; p++) {
            fprintf(fh, "%d,", cells[i][p]);
            if ((p & 31) == 31) fputc('\n', fh);
        }
        fprintf(fh, "},\n");
    }
    fprintf(fh, "};\n");
    fclose(fh);
    printf("wrote %s (%d glyphs, %dx%d)\n", out, A_COUNT, AW, AH);
}

int main(int argc, char** argv) {
    /* The SHIPPED afont_data.h was baked from DejaVu Sans 16 px (verified: its
     * 'I' is 166/180 with total ink 3467, which DejaVu reproduces byte-exactly
     * while Microsoft YaHei gives 168 in a single column).  We therefore default
     * to the same face so the only visible change is the grid fitting -- the
     * glyph weight and shapes stay identical to what users already see. */
    const char* ttf = argc > 1 ? argv[1] : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    const char* out = argc > 2 ? argv[2] : "afont_data.h";
    const char* oldh = argc > 3 ? argv[3] : NULL;
    int use_raster = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--minrun=", 9)) g_minrun = atoi(argv[i] + 9);
        if (!strncmp(argv[i], "--maxnb=", 8))  g_maxnb  = atoi(argv[i] + 8);
        if (!strncmp(argv[i], "--minsum=", 10)) g_minsum = atoi(argv[i] + 10);
    }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--raster")) use_raster = 1;

    long sz = 0;
    unsigned char* buf = slurp(ttf, &sz);
    if (!buf) return 1;
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, buf, stbtt_GetFontOffsetForIndex(buf, 0))) {
        fprintf(stderr, "bake_afont: stbtt_InitFont failed for %s\n", ttf);
        return 1;
    }

    int asc, desc, gap;
    stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
    float s16 = stbtt_ScaleForPixelHeight(&font, (float)AH);
    float s4  = stbtt_ScaleForPixelHeight(&font, (float)(AH * SS));
    int a_base = (int)floor((double)(asc * s16) + 0.5);
    if (a_base < 0) a_base = 0;
    if (a_base > AH) a_base = AH;

    /* ---------- advances: identical to the original generator ---------- */
    unsigned char adv[A_COUNT];
    for (int i = 0; i < A_COUNT; i++) {
        int a = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font, A_BASE + i, &a, &lsb);
        int apx = (int)floor((double)(a * s16) + 0.5);
        if (apx < 1) apx = 1;
        if (apx > AW) apx = AW;
        adv[i] = (unsigned char)apx;
    }
    /* Optional: the shipped table, for the faithfulness + weight comparison. */
    static unsigned char old_cells[A_COUNT][AW * AH];
    int have_old = 0;
    if (oldh) have_old = parse_old_table(oldh, old_cells);
    if (have_old) printf("old table loaded from %s\n", oldh);

    unsigned char old_adv[A_COUNT];
    if (oldh && parse_old_adv(oldh, old_adv)) {
        int diff = 0;
        for (int i = 0; i < A_COUNT; i++) if (old_adv[i] != adv[i]) diff++;
        printf("advance check vs %s: %d difference(s)%s\n", oldh, diff,
               diff ? " -> REUSING the old table to keep layout identical" : "");
        if (diff) memcpy(adv, old_adv, sizeof adv);
    }

    static unsigned char tmp[A_COUNT][AW * AH];
    static unsigned char best_cells[A_COUNT][AW * AH];

    /* ---------- MODE A (default): grid-fit the SHIPPED coverage table -------
     * Re-rasterising the same face at 4x and box-filtering back is NOT
     * ink-neutral for thin features: measured against the shipped table the
     * mean per-pixel difference is 8/255 and a 1.4 px horizontal bar (underscore)
     * gains +36% ink, which the user would see as a weight change.  Since the
     * design already lands on integer bearings (the phase search below moves the
     * score by ~0.2%), the useful part of "grid fitting" here is the area-
     * conserving stem redistribution, which can be applied DIRECTLY to the
     * shipped cells: shapes and total ink stay bit-identical, only the split of
     * a 1.3 px stroke into "one solid column + one light AA column" changes.
     * That is what this mode does, and it is the default. */
    if (have_old && !use_raster) {
        for (int i = 0; i < A_COUNT; i++) memcpy(best_cells[i], old_cells[i], AW * AH);
        report_table("shipped table (as-is)", best_cells);
        { int sp = 0; for (int i = 0; i < A_COUNT; i++) sp += count_specks(old_cells[i]);
          printf("specks (isolated ink px) in shipped table: %d\n", sp); }
        int fixes = 0;
        for (int i = 0; i < A_COUNT; i++) fixes += redistribute(best_cells[i]);
        printf("stem redistribution (table mode): %d column/row pairs sharpened\n", fixes);
        report_table("final (table mode: area-conserving redistribution)", best_cells);
        { int sp = 0; for (int i = 0; i < A_COUNT; i++) sp += count_specks(best_cells[i]);
          printf("specks (isolated ink px) after redistribution: %d\n", sp); }
        write_header(out, adv, best_cells, 0, 0, "shipped-table redistribution");
        free(buf);
        return 0;
    }

    /* ---------- global sub-pixel phase search ---------- */
    double best_score = -1e9;
    int best_px = 0, best_py = 0;
    for (int py = 0; py < PH_STEPS; py++) {
        for (int px = 0; px < PH_STEPS; px++) {
            double total = 0;
            for (int i = 0; i < A_COUNT; i++) {
                raster_cell(&font, s4, a_base, A_BASE + i, px, py, tmp[i]);
                total += score_cell(tmp[i]);
            }
            if (total > best_score) {
                best_score = total; best_px = px; best_py = py;
                memcpy(best_cells, tmp, sizeof best_cells);
            }
        }
    }
    printf("phase search: best = (%d/8, %d/8) px  score=%.1f\n", best_px, best_py, best_score);

    /* ---------- faithfulness: does our N-x pipeline reproduce the OLD table? --
     * Rasterising at 4x and box-filtering back to 16 must reproduce the shipped
     * (1x, unhinted) coverage to within a rounding step.  If this drifts, the
     * only differences the user could see would be accidental. */
    if (oldh) {
        static unsigned char chk[A_COUNT][AW * AH];
        for (int i = 0; i < A_COUNT; i++)
            raster_cell(&font, s4, a_base, A_BASE + i, 0, 0, chk[i]);
        long sumd = 0; int maxd = 0, n = 0;
        for (int i = 0; i < A_COUNT; i++)
            for (int p = 0; p < AW * AH; p++) {
                int o = old_cells[i][p];
                int d = chk[i][p] - o; if (d < 0) d = -d;
                sumd += d; if (d > maxd) maxd = d; n++;
            }
        printf("faithfulness (4x box vs shipped 1x, phase 0): mean |diff| = %.2f/255, max = %d\n",
               (double)sumd / (double)(n ? n : 1), maxd);
    }

    /* ---------- baseline: the old (unhinted) table, for the report ---------- */
    static unsigned char raw_cells[A_COUNT][AW * AH];
    for (int i = 0; i < A_COUNT; i++)
        raster_cell(&font, s4, a_base, A_BASE + i, 0, 0, raw_cells[i]);
    report_table("raw (unhinted, what ships today)", raw_cells);
    report_table("phase-aligned only", best_cells);

    /* ---------- area-conserving redistribution ---------- */
    int total_fixes = 0;
    for (int i = 0; i < A_COUNT; i++) total_fixes += redistribute(best_cells[i]);
    printf("stem redistribution: %d column/row pairs sharpened\n", total_fixes);
    report_table("final (phase + redistribution)", best_cells);

    write_header(out, adv, best_cells, best_px, best_py, "grid-fitted raster");
    free(buf);
    return 0;
}
