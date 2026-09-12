// =====================================================================
//  bootsplash.cpp  -  earliest-possible boot animation (32-bit kernel)
// ---------------------------------------------------------------------
//  A self-contained animated boot screen ("NexOS" wordmark + orbiting
//  dots + progress bar + stage label) painted straight into the linear
//  framebuffer.
//
//  Why here and not in gui.cpp: this must run BEFORE the GUI subsystem
//  exists -- before the heap, before SFS is mounted, before the fonts are
//  loaded.  It therefore owns its own framebuffer geometry (read from the
//  VbeInfo block at 0x5000), draws with integer math only, allocates
//  nothing, and is callable from the very first drawable moment in kmain.
//
//  The only thing it borrows is the 8x16 glyph bitmap, via a one-line
//  accessor exported by gui.cpp (the array has internal linkage there).
// =====================================================================
#include "bootsplash.h"

// ---------------------------------------------------------------------
//  Freestanding primitives: no libc, no libm, no allocation
// ---------------------------------------------------------------------
static inline void sp_outb(unsigned short port, unsigned char v) {
    __asm__ __volatile__("outb %0, %1" :: "a"(v), "Nd"(port));
}
static void sp_serial(const char* s) {
    while (*s) sp_outb(0x3F8, (unsigned char)*s++);
}
static void sp_serial_u32(unsigned int v) {
    char b[12];
    int n = 0;
    if (v == 0) b[n++] = '0';
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0) sp_outb(0x3F8, (unsigned char)b[--n]);
}
// Low 32 bits of the TSC.  It wraps every ~1.7 s at 2.5 GHz, but only
// differences are used and the frame interval is far below that, so the
// unsigned subtraction paces the animation correctly.
static inline unsigned int sp_rdtsc(void) {
    unsigned int lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo) : : "edx");
    return lo;
}
// ~5.6 ms at 2.5 GHz / ~3.5 ms at 4 GHz: a floor on the frame interval, so the
// spinner is redrawn as often as the boot code offers a chance but never more
// than ~180 fps.  A frame only repaints the small dot ring, so this is cheap.
#define SP_FRAME_TSC 14000000u

// ---------------------------------------------------------------------
//  Borrowed pieces from gui.cpp
//  - nexos_glyph8x16: the CP437 8x16 glyph rows (internal linkage there).
//  - nexos_boot_splash_own: tells the fb text console to stay quiet.
//    Both live in gui.cpp so the shared gui.cpp never references a symbol
//    that the 64-bit kernel would have to supply.
// ---------------------------------------------------------------------
extern "C" const unsigned char* nexos_glyph8x16(int ch);
extern "C" void nexos_boot_splash_own(int on);
// Optional idle callback pumped by net.cpp's synchronous HTTP loop.
extern "C" void (*g_net_idle_hook)(void);

// ---------------------------------------------------------------------
//  VbeInfo scratch block at 0x5000 (mirrors gui.cpp's struct exactly)
// ---------------------------------------------------------------------
#define VBE_ADDR          0x5000u
#define VBE_FB_PHYS_OFF   0x00   // u32
#define VBE_WIDTH_OFF     0x04   // u16
#define VBE_HEIGHT_OFF    0x06   // u16
#define VBE_BPP_OFF       0x08   // u8
#define VBE_PITCH_OFF     0x09   // u16
#define VBE_OK_OFF        0x0D   // u8
#define VBE_MODE_SET_OFF  0x0E   // u8
#define VBE_PXF_OFF       0x0F   // u8
#define PXF_BLT_ONLY      4

// ---------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------
static unsigned char* sp_fb      = 0;   // LFB as the 32-bit kernel sees it
static unsigned int   sp_pitch   = 0;
static unsigned int   sp_w       = 0;
static unsigned int   sp_h       = 0;
static unsigned int   sp_bpp     = 32;
static unsigned int   sp_pxf     = 0;
static int            sp_active  = 0;

static int            sp_prog    = 0;   // 0..100, never decreases
static int            sp_phase   = 0;   // spinner step
static unsigned int   sp_last    = 0;
static const char*    sp_label   = "Starting";
static unsigned int   sp_frames  = 0;   // frames painted (reported at hand-off)
static short          sp_dot_x[12], sp_dot_y[12];  // dot centres drawn last frame
static int            sp_dot_valid = 0;

// Cadence probes.  The measured cost of a frame is small (~60 us), yet the
// animation still looks stuttery -- because it is driven by BOOT PROGRESS, not
// by time.  Between two boot_splash_tick() calls there is simply no chance to
// draw, and the boot code can sit inside one long operation for a second or
// more.  These record the longest such freeze and WHICH phase it spanned, so
// the next fix goes where the time actually goes instead of everywhere.
static unsigned int   sp_ticks     = 0;   // tick() calls offered by the boot
static unsigned int   sp_gap_max   = 0;   // longest interval between frames
static const char*    sp_gap_where = "?"; //   ... and the phase it spanned
static const char*    sp_prev_lab  = "Starting";

// Called immediately BEFORE each frame is drawn.  The gap is attributed to the
// phase we are leaving (sp_prev_lab), because by the time a milestone renders
// the label already names the phase we are entering.
static void sp_frame_begin(void) {
    unsigned int now = sp_rdtsc();
    unsigned int dt  = (unsigned int)(now - sp_last);
    if (dt > sp_gap_max) { sp_gap_max = dt; sp_gap_where = sp_prev_lab; }
    sp_last     = now;
    sp_prev_lab = sp_label;
}
// Render-cost probes (TSC cycles).  Reported at hand-off so the paint path can
// be measured instead of guessed at.
static unsigned int   sp_cy_static = 0; // cycles in the static layer (bg+logo+info)
static unsigned int   sp_cy_ring   = 0; // cycles in the animated frames
static unsigned int   sp_cy_bg     = 0; // ... split by phase, to find the hot one
static unsigned int   sp_cy_glow   = 0;
static unsigned int   sp_cy_logo   = 0;
static unsigned int   sp_cy_info   = 0;

// Only the label, the bar and the percentage change from one milestone to the
// next -- the background, the glow and the wordmark never do.  Recording their
// rectangles lets a milestone repaint a few thousand pixels instead of the
// whole 1280x720 screen (~920k): on the 32-bit kernel every LFB write is real
// work, and a full-screen repaint per milestone was the dominant cost of the
// boot animation (and the cause of the visible hitch at each step).
struct SpRect { int x, y, w, h; };
static SpRect sp_r_label = {0, 0, 0, 0};
static SpRect sp_r_bar   = {0, 0, 0, 0};
static SpRect sp_r_pct   = {0, 0, 0, 0};
static int    sp_r_valid = 0;

// ---------------------------------------------------------------------
//  Palette (0xRRGGBB; pack() converts to the framebuffer's byte order)
// ---------------------------------------------------------------------
#define CL_BG_TOP   0x060A12u
#define CL_BG_BOT   0x0E1A2Eu
#define CL_GLOW     0x1E4A8Cu
#define CL_LOGO     0xEAF2FFu
#define CL_LOGO_SHD 0x081020u
#define CL_ACCENT   0x4FA3FFu
#define CL_LABEL    0x8FA6C4u
#define CL_TRACK    0x16233Au
#define CL_RINGOFF  0x1B2E4Au

// Blend a -> b by t/256.  A multiply-shift instead of three integer divisions:
// the glow samples this ~37k times per paint, and an emulated `div` costs
// 20-40 TCG operations, so this was the single hottest arithmetic in the path.
static inline unsigned int mix_rgb(unsigned int a, unsigned int b, int t) {
    if (t < 0) t = 0; else if (t > 255) t = 255;
    int ar = (int)((a >> 16) & 0xFFu), ag = (int)((a >> 8) & 0xFFu), ab = (int)(a & 0xFFu);
    int br = (int)((b >> 16) & 0xFFu), bg = (int)((b >> 8) & 0xFFu), bb = (int)(b & 0xFFu);
    int r  = ar + (((br - ar) * t) >> 8);
    int g  = ag + (((bg - ag) * t) >> 8);
    int bl = ab + (((bb - ab) * t) >> 8);
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)bl;
}

// Convert an 0xRRGGBB colour into the framebuffer's native encoding.
static inline unsigned int pack(unsigned int rgb) {
    unsigned int r = (rgb >> 16) & 0xFFu, g = (rgb >> 8) & 0xFFu, b = rgb & 0xFFu;
    if (sp_bpp == 16) return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    if (sp_pxf == 1)  return (b << 16) | (g << 8) | r;   // RGBX32 / RGB24: swap R/B
    return (r << 16) | (g << 8) | b;                     // BGRX32 / BGR24
}

// ---------------------------------------------------------------------
//  Cached colours
//
//  The paint path used to recompute everything it drew: the gradient lerp ran
//  three integer divisions PER SCANLINE (thousands of rows per erase, tens of
//  thousands of glow cells), and each primitive re-ran pack() on its argument.
//  Caching the packed gradient per scanline and pre-packing the palette moves
//  all of that out of the loops.
// ---------------------------------------------------------------------
#define SP_ROW_MAX 8192
// Gradient as 0xRRGGBB per scanline -- deliberately NOT pre-packed.  Blending
// (the glow) needs a real colour, and a packed value is only 0xRRGGBB at
// 32 bpp; at 16 bpp it is an RGB565 word, so feeding it to mix_rgb() reads
// garbage in all three channels (that mistake flattened the glow into a
// saturated disc).  Fills pack it on use, which is one cheap shift/mask per
// scanline rather than per pixel.
static unsigned int sp_rowrgb[SP_ROW_MAX];
static int          sp_rowc_n  = 0;

static void build_row_cache(void) {
    if (sp_rowc_n) return;
    int n = (int)sp_h; if (n > SP_ROW_MAX) n = SP_ROW_MAX;
    sp_rowc_n = n;
    int d = n - 1; if (d < 1) d = 1;
    unsigned int r0 = (CL_BG_TOP >> 16) & 0xFFu, g0 = (CL_BG_TOP >> 8) & 0xFFu, b0 = CL_BG_TOP & 0xFFu;
    unsigned int r1 = (CL_BG_BOT >> 16) & 0xFFu, g1 = (CL_BG_BOT >> 8) & 0xFFu, b1 = CL_BG_BOT & 0xFFu;
    // 16.16 fixed-point increments: three divisions for the whole screen
    // instead of three per scanline.
    int ir = (int)(((long long)r1 - (long long)r0) << 16) / d;
    int ig = (int)(((long long)g1 - (long long)g0) << 16) / d;
    int ib = (int)(((long long)b1 - (long long)b0) << 16) / d;
    int cr = (int)r0 << 16, cg = (int)g0 << 16, cb = (int)b0 << 16;
    for (int y = 0; y < n; y++) {
        sp_rowrgb[y] = ((unsigned int)(cr >> 16) << 16)
                     | ((unsigned int)(cg >> 16) << 8)
                     |  (unsigned int)(cb >> 16);
        cr += ir; cg += ig; cb += ib;
    }
}

// Gradient colour for scanline y, as 0xRRGGBB (clamped to the screen).
static inline unsigned int row_rgb(int y) {
    if (y < 0) y = 0;
    if (y >= sp_rowc_n) y = sp_rowc_n - 1;
    if (y < 0) y = 0;
    return sp_rowrgb[y];
}
// Same thing, in the framebuffer's native encoding (for fills).
static inline unsigned int row_p(int y) { return pack(row_rgb(y)); }

// Pre-packed palette, built once the pixel format is known.
static unsigned int sp_c_track, sp_c_accent, sp_c_head, sp_c_label,
                    sp_c_logo, sp_c_logo_shd, sp_c_ringoff, sp_c_white;
static void build_palette(void) {
    sp_c_track     = pack(CL_TRACK);
    sp_c_accent    = pack(CL_ACCENT);
    sp_c_head      = pack(0xBFDCFFu);
    sp_c_label     = pack(CL_LABEL);
    sp_c_logo      = pack(CL_LOGO);
    sp_c_logo_shd  = pack(CL_LOGO_SHD);
    sp_c_ringoff   = pack(CL_RINGOFF);
    sp_c_white     = pack(0xFFFFFFu);
}

// Packed colour of each of the 12 spinner phases, so a frame does no maths.
static unsigned int sp_ring_col[12];
static void build_ring_colors(void) {
    for (int d = 0; d < 12; d++) {
        unsigned int c = (d == 0) ? 0xFFFFFFu : mix_rgb(CL_RINGOFF, CL_ACCENT, 255 - d * 19);
        sp_ring_col[d] = pack(c);
    }
}

// ---------------------------------------------------------------------
//  Primitive drawing
//
//  Every primitive takes an ALREADY-PACKED colour and writes through plain
//  (non-volatile) pointers.  Both matter, and both were measured:
//    * `volatile` forced exactly one 4-byte store per pixel and prevented GCC
//      from widening the loops.  The linear framebuffer is ordinary memory, so
//      letting the compiler emit wide stores is legal and much cheaper under
//      TCG, which charges per emulated memory operation.
//    * packing inside the per-row helper meant the glow re-packed the same
//      colour ~74k times per paint.
// ---------------------------------------------------------------------
static void hline_p(int x0, int x1, int y, unsigned int c) {
    if ((unsigned)y >= sp_h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)sp_w - 1) x1 = (int)sp_w - 1;
    if (x1 < x0) return;
    unsigned char* p = sp_fb + (unsigned int)y * sp_pitch;
    if (sp_bpp == 32) {
        unsigned int* q = (unsigned int*)p;
        for (int x = x0; x <= x1; x++) q[x] = c;
    } else if (sp_bpp == 24) {
        for (int x = x0; x <= x1; x++) {
            unsigned char* q = p + x * 3;
            q[0] = (unsigned char)c;
            q[1] = (unsigned char)(c >> 8);
            q[2] = (unsigned char)(c >> 16);
        }
    } else {
        unsigned short* q = (unsigned short*)p;
        for (int x = x0; x <= x1; x++) q[x] = (unsigned short)c;
    }
}

// Write a 2x2 block in a packed colour.  Used by the glow, which used to make
// two full hline_p calls (with their bounds checks, bpp switch and loop setup)
// for every one of ~37k cells.
static inline void put2x2(int x, int y, unsigned int c) {
    if ((unsigned)x >= sp_w || (unsigned)y >= sp_h) return;
    unsigned char* p = sp_fb + (unsigned int)y * sp_pitch;
    int ok2 = (x + 1) < (int)sp_w;
    int ok_y2 = (y + 1) < (int)sp_h;
    if (sp_bpp == 32) {
        unsigned int* q = (unsigned int*)p;
        q[x] = c;
        if (ok2) q[x + 1] = c;
        if (ok_y2) {
            unsigned int* q2 = (unsigned int*)(p + sp_pitch);
            q2[x] = c;
            if (ok2) q2[x + 1] = c;
        }
    } else if (sp_bpp == 24) {
        for (int j = 0; j < 2; j++) {
            if (j && !ok_y2) break;
            unsigned char* q = p + (unsigned int)j * sp_pitch;
            q[x * 3]     = (unsigned char)c;
            q[x * 3 + 1] = (unsigned char)(c >> 8);
            q[x * 3 + 2] = (unsigned char)(c >> 16);
            if (ok2) {
                q[x * 3 + 3] = (unsigned char)c;
                q[x * 3 + 4] = (unsigned char)(c >> 8);
                q[x * 3 + 5] = (unsigned char)(c >> 16);
            }
        }
    } else {
        for (int j = 0; j < 2; j++) {
            if (j && !ok_y2) break;
            unsigned short* q = (unsigned short*)(p + (unsigned int)j * sp_pitch);
            q[x] = (unsigned short)c;
            if (ok2) q[x + 1] = (unsigned short)c;
        }
    }
}

static void fill_rect_p(int x, int y, int w, int h, unsigned int c) {
    for (int j = 0; j < h; j++) hline_p(x, x + w - 1, y + j, c);
}

static void fill_circle_p(int cx, int cy, int r, unsigned int c) {
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int span = 0;
        while ((span + 1) * (span + 1) + dy * dy <= r * r) span++;
        hline_p(cx - span, cx + span, cy + dy, c);
    }
}

// ---------------------------------------------------------------------
//  Text (scaled 8x16 glyphs)
// ---------------------------------------------------------------------
static int text_w(const char* s, int scale) {
    int n = 0; while (s[n]) n++;
    return n * 8 * scale;
}

static void draw_text(const char* s, int x, int y, int scale, unsigned int c) {
    // Pack once, then emit one span per RUN of set pixels rather than one call
    // per lit pixel: at scale 4 the original ran four span fills -- and four
    // pack() calls -- for every single dot of the wordmark.
    if (scale < 1) scale = 1;
    for (; *s; s++) {
        const unsigned char* g = nexos_glyph8x16((unsigned char)*s);
        if (g) {
            for (int row = 0; row < 16; row++) {
                unsigned char bits = g[row];
                if (!bits) continue;
                int col = 0;
                while (col < 8) {
                    if (!(bits & (0x80u >> col))) { col++; continue; }
                    int run = col;
                    while (run < 8 && (bits & (0x80u >> run))) run++;
                    int px = x + col * scale;
                    int pw = (run - col) * scale;
                    if (scale == 1) {
                        hline_p(px, px + pw - 1, y + row, c);
                    } else {
                        for (int j = 0; j < scale; j++)
                            hline_p(px, px + pw - 1, y + row * scale + j, c);
                    }
                    col = run;
                }
            }
        }
        x += 8 * scale;
    }
}

static void draw_text_centered(const char* s, int cy, int scale, unsigned int rgb) {
    draw_text(s, ((int)sp_w - text_w(s, scale)) / 2, cy, scale, rgb);
}

static void draw_uint_centered(unsigned int v, int cy, int scale, unsigned int rgb) {
    char b[12];
    int n = 0;
    if (v == 0) b[n++] = '0';
    while (v && n < 11) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    char out[14];
    int o = 0;
    while (n > 0) out[o++] = b[--n];
    out[o] = 0;
    draw_text_centered(out, cy, scale, rgb);
}

// ---------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------
static int logo_scale(void) {
    int s = (int)sp_w / 280;
    if (s < 2) s = 2;
    if (s > 6) s = 6;
    return s;
}
static int logo_y(void)   { return (int)sp_h * 38 / 100; }
static int logo_cx(void)  { return (int)sp_w / 2; }
// The glow is centred on the wordmark and ends well above the ring, so
// repainting the ring's little box with the plain gradient leaves no seam.
static int glow_cy(void)  { return logo_y() + 8 * logo_scale(); }
static int ring_cy(void)  { return logo_y() + 16 * logo_scale() + 96; }
static int ring_r(void)   { return 14 + logo_scale() * 2; }
static int bar_y(void)    { return (int)sp_h - (int)sp_h / 6; }

// 12 ring positions, unit circle x1000 (no libm).
static const short RING_TAB[12][2] = {
    { 1000,    0}, {  866,  500}, {  500,  866}, {    0, 1000},
    { -500,  866}, { -866,  500}, {-1000,    0}, { -866, -500},
    { -500, -866}, {    0,-1000}, {  500, -866}, {  866, -500}
};

// ---------------------------------------------------------------------
//  Painting
// ---------------------------------------------------------------------
static void draw_gradient(void) {
    // One cached colour per scanline; packed per row, not per pixel.
    for (unsigned int y = 0; y < sp_h; y++) hline_p(0, (int)sp_w - 1, (int)y, row_p((int)y));
}

static void draw_glow(void) {
    // Soft elliptical glow behind the wordmark, on a 2x2 grid.
    //
    // This used to be 94% of the entire static paint (452M of 481M cycles) for
    // ~46k cells: every cell ran TWO 64-bit divisions, and in this -m32
    // -nostdlib kernel each of those is a call into the software 64/64-bit
    // division helper (~12k cycles/cell once TCG has emulated the shift-
    // subtract loop).  There is no division left:
    //   * the ellipse test is cross-multiplied (dx^2*r2 + dy^2*rxx < r2*rxx),
    //     which needs only 64-bit multiplies and adds;
    //   * the falloff t = (den-num)/den * 110 comes from a reciprocal computed
    //     once per paint, i.e. a multiply and two shifts.
    // NOTE: dx*dx*r2 reaches ~2.2e9, which overflows a 32-bit long, so the
    // intermediates must stay 64-bit or the ellipse test goes wrong and leaves
    // stray vertical bands.
    int gx = logo_cx(), gy = glow_cy();
    int rx = (int)sp_w / 3, ry = 110;
    if (rx < 1) rx = 1;

    const unsigned long long r2  = (unsigned long long)ry * (unsigned long long)ry;
    const unsigned long long rxx = (unsigned long long)rx * (unsigned long long)rx;
    const unsigned long long den = r2 * rxx;
    const unsigned long long recip = (1ULL << 48) / den;   // one division per paint

    for (int y = gy - ry; y <= gy + ry; y += 2) {
        int dy = y - gy;
        long long dy2 = (long long)dy * dy;
        if ((unsigned long long)dy2 * rxx >= den) continue;   // whole row outside
        unsigned int base = row_rgb(y);        // 0xRRGGBB: mix_rgb needs a real
                                               // colour, not a packed one
        for (int x = gx - rx; x <= gx + rx; x += 2) {
            long long dx = x - gx;
            long long sum = (long long)dx * dx * (long long)r2
                          + (long long)dy2 * (long long)rxx;
            long long num = (long long)den - sum;
            if (num <= 0) continue;                            // outside the ellipse
            // q16 = num/den in 0..65536, then t = q16*110 >> 16.
            unsigned int q16 = (unsigned int)(((unsigned long long)num * recip) >> 32);
            if (q16 > 65536u) q16 = 65536u;
            int t = (int)((q16 * 110u) >> 16);
            put2x2(x, y, pack(mix_rgb(base, CL_GLOW, t)));
        }
    }
}

static void draw_logo(void) {
    const char* s = "NexOS";
    int scale = logo_scale();
    int x = ((int)sp_w - text_w(s, scale)) / 2;
    int y = logo_y();
    draw_text(s, x + scale, y + scale, scale, sp_c_logo_shd);   // drop shadow
    draw_text(s, x, y, scale, sp_c_logo);
}

// Erase one recorded band back to the plain gradient (the glow does not reach
// the label/bar bands, so the gradient alone matches the untouched background).
static void clear_band(const SpRect& r) {
    if (r.w <= 0 || r.h <= 0) return;
    // Colour straight from the scanline cache: the original recomputed a
    // three-division lerp for every row of every band on every milestone.
    for (int j = 0; j < r.h; j++) {
        int y = r.y + j;
        hline_p(r.x, r.x + r.w - 1, y, row_p(y));
    }
}
static void erase_info(void) {
    if (!sp_r_valid) return;
    clear_band(sp_r_label);
    clear_band(sp_r_bar);
    clear_band(sp_r_pct);
    sp_r_valid = 0;
}

static void draw_label(void) {
    int scale = (sp_w >= 900) ? 2 : 1;
    int tw = text_w(sp_label, scale);
    int x  = ((int)sp_w - tw) / 2;
    int y  = ring_cy() + ring_r() + 30;
    sp_r_label.x = x - 2;      sp_r_label.y = y - 2;
    sp_r_label.w = tw + 4;     sp_r_label.h = 16 * scale + 4;
    draw_text_centered(sp_label, y, scale, sp_c_label);
}

static void draw_progress(void) {
    int bw = (int)sp_w / 3;
    if (bw < 120) bw = 120;
    int bh = (int)sp_h / 140;
    if (bh < 4) bh = 4;
    if (bh > 10) bh = 10;
    int bx = ((int)sp_w - bw) / 2;
    int by = bar_y();
    sp_r_bar.x = bx - 2;  sp_r_bar.y = by - 2;
    sp_r_bar.w = bw + 4;  sp_r_bar.h = bh + 4;
    fill_rect_p(bx, by, bw, bh, sp_c_track);
    int fw = bw * sp_prog / 100;
    if (fw > 0) fill_rect_p(bx, by, fw, bh, sp_c_accent);
    if (fw > 2) fill_rect_p(bx + fw - 2, by, 2, bh, sp_c_head);   // bright head

    // Percentage under the bar so the progress is obviously alive.  The wide
    // fixed rectangle covers any 1..3-digit value in scale-1 glyphs.
    int py = by + bh + 12;
    sp_r_pct.x = ((int)sp_w - 48) / 2;  sp_r_pct.y = py - 2;
    sp_r_pct.w = 48;                    sp_r_pct.h = 20;
    draw_uint_centered((unsigned int)sp_prog, py, 1, sp_c_label);
}

// The parts that change from milestone to milestone.
static void draw_info(void) {
    draw_label();
    draw_progress();
    sp_r_valid = 1;
}

// Full paint: background + glow + wordmark + info.  Runs once at startup.
// Each phase is timed separately -- guessing which one dominates is exactly
// how the first optimisation round made things slower.
static void draw_static(void) {
    unsigned int t = sp_rdtsc();
    draw_gradient();
    sp_cy_bg += sp_rdtsc() - t;

    t = sp_rdtsc();
    draw_glow();
    sp_cy_glow += sp_rdtsc() - t;

    t = sp_rdtsc();
    draw_logo();
    sp_cy_logo += sp_rdtsc() - t;

    t = sp_rdtsc();
    draw_info();
    sp_cy_info += sp_rdtsc() - t;
}

// Erase a small box back to the (cached) gradient.
static void erase_box(int x0, int y0, int w, int h) {
    for (int j = 0; j < h; j++) {
        int y = y0 + j;
        hline_p(x0, x0 + w - 1, y, row_p(y));
    }
}

// Repaint only the 12 orbiting dots.  The original cleared the entire ring
// bounding box (~56x56 = 3136 stores) every frame and rebuilt a 12-entry
// colour table with three divisions per dot.  Erasing just the boxes the dots
// occupied last frame is ~5x cheaper, and the colours are cached, so a frame
// now involves no arithmetic at all.
static void draw_ring(void) {
    sp_frames++;
    int cx = logo_cx(), cy = ring_cy(), r = ring_r();
    int dr = (logo_scale() >= 4) ? 3 : 2;

    if (sp_dot_valid) {
        for (int i = 0; i < 12; i++)
            erase_box((int)sp_dot_x[i] - dr, (int)sp_dot_y[i] - dr, dr * 2 + 1, dr * 2 + 1);
    }
    for (int i = 0; i < 12; i++) {
        int d = (i - sp_phase) % 12;
        if (d < 0) d += 12;
        int px = cx + (int)((long)RING_TAB[i][0] * r / 1000);
        int py = cy + (int)((long)RING_TAB[i][1] * r / 1000);
        sp_dot_x[i] = (short)px;
        sp_dot_y[i] = (short)py;
        fill_circle_p(px, py, dr, sp_ring_col[d]);
    }
    sp_dot_valid = 1;
}

// ---------------------------------------------------------------------
//  Framebuffer probe
// ---------------------------------------------------------------------
static int splash_probe(void) {
    volatile unsigned char* vb = (volatile unsigned char*)VBE_ADDR;
    if (vb[VBE_OK_OFF] != 1 || vb[VBE_MODE_SET_OFF] != 1) return 0;
    if (vb[VBE_PXF_OFF] == PXF_BLT_ONLY) return 0;          // no linear framebuffer

    unsigned int base  = *(volatile unsigned int*)(vb + VBE_FB_PHYS_OFF);
    unsigned int w     = *(volatile unsigned short*)(vb + VBE_WIDTH_OFF);
    unsigned int h     = *(volatile unsigned short*)(vb + VBE_HEIGHT_OFF);
    unsigned int bpp   = vb[VBE_BPP_OFF];
    unsigned int pitch = *(volatile unsigned short*)(vb + VBE_PITCH_OFF);

    // NOTE: the 32-bit kernel always reaches the LFB through
    // framebuffer_phys -- vmm_init() rewrites it to the <4 GiB 0xF0000000
    // window when the GOP framebuffer sits above 4 GiB, so framebuffer_phys64
    // (the raw high address) would be unreachable here.
    if (!base || !pitch || w < 320 || h < 200 || w > 8192 || h > 8192) return 0;
    if (bpp != 16 && bpp != 24 && bpp != 32) return 0;
    if (pitch < w * (bpp / 8)) return 0;

    sp_fb = (unsigned char*)(unsigned long)base;
    sp_pitch = pitch;
    sp_w = w;
    sp_h = h;
    sp_bpp = bpp;
    sp_pxf = vb[VBE_PXF_OFF];
    return 1;
}

// ---------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------
int boot_splash_init(void) {
    if (sp_active) return 1;
    if (!splash_probe()) return 0;

    sp_prog = 0;
    sp_phase = 0;
    sp_label = "Starting";
    sp_frames = 0;
    sp_r_valid = 0;
    sp_dot_valid = 0;
    sp_last = sp_rdtsc();
    sp_active = 1;

    // Build the colour caches before the first paint: the scanline gradient,
    // the pre-packed palette and the 12 spinner phases.
    build_row_cache();
    build_palette();
    build_ring_colors();

    nexos_boot_splash_own(1);            // fb text console stands down
    g_net_idle_hook = boot_splash_tick;  // keep moving through blocking waits

    // Log the geometry and format: the pixel format decides how every colour
    // is encoded, and getting it wrong is invisible in the source (a packed
    // RGB565 word fed to a 0xRRGGBB blender reads as garbage in all channels).
    sp_serial("[SPLASH] active ");
    sp_serial_u32(sp_w); sp_serial("x"); sp_serial_u32(sp_h);
    sp_serial(" bpp="); sp_serial_u32(sp_bpp);
    sp_serial(" pxf="); sp_serial_u32(sp_pxf);
    sp_serial(" pitch="); sp_serial_u32(sp_pitch); sp_serial("\n");
    {
        unsigned int t0 = sp_rdtsc();
        draw_static();
        sp_cy_static += sp_rdtsc() - t0;
        t0 = sp_rdtsc();
        draw_ring();
        sp_cy_ring += sp_rdtsc() - t0;
    }
    return 1;
}

void boot_splash_stage(int stage) {
    if (!sp_active) return;
    int pct = sp_prog;
    const char* lab = sp_label;
    switch (stage) {
        case 1: pct = 12;  lab = "Starting kernel";      break;
        case 2: pct = 30;  lab = "Initialising devices"; break;
        case 5: pct = 45;  lab = "Mapping memory";       break;
        case 3: pct = 60;  lab = "Detecting hardware";   break;
        case 4: pct = 72;  lab = "Starting display";     break;
        case 6: pct = 88;  lab = "Starting desktop";     break;
        case 9: pct = 100; lab = "Ready";                break;
        default: return;
    }
    if (pct < sp_prog) pct = sp_prog;       // milestones arrive out of order
    if (pct == sp_prog && lab == sp_label) return;
    sp_prog = pct;
    sp_label = lab;
    // Incremental: erase just the label / bar / percentage bands and repaint
    // them.  The background, glow and wordmark are untouched, so a milestone
    // costs ~12k pixel writes instead of ~920k -- no more hitch per step.
    {
        unsigned int t0 = sp_rdtsc();
        erase_info();
        draw_info();
        unsigned int dt = sp_rdtsc() - t0;
        sp_cy_static += dt;
        sp_cy_info   += dt;
    }
    sp_frame_begin();
    sp_phase = (sp_phase + 1) % 12;
    {
        unsigned int t0 = sp_rdtsc();
        draw_ring();
        sp_cy_ring += sp_rdtsc() - t0;
    }
}

void boot_splash_tick(void) {
    if (!sp_active) return;
    sp_ticks++;
    if ((unsigned int)(sp_rdtsc() - sp_last) < SP_FRAME_TSC) return;
    sp_frame_begin();
    sp_phase = (sp_phase + 1) % 12;
    unsigned int t0 = sp_rdtsc();
    draw_ring();
    sp_cy_ring += sp_rdtsc() - t0;
}

int boot_splash_active(void) {
    return sp_active;
}

void boot_splash_finish(void) {
    if (!sp_active) return;
    sp_active = 0;
    g_net_idle_hook = 0;
    nexos_boot_splash_own(0);            // console/GUI may paint again
    sp_serial("[SPLASH] handed screen over to the desktop (frames painted: ");
    sp_serial_u32(sp_frames);
    sp_serial(")\n[SPLASH] render cost: static ");
    sp_serial_u32(sp_cy_static);
    sp_serial(" cyc, ring ");
    sp_serial_u32(sp_cy_ring);
    sp_serial(" cyc total, ");
    sp_serial_u32(sp_frames ? (sp_cy_ring / sp_frames) : 0);
    sp_serial(" cyc/frame\n[SPLASH] static split: gradient ");
    sp_serial_u32(sp_cy_bg);
    sp_serial(", glow ");
    sp_serial_u32(sp_cy_glow);
    sp_serial(", logo ");
    sp_serial_u32(sp_cy_logo);
    sp_serial(", info ");
    sp_serial_u32(sp_cy_info);
    sp_serial(" cyc\n[SPLASH] cadence: ");
    sp_serial_u32(sp_ticks);
    sp_serial(" tick calls, longest freeze ");
    sp_serial_u32(sp_gap_max);
    sp_serial(" cyc during \"");
    sp_serial(sp_gap_where);
    sp_serial("\"\n");
}
