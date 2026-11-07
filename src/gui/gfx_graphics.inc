// =====================================================================
//  gfx_graphics.inc  -  struct Graphics (framebuffer surface + pixel ops)
// ---------------------------------------------------------------------
//  Extracted from gui_legacy.cpp (Strangler Fig, step 2a phase 2).
//
//  This file is #included INSIDE gui_legacy.cpp's anonymous namespace, at the
//  exact position the struct used to occupy.  That is deliberate: the inline
//  bodies reference file-scope state (palette constants, font tables, helper
//  functions) that only exists at that point in that translation unit.
//
//  Consequence: this is a pure textual move -- linkage, declaration order and
//  therefore generated code are unchanged.  .text is verified byte-identical.
//  Moving the bodies out to their own .cpp is step 2b, and that step is judged
//  on performance numbers instead, because it genuinely changes codegen.
//
//  Do NOT include this file from anywhere else, and do NOT add a #pragma once:
//  it is meant to be pasted into one specific namespace.
// =====================================================================

struct Graphics {
    volatile uint32_t* lfb;      // frontbuffer (VBE LFB)
    uint32_t*          backbuffer; // off-screen buffer (regular RAM)
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint8_t  bpp;
    uint8_t  pixel_format;       // 0=BGRX32, 1=RGBX32, 2=RGB24, 3=RGB565
    bool     initialized;

void init();

    // Fast backbuffer -> LFB row copy (n pixels).
    //
    // Deliberately does NOT call memcpy().  In the 32-bit kernel memcpy is a
    // non-`rep` MOVSB loop (kernel.elf:17268 `a4; jmp`), and a bare MOVSB is
    // microcoded at ~20-30 cycles PER BYTE.  Measured on a 1280x720x32bpp
    // full-screen flip: 32-bit 95 ms (39 MB/s) vs 64-bit 7 ms -- a 13.6x gap
    // on identical source, purely from that instruction choice.  Word stores
    // cost ~1-2 cycles per 4/8 bytes on both kernels.
    //
    // The LFB pitch is frequently write-combining; word/quadword stores also
    // let the WC buffer coalesce instead of emitting one byte store per cycle.
    static inline void lfb_row_copy(uint32_t* dst, const uint32_t* src, int npix) {
        int i = 0;
        // Defensive: if either side is not 4-byte aligned (a pathological LFB
        // pitch), fall back to a byte copy rather than issue misaligned dword
        // stores into write-combining memory.
        if ((((uintptr_t)dst | (uintptr_t)src) & 3u) != 0) {
            uint8_t*       d = (uint8_t*)dst;
            const uint8_t* s = (const uint8_t*)src;
            int nb = npix * 4;
            for (int k = 0; k < nb; k++) d[k] = s[k];
            return;
        }
        if ((((uintptr_t)dst | (uintptr_t)src) & 7u) == 0) {
            uint64_t*       d8 = (uint64_t*)dst;
            const uint64_t* s8 = (const uint64_t*)src;
            int n8 = npix >> 1;
            for (int k = 0; k < n8; k++) d8[k] = s8[k];
            i = n8 << 1;
        }
        for (; i < npix; i++) dst[i] = src[i];
    }

    void present() {
        // ---- GPU fast path ------------------------------------------------
        // When the SVGA-II device has ENABLED its own scanout and is pointed at
        // this very backbuffer (sv_fb_start == backbuffer phys), the pixels are
        // ALREADY in the framebuffer memory the device scans out.  All that is
        // missing is telling it "this rectangle changed", which is one FIFO
        // command -- no 3.7 MB CPU copy per frame.
        //
        // This is the single biggest frame-time win available, and it is the
        // difference between "the desktop animates" and "the desktop is a
        // slideshow": the CPU copy of a 1024x768x32 image is ~786k 32-bit
        // stores that the VM has to emulate one at a time, while C_UPDATE
        // hands the same work to the device in six dwords.
        //
        // Guarded by svga_scanout_enabled() so any device that did not accept
        // our buffer (or has no 2D engine) falls through to the exact copy
        // path that has always worked.
        // The backbuffer is identity-mapped RAM in both kernels, so its
        // address IS its physical address -- exactly what the device must be
        // scanning for the fast path to be valid.
        if (svga_buffer_is_scanout((unsigned int)(uintptr_t)backbuffer,
                                   (unsigned int)width, (unsigned int)height)) {
            svga_update(0, 0, width, height);
            return;
        }

        // Copy backbuffer to LFB (frontbuffer), converting pixel format if needed.
        // The backbuffer stores colors as 0x00RRGGBB (R in bits 16-23) and is
        // row-major with stride = width pixels.  The LFB, however, has a row
        // PITCH that is frequently LARGER than width*bpp: real GOP hardware
        // (Intel Iris Xe, etc.) aligns each scanline to 16/32/256 bytes, so
        // PixelsPerScanLine != HorizontalResolution.  We MUST stride the LFB by
        // `pitch` per row, otherwise the whole framebuffer is written into the
        // first display row(s) and the screen collapses into a single garbled
        // strip (e.g. a white line at the top-left).  present_rect() already
        // honors pitch; this full-screen flip must too.
        if (pixel_format == PXF_RGBX32) {
            // RGBX32: swap R and B bytes
            for (int ry = 0; ry < height; ry++) {
                volatile uint8_t* dst8 = (volatile uint8_t*)lfb + (uint32_t)ry * pitch;
                uint8_t* src8 = (uint8_t*)backbuffer + (uint32_t)ry * width * 4u;
                for (int i = 0; i < width; i++) {
                    dst8[i*4]   = src8[i*4+2];  // R <- B (swap)
                    dst8[i*4+1] = src8[i*4+1];  // G
                    dst8[i*4+2] = src8[i*4];    // B <- R (swap)
                    dst8[i*4+3] = 0;            // X
                }
            }
        } else if (pixel_format == PXF_RGB24 || bpp == 24) {
            // 24-bit packed RGB (BGR in memory on x86)
            for (int ry = 0; ry < height; ry++) {
                volatile uint8_t* dst = (volatile uint8_t*)lfb + (uint32_t)ry * pitch;
                uint8_t* src = (uint8_t*)backbuffer + (uint32_t)ry * width * 4u;
                for (int i = 0; i < width; i++) {
                    dst[i*3]   = src[i*4];      // B
                    dst[i*3+1] = src[i*4+1];    // G
                    dst[i*3+2] = src[i*4+2];    // R
                }
            }
        } else if (pixel_format == PXF_RGB565 || bpp == 16) {
            // 16-bit RGB565
            for (int ry = 0; ry < height; ry++) {
                volatile uint16_t* dst = (volatile uint16_t*)((volatile uint8_t*)lfb + (uint32_t)ry * pitch);
                uint32_t* src = backbuffer + (uint32_t)ry * width;
                for (int i = 0; i < width; i++) {
                    uint32_t c = src[i];  // 0x00RRGGBB
                    uint16_t r = (c >> 19) & 0x1F;
                    uint16_t g = (c >> 10) & 0x3F;
                    uint16_t b = (c >> 3) & 0x1F;
                    dst[i] = (r << 11) | (g << 5) | b;
                }
            }
        } else {
            // BGRX32 (most common UEFI): the backbuffer byte order (B,G,R,X)
            // already matches the LFB, so this is a verbatim row copy.  Use
            // __builtin_memcpy like present_rect() instead of a per-pixel
            // volatile loop -- a full-screen flip sits on the critical path of
            // every animation frame, and `volatile` defeated any vectorisation
            // so the scalar loop dominated the frame cost.  Also the fallback
            // for unknown formats.
            for (int ry = 0; ry < height; ry++) {
                uint8_t* dst = (uint8_t*)lfb + (uint32_t)ry * pitch;
                uint8_t* src = (uint8_t*)backbuffer + (uint32_t)ry * width * 4u;
                lfb_row_copy((uint32_t*)dst, (const uint32_t*)src, width);
            }
        }
    }

    // Partial blit of a sub-rectangle from backbuffer to LFB.
    // x,y,w,h are logical pixel coordinates. Honors the LFB row pitch
    // (which may exceed width*4 on real hardware) so it is safe to call
    // for small regions. Used by the clock tick and cursor moves to avoid
    // the tearing and CPU cost of a full-screen flip.
    void present_rect(int x, int y, int w, int h) {
        if (!initialized || !backbuffer) return;
        // Clip to screen bounds
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width  - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;

        // ---- GPU fast path ------------------------------------------------
        // THE DIRTY-RECT PATH MUST USE THE SAME SURFACE AS present().
        //
        // Once svga is enabled and scans the BACKBUFFER, the pixels for this
        // rectangle are already in the memory the device displays -- the only
        // thing needed is to tell the device that this rectangle changed.
        //
        // Without this branch, present_rect() still copied the rect into the
        // VBE LFB (0xE0000000) while the device scanned the backbuffer
        // (0x01500000).  Nothing ever reached the visible surface, so partial
        // updates (clock ticks, cursor moves, window drags, menu highlights)
        // left stale pixels behind -- exactly the "dirty rectangle is broken"
        // symptom.  The full present() path looked fine because it had already
        // been switched, which is what made the bug look intermittent: static
        // screens were correct, anything that used a partial update was not.
        if (svga_buffer_is_scanout((unsigned int)(uintptr_t)backbuffer,
                                   (unsigned int)width, (unsigned int)height)) {
            svga_update(x, y, w, h);
            return;
        }

        // CPY path: the device is not scanning our buffer, so bytes must
        // actually be copied into the LFB the firmware set up.
        if (!lfb) return;
        if (pixel_format == PXF_RGBX32) {
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                volatile uint8_t* dst = (volatile uint8_t*)lfb + (uint32_t)by * pitch + (uint32_t)x * 4u;
                uint8_t* src = (uint8_t*)(backbuffer + by * width + x);
                for (int i = 0; i < w; i++) {
                    dst[i*4]   = src[i*4+2];
                    dst[i*4+1] = src[i*4+1];
                    dst[i*4+2] = src[i*4];
                    dst[i*4+3] = 0;
                }
            }
        } else if (pixel_format == PXF_RGB24 || bpp == 24) {
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                volatile uint8_t* dst = (volatile uint8_t*)lfb + (uint32_t)by * pitch + (uint32_t)x * 3u;
                uint8_t* src = (uint8_t*)(backbuffer + by * width + x);
                for (int i = 0; i < w; i++) {
                    dst[i*3]   = src[i*4];
                    dst[i*3+1] = src[i*4+1];
                    dst[i*3+2] = src[i*4+2];
                }
            }
        } else if (pixel_format == PXF_RGB565 || bpp == 16) {
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                volatile uint16_t* dst = (volatile uint16_t*)((volatile uint8_t*)lfb + (uint32_t)by * pitch);
                uint32_t* src = backbuffer + by * width + x;
                for (int i = 0; i < w; i++) {
                    uint32_t c = src[i];
                    uint16_t r = (c >> 19) & 0x1F;
                    uint16_t g = (c >> 10) & 0x3F;
                    uint16_t b = (c >> 3)  & 0x1F;
                    dst[x + i] = (uint16_t)((r << 11) | (g << 5) | b);
                }
            }
        } else {
            // BGRX32 (most common) and fallback: the LFB byte order already
            // matches the internal 0x00RRGGBB layout, so copy verbatim.  Use a
            // per-row __builtin_memcpy instead of a scalar per-pixel loop -- a
            // large win for window-drag / cursor damage (no conversion needed).
            // pitch may exceed width*4, so step the destination per row.
            for (int ry = 0; ry < h; ry++) {
                int by = y + ry;
                uint8_t* dst = (uint8_t*)lfb + (uint32_t)by * pitch + (uint32_t)x * 4u;
                uint8_t* src = (uint8_t*)(backbuffer + by * width + x);
                lfb_row_copy((uint32_t*)dst, (const uint32_t*)src, w);
            }
        }
        // Drain the store buffer so the scanout reads the freshest LFB pixels.
        // sfence orders all preceding framebuffer stores (WC/UC mapping).
        asm volatile("sfence" ::: "memory");
    }

void enable_vbe_mode();
void disable_vbe_mode();

    inline void put_pixel(int x, int y, Color c) {
        if ((uint32_t)x >= width || (uint32_t)y >= height) return;
        backbuffer[y * width + x] = c;
    }

inline Color get_pixel(int x, int y);

    void fill_rect(int x, int y, int w, int h, Color c) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;

        // ---- GPU rect fill ------------------------------------------------
        // The SVGA-II device can paint a solid rectangle itself (C_RECT_FILL),
        // which turns a w*h CPU store loop into six FIFO dwords.  Solid fills
        // are the single most common primitive in this compositor: window
        // backgrounds, taskbar, menu panels, list rows.
        //
        // It writes into the DEVICE's framebuffer, so it is only valid when the
        // device scans the same surface we would otherwise write -- and after
        // the black-screen regression the scanout is deliberately kept on the
        // firmware LFB.  The backbuffer is the compositor's working buffer, so
        // a device fill would land on the DISPLAYED image while our backbuffer
        // stayed stale.  Those must not diverge, so the GPU path is used ONLY
        // when the scanout target is the backbuffer (which svga can report
        // truthfully); otherwise the CPU path below keeps both in step.
        if (w >= 32 && h >= 8 && svga_buffer_is_scanout((unsigned int)(uintptr_t)backbuffer,
                                                        (unsigned int)width, (unsigned int)height)) {
            svga_fill_rect(x, y, w, h, c);
        }

        for (int row = 0; row < h; row++) {
            uint32_t* p = backbuffer + (y + row) * width + x;
            for (int col = 0; col < w; col++) p[col] = c;
        }

        // ---- GPU fill straight into the DISPLAYED framebuffer ------------
        // The device can paint a solid rectangle itself (C_RECT_FILL) and, unlike
        // the scanout experiment, this needs no change to what it scans: it
        // writes into the very VRAM the firmware is already displaying
        // (0xE0000000).  That is the only memory the 2D engine can touch -- a
        // guest-RAM address such as the backbuffer is invisible to it, which is
        // why the earlier scanout attempt produced a black screen.
        //
        // Requirements, all checked before issuing the command:
        //   * the device is up and its FIFO is running;
        //   * the surface we fill is the LFB it scans, at the LFB's own pitch;
        //   * the colour is converted to the framebuffer's real format
        //     (this VM is 16bpp / RGB565, pitch 2048 -- NOT the backbuffer's
        //     32-bit layout), because the device does no format conversion;
        //   * the rectangle is big enough to be worth a FIFO command instead
        //     of a few hundred stores.
        //
        // The backbuffer copy above is kept ALWAYS: the compositor and the
        // damage tracker both read those pixels, so the two surfaces must stay
        // identical regardless of which one the device painted.
        if (w >= 48 && h >= 16 && svga_lfb_fill_ready()) {
            uint32_t packed = 0;
            if (bpp == 16) {
                uint16_t r5 = (uint16_t)((c >> 19) & 0x1F);
                uint16_t g6 = (uint16_t)((c >> 10) & 0x3F);
                uint16_t b5 = (uint16_t)((c >> 3)  & 0x1F);
                packed = ((uint32_t)r5 << 11) | ((uint32_t)g6 << 5) | b5;
            } else if (bpp == 32) {
                packed = (c & 0xFFFFFFu);
                if (pixel_format == PXF_RGBX32) {
                    // R and B swap for the RGBX layouts
                    packed = ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
                }
            } else {
                return;             // 24bpp: leave it to the CPU path below
            }
            svga_fill_rect(x, y, w, h, packed);
        }
    }

    // Bulk blit a w*h RGBX32 (0x00RRGGBB) pixel block into the backbuffer.
    // Replaces the VNC client's per-pixel put_pixel() loop with a row-wise
    // copy: no per-pixel virtual call, no per-pixel bounds check, no vtable
    // indirection. On a 1024x768 full frame this removes ~786k function calls
    // per frame and is the single biggest client-side speedup.  The source and
    // backbuffer share the same Color layout (0x00RRGGBB), so no format
    // conversion is needed -- a straight 32-bit copy is correct.
    void blit_rect(int x, int y, int w, int h, const uint32_t* src) {
        if (!src || w <= 0 || h <= 0) return;
        int sx = 0, sy = 0;                       // left/top clipped columns/rows
        if (x < 0) { sx = -x; w += x; x = 0; }     // w now = original_w - sx
        if (y < 0) { sy = -y; h += y; y = 0; }     // h now = original_h - sy
        if (x + w > (int)width)  w = (int)width  - x;
        if (y + h > (int)height) h = (int)height - y;
        if (w <= 0 || h <= 0) return;
        int src_stride = w + sx;                    // original source row width
        const uint32_t* row = src + (uint64_t)sy * src_stride + sx;
        for (int r = 0; r < h; r++) {
            uint32_t* dst = backbuffer + (uint64_t)(y + r) * width + x;
            for (int c = 0; c < w; c++) dst[c] = row[c];
            row += src_stride;
        }
    }

    // Alpha-blend a solid colour over a rectangle (used for fade animations)
void blend_rect(int x, int y, int w, int h, Color c, int alpha);

    // Alpha-blend a single pixel over whatever is already in the backbuffer.
    // (put_pixel is hard-replace and ignores alpha, so translucent effects
    //  must composite manually 鈥?this is what gives windows a real glass
    //  edge that lets the desktop / window behind show through.)
    inline void blend_pixel(int x, int y, Color c, int alpha) {
        if (alpha <= 0) return;
        if ((uint32_t)x >= width || (uint32_t)y >= height) return;
        uint32_t d = backbuffer[y * width + x];
        int dr = (int)((d >> 16) & 0xFF), dg = (int)((d >> 8) & 0xFF), db = (int)(d & 0xFF);
        int cr = (int)((c >> 16) & 0xFF), cg = (int)((c >> 8) & 0xFF), cb = (int)(c & 0xFF);
        int r = (dr * (255 - alpha) + cr * alpha) / 255;
        int g = (dg * (255 - alpha) + cg * alpha) / 255;
        int b = (db * (255 - alpha) + cb * alpha) / 255;
        backbuffer[y * width + x] = (uint32_t)((r << 16) | (g << 8) | b);
    }

    // ---- Gamma-correct alpha blend (modern, crisp text rendering) ----
    // Plain alpha-over blends in (perceptual) sRGB space, which makes thin AA
    // glyph edges look washed-out / blurry.  Blending in linear light instead
    // is what FreeType/ClearType-style engines do, and yields the tight,
    // high-contrast strokes users associate with a modern OS font.  The LUTs
    // (G_TO_LIN / G_TO_SRGB) are precomputed offline into file scope so we stay
    // freestanding (no libm / pow()).
    inline void blend_pixel_lin(int x, int y, Color c, int alpha) {
        if (alpha <= 0) return;
        if ((uint32_t)x >= width || (uint32_t)y >= height) return;
        if (alpha >= 255) { put_pixel(x, y, c); return; }
        uint32_t d = backbuffer[y * width + x];
        int dr = G_TO_LIN[(d >> 16) & 0xFF], dg = G_TO_LIN[(d >> 8) & 0xFF], db = G_TO_LIN[d & 0xFF];
        int cr = G_TO_LIN[(c >> 16) & 0xFF], cg = G_TO_LIN[(c >> 8) & 0xFF], cb = G_TO_LIN[c & 0xFF];
        int a = alpha;
        int r = (dr * (255 - a) + cr * a) / 255;
        int g = (dg * (255 - a) + cg * a) / 255;
        int b = (db * (255 - a) + cb * a) / 255;
        backbuffer[y * width + x] =
            ((uint32_t)G_TO_SRGB[r] << 16) | ((uint32_t)G_TO_SRGB[g] << 8) | G_TO_SRGB[b];
    }

    // Sample a ClearType coverage bitmap (3 subpixels per destination pixel)
    // at subpixel column c3.  vec_glyph() rasterizes at 3x horizontal
    // resolution, so the buffer is cov_w = 3*pixel_w bytes wide and subpixel
    // c3 = 3*col + k lands directly on the R/G/B coverage for destination col.
static int sample_cov(const uint8_t* s, int wc, int c3);

    // Vector glyphs are rendered as a 3x subpixel coverage bitmap and then
    // rounded via cov_dil1() (a 1px morphological dilation, applied inline in
    // the draw loops) before being averaged to grayscale for compositing.  This
    // rounds the sharp glyph corners (the "rounded vector font" look) with no
    // coloured ClearType fringe on the BGRX LFB.  The dilation reads only within
    // the glyph bitmap (neighbourhood clamped to [0,w)x[0,h)), so it can never
    // overflow the buffer regardless of glyph dimensions.

    // Gamma-correct subpixel (ClearType-style) blend: R/G/B are composited
    // with their own coverage value independently, so a glyph can sharpen
    // horizontal stems by ~3x without changing its vertical size.
inline void blend_subpixel(int x, int y, Color fg, int aR, int aG, int aB);

    // Anti-aliased filled rounded rectangle (Win11 soft shadow / frosted rim /
    // glass body).  A signed-distance field to the rounded-rect outline drives
    // coverage: pixels well inside blend at full `alpha`, pixels within ~1px of
    // an edge get partial coverage, pixels outside are skipped.  The result is
    // smooth, *truly* rounded corners with no aliasing and no libm 鈥?gfx_isqrt
    // only fires for the thin boundary ring, so large interior fills stay fast.
void blend_rounded_rect(int x, int y, int w, int h, int r, Color c, int alpha);

void draw_rect(int x, int y, int w, int h, Color c);

void draw_line(int x0, int y0, int x1, int y1, Color c);

    // Integer square root (freestanding, no libm).  Used for true circular
    // rounded corners and the radial gradient helpers.
static int gfx_isqrt(int v);

    // Filled rounded rectangle (Win11 style corners) with full edge
    // anti-aliasing.  A signed-distance field to the rounded-rect outline
    // drives per-pixel coverage: the interior is opaque, the ~1px boundary
    // ring gets partial coverage (smooth against whatever is behind it), and
    // pixels outside the shape are skipped.  This replaces the old hard-edged
    // disc-corner fill so cards / windows / buttons get truly rounded, jaggy-
    // free corners.  gfx_isqrt only fires for the thin boundary ring, so the
    // large interior stays a fast opaque fill.
void fill_rounded_rect(int x, int y, int w, int h, int r, Color c);

    // Anti-aliased rounded rectangle outline (1px stroke, SDF-driven so the
    // corners are smooth 鈥?no more staircase arcs).  Straight edges stay crisp
    // (axis-aligned, no jaggies); only the corner transition gets partial
    // coverage.  blend_pixel() at alpha 255 is an opaque replace, so the stroke
    // reads as solid except at the AA boundary ring.
void draw_rounded_rect(int x, int y, int w, int h, int r, Color c);

    // ---- Inverse (XOR) rectangle 鈥?"鍙嶇煩褰? -----------------------------
    // Flips every pixel's RGB (keeps the high byte).  Re-drawing the same rect
    // restores the original, so it is ideal for selection / drag / focus
    // rectangles that must stand out over any background without a backing
    // store.  Bounds-clipped exactly like fill_rect.
void invert_rect(int x, int y, int w, int h);

    // Inverse (XOR) rectangle outline
void invert_rect_outline(int x, int y, int w, int h);


    // Draw one Latin glyph.  X-stage: rasterize the TrueType outline via
    // ---- Modern glyph cache -------------------------------------------
    // Caches rasterized coverage bitmaps (vec or baked AA) keyed by
    // (codepoint, pixel-height) so we rasterize each glyph at most once per
    // size per session 鈥?the standard performance technique of modern text
    // renderers (FreeType glyph cache / atlas).

    // ---- LRU bookkeeping for the glyph cache (P3) ----
    // Hash table (g_gc_tab) gives O(1) lookup by (cp,px); a doubly-linked LRU
    // list (lru_prev/lru_next over g_gc[]) makes eviction O(1).  Replaces the old
    // linear 256-entry scan + lru-seq counter.  Behaviour to callers is identical.
static int gc_hash(int cp, int px);
static void gc_ensure_init(void);
static void gc_lru_remove(int s);
static void gc_lru_push_mru(int s);
static void gc_hash_remove(int s);
static void gc_hash_insert(int s);

static const uint8_t* gc_get(int cp, int px, int* w, int* h, int* xo, int* yo);
static const uint8_t* gc_put(int cp, int px, int w, int h, int xo, int yo, const uint8_t* src);

    // Draw one ASCII char with anti-aliased coverage compositing.
    // Priority: (1) vector outline (64-bit, true AA at any size, cached),
    //           (2) baked 16px grayscale AA Latin (32-bit modern path),
    //           (3) legacy 1-bit 16x16 bitmap (BIOS default) fallback.
    // Returns the horizontal advance (pixels) for the next glyph.
    // Rounded vector font: a 1px morphological DILATION of the subpixel
    // coverage rounds the sharp corners of glyph strokes (a "rounded sans"
    // look) and softens edges. Coverage layout is 3 bytes/pixel (R,G,B)
    // row-major, stride w (1x grayscale: one coverage byte per pixel).  The
    // rounded-corner dilation is applied inline via cov_dil1(), which reads
    // only within src (neighbourhood clamped to [0,w)x[0,h)).
    #define RD_MAXW 160
    // Inline rounded-corner ("sans" look) dilation helper for the 1-channel
    // grayscale glyph bitmap.  Reads ONLY within src (neighbourhood clamped to
    // [0,w)x[0,h)), so it can never overflow the glyph bitmap no matter what
    // w/h are passed.  Returns a raw 0..255 coverage.  Replaces the old
    // separate static output buffer, which could be overrun for certain glyph
    // dimensions and triggered a guest triple-fault / reboot.
static int cov_dil1(const uint8_t* src, int w, int h, int row, int col);

int draw_char(int x, int y, char ch, Color fg, Color bg);

void draw_text(int x, int y, const char* s, Color fg, Color bg);

void draw_text_transparent(int x, int y, const char* s, Color fg);

    // Faux-bold: stroke the glyph 4x with 1px offsets then once centered.
    // Gives the UI modern weight for window titles / accent labels.
void draw_text_bold(int x, int y, const char* s, Color fg);
void draw_text_bold_bg(int x, int y, const char* s, Color fg, Color bg);
    // Text with a soft offset shadow (modern glass / depth cue).
    // sh = shadow colour, so = shadow offset in px.
void draw_text_shadow(int x, int y, const char* s, Color fg,
                          Color sh = 0x000000, int so = 1);
void draw_text_shadow_bg(int x, int y, const char* s, Color fg, Color bg,
                             Color sh = 0x000000, int so = 1);

    // ================= CJK (GB2312 16x16) rendering =================
static int zfont_find_unicode(uint32_t cp);

static int zfont_runtime_lookup(uint32_t cp);

    // Blit one char of the SFS 24x24 alpha font. Returns the advance, or 0 if
    // the runtime font is unavailable / the codepoint is absent (caller falls
    // through to the vector or embedded 16x16 path).
int blit_cjk_alpha(int x, int y, uint32_t cp, Color fg, Color bg, bool transparent);

    // Draw one CJK glyph by Unicode codepoint (with background).
    // Priority mirrors draw_char() (ASCII): (1) the pre-baked 16x16 8-bit
    // alpha CJK bitmap (Noto Sans CJK, open license) at the native size --
    // baked AA is crisper than runtime rasterization, exactly like English;
    // (2) the stb_truetype vector path for sizes/pieces the baked font does
    // not cover (g_font_px != 16 or a non-GB2312 glyph); (3) the embedded
    // 16x16 1-bit font as the last-resort early-boot fallback.
    // Returns the horizontal advance (pixels).
int draw_cjk(int x, int y, uint32_t cp, Color fg, Color bg);

    // Draw CJK glyph transparent (only fg pixels). Returns advance.
    // Priority mirrors draw_cjk(): baked 16x16 Noto bitmap first, then the
    // vector path, then the embedded 1-bit font.
int draw_cjk_transparent(int x, int y, uint32_t cp, Color fg);

    // Draw a mixed UTF-8 string (ASCII 8x16 + CJK 16x16) with background.
void draw_text_utf8(int x, int y, const char* s, Color fg, Color bg);

    // Same but transparent (CJK glyphs only draw fg pixels).
void draw_text_utf8_transparent(int x, int y, const char* s, Color fg);

void clear_screen(Color c);

    // Force-zero the entire front-buffer (LFB). Used on GUI entry to wipe
    // any bootuefi/OVMF residue before the GUI draws over it.
void force_clear_lfb();

    // Gradient fill (top to bottom)
void fill_gradient(int x, int y, int w, int h, Color top, Color bot);

    // 16.16 fixed-point sample coordinate for the bilinear stretch, computed
    // with 32-bit math only.  The naive form ((idx*src << 16) + (src << 15))/dst
    // needs a 64-bit divide (__divdi3), which a freestanding 32-bit kernel does
    // not link against.  Split into quotient + remainder so every intermediate
    // stays inside int32 -- bit-exact with the 64-bit expression:
    //     N = idx*src*2 + src
    //     result = ((N/dst) << 15) + (((N%dst) << 15) / dst) - 0x8000
static inline int bilinear_fp(int idx, int src, int dst);

    // Stretch-blit a SFS texture into the backbuffer, clipped to a SCREEN-space
    // box.  fmt 0 = RGB565 (bilinear), fmt 1 = ARGB32 (nearest + alpha blend).
    //
    // (x,y,w,h) is the full destination rect and is what the texture is sampled
    // over; [cx0,cy0,cx1,cy1) only limits which destination pixels are written.
    // The distinction matters: the old code clipped the DEST RECT and then
    // derived the texture index from the clipped size, so a partially visible
    // image was squashed rather than cropped.  Managed windows must never draw
    // outside their client box, hence this variant.
void draw_image_clip(const TexRec& t, int x, int y, int w, int h,
                         int cx0, int cy0, int cx1, int cy1);

    // Stretch-blit a SFS texture into the backbuffer (no extra clip box).
void draw_image(const TexRec& t, int x, int y, int w, int h);

    // Draw a simple icon (colored square with letter)
void draw_icon(int x, int y, int sz, Color bg, char letter, Color lc);

    // Draw a progress bar
void draw_progress(int x, int y, int w, int h, int pct, Color fill);

    // ---- New helper methods for UI refactoring ----

    // Fill a circle (for circular icons) - SDF anti-aliased, no math.h.
    // Inside pixels are opaque; the outer ~1px boundary ring gets partial
    // coverage so the disc no longer shows a hard staircase.
void fill_circle(int cx, int cy, int r, Color c);

    // Draw circle outline - SDF anti-aliased 1px stroke (no midpoint/ jaggies).
void draw_circle(int cx, int cy, int r, Color c);

    // Measure rendered text width using the real per-glyph advance (so
    // centering matches the actual draw, not a fixed-width assumption that
    // could let glyphs visually overlap).
int text_width(const char* s);

    // Draw text centered in a region
void draw_text_centered(int x, int y, int w, const char* s, Color fg);

    // Draw text centered with background
void draw_text_centered_bg(int x, int y, int w, const char* s, Color fg, Color bg);

    // Draw a card with shadow and rounded corners
void draw_card(int x, int y, int w, int h, Color bg);

    // Draw a circular category icon with letter
void draw_category_icon(int cx, int cy, int r, Color bg, char letter);

    // Draw a search bar (input field with magnifying glass icon)
void draw_search_bar(int x, int y, int w, int h, const char* placeholder,
                         const char* text, int text_len, bool focused);

    // Draw a table header row
void draw_table_header(int x, int y, int w, int h, const char* headers[],
                           int col_widths[], int ncols);

    // Draw a shortcut tile (icon circle + label below)
void draw_shortcut_tile(int x, int y, int w, int h, Color icon_color,
                            char letter, const char* label, bool hover);

    // Draw a navigation tab
void draw_nav_tab(int x, int y, int w, int h, const char* label, bool selected);
};

// ---- cold drawing-composition bodies (step 2b) ----
#include "gfx_draw.inc"


