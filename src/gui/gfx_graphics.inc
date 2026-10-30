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

    void init() {
        volatile VbeInfo* info = (volatile VbeInfo*)0x5000;
        if (info->vbe_ok != 1) {
            // No VBE info from BIOS INT 10h or UEFI GOP. Try BGA ports
            // (QEMU/VirtualBox/Bochs always provide Bochs VBE ports even when
            // SeaBIOS/VBoxVGA don't expose classic VBE BIOS).
            if (!bga_detect()) {
                initialized = false;
                serial_puts("[GUI] VBE not available (no BIOS VBE, no BGA)\n");
                return;
            }
            // Synthesize a default VBE info so the rest of init works.
            // enable_vbe_mode() will actually set the mode via BGA ports.
            info->vbe_ok         = 1;
            info->vbe_mode_set   = 0;            // BGA will set it; do not claim BIOS already did
            info->width          = 1024;
            info->height         = 768;
            info->bpp            = 32;
            info->pitch          = 1024 * 4;
            info->mode_number    = 0x117;        // standard 1024x768x32
            info->pixel_format   = PXF_BGRX32;
            info->framebuffer_phys64 = 0xFD000000ULL;  // QEMU stdvga (bochs-display) BAR0 LFB
            info->framebuffer_phys   = 0xFD000000;
            info->shadow_buffer  = 1;   // VMM maps BGA LFB; do not heap-shadow it
            g_bga_available = true;
            serial_puts("[GUI] BGA fallback: synthesized VBE info (1024x768x32, BGRX32)\n");
        }

        // Check for BltOnly (no linear framebuffer)
        if (info->pixel_format == PXF_BLT_ONLY) {
            initialized = false;
            serial_puts("[GUI] GOP is BltOnly - no linear framebuffer\n");
            return;
        }

#if defined(__x86_64__)
        // 64-bit kernel: pointers are 64-bit wide, and switch32to64.asm has
        // identity-mapped the high framebuffer with a 1 GiB page, so use the
        // FULL physical address.  Truncating to framebuffer_phys would give 0
        // for a real-hardware LFB at 0x4000000000 (Intel Iris Xe) and every
        // pixel write would land on the IVT instead of the screen -- the
        // machine stays alive but the display never lights up.
        if (info->framebuffer_phys64 > 0xFFFFFFFFULL) {
            lfb = (volatile uint32_t*)(uintptr_t)info->framebuffer_phys64;
            // Register the real high framebuffer as FB_PHYS and, crucially, as the
            // draw address FB_VIRT.  In 64-bit, switch32to64.asm identity-mapped
            // the REAL address, so the GUI must draw there (NOT the 0xF0000000
            // window that only exists in the 32-bit compat page tables).
            addr_set(ADDR_FB_PHYS, info->framebuffer_phys64, info->framebuffer_phys64,
                     (uint64_t)info->pitch * info->height, ADDR_FLAG_FB | ADDR_FLAG_MMIO);
            addr_set(ADDR_FB_VIRT, info->framebuffer_phys64, info->framebuffer_phys64, 0, ADDR_FLAG_FB);
            serial_puts("[GUI] 64-bit high framebuffer in use (1GiB identity page)\n");
        } else {
            lfb = (volatile uint32_t*)(uintptr_t)info->framebuffer_phys;
            addr_set(ADDR_FB_PHYS, info->framebuffer_phys, info->framebuffer_phys,
                     (uint64_t)info->pitch * info->height, ADDR_FLAG_FB | ADDR_FLAG_MMIO);
            addr_set(ADDR_FB_VIRT, info->framebuffer_phys, info->framebuffer_phys, 0, ADDR_FLAG_FB);
        }
#else
        lfb = (volatile uint32_t*)info->framebuffer_phys;
#endif
        width = info->width;
        height = info->height;
        pitch = info->pitch;
        bpp = info->bpp;
        pixel_format = info->pixel_format;

        // [DGOP] Dump the actual GOP/UEFI framebuffer parameters so real-hardware
        // mismatches (PixelsPerScanLine != HorizontalResolution, or FB above 4GB)
        // are visible in the serial log.  On Intel Iris Xe the pitch is usually
        // wider than width*bpp; present() now honors it.
        {
            uint32_t nominal = (uint32_t)width * (bpp / 8u);
            serial_puts("[DGOP] fb_base=0x");
            serial_puthex32(info->framebuffer_phys);
            serial_puts(" fb64=0x");
            serial_puthex32((uint32_t)(info->framebuffer_phys64 & 0xFFFFFFFFULL));
            serial_puts((info->framebuffer_phys64 >> 32) ? "HIGH " : " ");
            serial_puts("w=");
            serial_putdec((int)width);
            serial_puts(" h=");
            serial_putdec((int)height);
            serial_puts(" bpp=");
            serial_putdec((int)bpp);
            serial_puts(" pitch=");
            serial_putdec((int)pitch);
            serial_puts(" nominal=");
            serial_putdec((int)nominal);
            serial_puts(pitch == nominal ? " MATCH\n" : " MISMATCH(pitch>width)\n");
        }

        // For a framebuffer above 4 GB, kernel.cpp vmm_init() has already built
        // 4-level page tables that map the real high framebuffer into a <4GB
        // virtual window (0xF0000000) and rewritten framebuffer_phys to that
        // window address, with shadow_buffer set to 1 as the "mapped" sentinel.
        // So writing to framebuffer_phys is always safe here.  We only abort
        // when the full 64-bit address is above 4 GB AND shadow_buffer == 0
        // (meaning the kernel could not set up the mapping).

        // Validate framebuffer address is accessible (below 4GB for 32-bit kernel).
        // The 64-bit kernel has no such limit: it addresses the high LFB directly
        // through the 1 GiB identity page built in switch32to64.asm, so this abort
        // is deliberately restricted to the 32-bit build.
#if !defined(__x86_64__)
        if (info->framebuffer_phys64 > 0xFFFFFFFFULL && info->shadow_buffer == 0) {
            serial_puts("[GUI] WARNING: Framebuffer above 4GB without shadow buffer\n");
            initialized = false;
            return;
        }
#endif

        serial_puts("[GUI] Pixel format: ");
        serial_puts(pixel_format == PXF_BGRX32 ? "BGRX32" :
                    pixel_format == PXF_RGBX32 ? "RGBX32" :
                    pixel_format == PXF_RGB24  ? "RGB24"  :
                    pixel_format == PXF_RGB565 ? "RGB565" : "unknown");
        serial_puts("\n");

        // Detect BGA (Bochs VBE ports) for dynamic mode switching
        g_bga_available = bga_detect();
        // Check if mode was already set by BIOS (INT 10h) or UEFI (GOP)
        g_vbe_mode_set_by_bios = (info->vbe_mode_set == 1);
        // A GOP framebuffer is recognised by the ABSENCE of a VBE mode number:
        // stage2.asm always fills it in on the BIOS path (0x117 / 0x144), and the
        // BGA fallback above synthesises 0x117 as well, so mode_number == 0 with
        // vbe_mode_set == 1 can only mean "UEFI GOP handed us an active surface".
        g_gop_native = (info->mode_number == 0) && (info->vbe_mode_set == 1);

        serial_puts("[GUI] BGA: ");
        serial_puts(g_bga_available ? "yes" : "no");
        serial_puts(", BIOS mode set: ");
        serial_puts(g_vbe_mode_set_by_bios ? "yes" : "no");
        serial_puts(", source: ");
        serial_puts(g_gop_native ? "UEFI-GOP\n" : "BIOS-VBE/BGA\n");

        // Allocate backbuffer (off-screen buffer for double buffering).
        // Framebuffer-class buffer: use bigbuf (PMM-backed, outside the GUI
        // heap) -- a 1024x768x4 = 3 MiB kmalloc cannot fit the 10 MiB heap
        // alongside the managed shell's own allocations and used to OOM.
        backbuffer = (uint32_t*)bigbuf_alloc((uint32_t)(width * height * 4));
        if (!backbuffer) {
            serial_puts("[GUI] Failed to allocate backbuffer\n");
            initialized = false;
            return;
        }
        // Clear backbuffer to black
        for (int i = 0; i < width * height; i++) backbuffer[i] = 0;

        initialized = true;
        serial_puts("[GUI] Framebuffer initialized with double-buffering\n");
    }

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
        if (!initialized || !lfb || !backbuffer) return;
        // Clip to screen bounds
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width  - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;

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

    void enable_vbe_mode() {
        // UEFI GOP already owns the ACTIVE scanout surface -- do not touch BGA.
        //
        // QEMU q35 + OVMF exposes BOTH a standard VGA device (with Bochs VBE
        // ports) AND the surface GOP actually selected (ramfb here).  Firing
        // bochs_vbe_set_mode() would reprogram std VGA and hand scanout to its
        // own LFB (0xFD000000 / 0xE0000000), while the GUI keeps drawing into the
        // GOP framebuffer we were told about (0x7E4CF000) -> the monitor shows the
        // untouched, black std-VGA surface.  Real hardware is worse: the LFB sits
        // at 0x4000000000 and BGA ports don't exist at all, but if a chipset did
        // answer we would blank the panel the firmware just lit up.
        if (g_gop_native) {
            serial_puts("[GUI] UEFI GOP surface is already active - skipping BGA mode set\n");
            return;
        }
        // If BGA ports available (QEMU/VirtualBox/Bochs), set a KNOWN-GOOD
        // 32bpp mode directly via the BGA ports.  The BIOS (SeaBIOS/VBoxVGA)
        // often enumerates a 16bpp mode (0x118) and the GUI's backbuffer +
        // present() path is built around 32bpp, so we MUST force 32bpp here
        // AND normalize the descriptor (bpp/pitch/pixel_format) to match what
        // we just programmed -- otherwise present() writes 4 bytes/pixel with a
        // 2-byte/px pitch and the live framebuffer never shows the desktop.
        if (g_bga_available) {
            bochs_vbe_set_mode(width, height, 32);
            bpp          = 32;
            pitch        = (uint16_t)((uint32_t)width * 4u);
            pixel_format = PXF_BGRX32;
            serial_puts("[GUI] VBE mode forced to 32bpp via BGA ports (w=");
            serial_putdec((int)width);
            serial_puts(" h=");
            serial_putdec((int)height);
            serial_puts(" pitch=");
            serial_putdec((int)pitch);
            serial_puts(")\n");
        } else if (g_vbe_mode_set_by_bios) {
            // Mode already set by BIOS INT 10h or UEFI GOP - nothing to do
            serial_puts("[GUI] VBE mode already set by BIOS/UEFI\n");
        } else {
            // No BGA and mode not set by BIOS - can't set mode in PM
            serial_puts("[GUI] WARNING: No way to set VBE mode (no BGA, no BIOS mode set)\n");
        }
    }
    void disable_vbe_mode() {
        // Symmetric with enable_vbe_mode(): we never programmed BGA on a GOP
        // framebuffer, so disabling it would only blank a surface we don't own.
        if (g_gop_native) {
            serial_puts("[GUI] UEFI GOP surface stays active (BGA never programmed)\n");
            return;
        }
        // Only disable if we set it via BGA (can't switch back to text mode without INT 10h)
        if (g_bga_available) {
            bochs_vbe_disable();
            serial_puts("[GUI] VBE mode disabled (BGA)\n");
        } else {
            // Can't switch back to text mode without INT 10h (we're in PM)
            // Just clear the framebuffer instead
            serial_puts("[GUI] VBE mode stays active (no BGA to disable)\n");
        }
    }

    inline void put_pixel(int x, int y, Color c) {
        if ((uint32_t)x >= width || (uint32_t)y >= height) return;
        backbuffer[y * width + x] = c;
    }

    inline Color get_pixel(int x, int y) {
        if ((uint32_t)x >= width || (uint32_t)y >= height) return 0;
        return backbuffer[y * width + x];
    }

    void fill_rect(int x, int y, int w, int h, Color c) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;
        for (int row = 0; row < h; row++) {
            uint32_t* p = backbuffer + (y + row) * width + x;
            for (int col = 0; col < w; col++) p[col] = c;
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
    void blend_rect(int x, int y, int w, int h, Color c, int alpha) {
        if (alpha <= 0) return;
        if (alpha > 255) alpha = 255;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;
        int cr = (int)((c >> 16) & 0xFF), cg = (int)((c >> 8) & 0xFF), cb = (int)(c & 0xFF);
        for (int row = 0; row < h; row++) {
            uint32_t* p = backbuffer + (y + row) * width + x;
            for (int col = 0; col < w; col++) {
                uint32_t d = p[col];
                int dr = (int)((d >> 16) & 0xFF), dg = (int)((d >> 8) & 0xFF), db = (int)(d & 0xFF);
                int r = (dr * (255 - alpha) + cr * alpha) / 255;
                int g = (dg * (255 - alpha) + cg * alpha) / 255;
                int b = (db * (255 - alpha) + cb * alpha) / 255;
                p[col] = (uint32_t)((r << 16) | (g << 8) | b);
            }
        }
    }

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
    static int sample_cov(const uint8_t* s, int wc, int c3) {
        if (c3 < 0)   c3 = 0;
        if (c3 >= wc) c3 = wc - 1;
        return s[c3];
    }

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
    inline void blend_subpixel(int x, int y, Color fg, int aR, int aG, int aB) {
        if ((uint32_t)x >= width || (uint32_t)y >= height) return;
        if (aR <= 4 && aG <= 4 && aB <= 4) return;
        uint32_t d = backbuffer[y * width + x];
        int dr = G_TO_LIN[(d >> 16) & 0xFF], dg = G_TO_LIN[(d >> 8) & 0xFF], db = G_TO_LIN[d & 0xFF];
        int fr = G_TO_LIN[(fg >> 16) & 0xFF], fg2 = G_TO_LIN[(fg >> 8) & 0xFF], fb = G_TO_LIN[fg & 0xFF];
        int r = (dr * (255 - aR) + fr * aR) / 255;
        int g = (dg * (255 - aG) + fg2 * aG) / 255;
        int b = (db * (255 - aB) + fb * aB) / 255;
        backbuffer[y * width + x] =
            ((uint32_t)G_TO_SRGB[r] << 16) | ((uint32_t)G_TO_SRGB[g] << 8) | G_TO_SRGB[b];
    }

    // Anti-aliased filled rounded rectangle (Win11 soft shadow / frosted rim /
    // glass body).  A signed-distance field to the rounded-rect outline drives
    // coverage: pixels well inside blend at full `alpha`, pixels within ~1px of
    // an edge get partial coverage, pixels outside are skipped.  The result is
    // smooth, *truly* rounded corners with no aliasing and no libm 鈥?gfx_isqrt
    // only fires for the thin boundary ring, so large interior fills stay fast.
    void blend_rounded_rect(int x, int y, int w, int h, int r, Color c, int alpha) {
        if (alpha <= 0) return;
        if (alpha > 255) alpha = 255;
        if (r < 0) r = 0;
        if (r > h / 2) r = h / 2;
        if (r > w / 2) r = w / 2;
        if (r == 0) { blend_rect(x, y, w, h, c, alpha); return; }
        int x0 = x - 1, y0 = y - 1, x1 = x + w + 1, y1 = y + h + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > width)  x1 = width;
        if (y1 > height) y1 = height;
        if (x1 <= x0 || y1 <= y0) return;
        int hw = w / 2, hh = h / 2;
        int cx = x + w / 2, cy = y + h / 2;
        for (int py = y0; py < y1; py++) {
            int ay = ((py < cy) ? (cy - py) : (py - cy)) - (hh - r);
            int qy = (ay > 0) ? ay : 0;
            for (int px = x0; px < x1; px++) {
                int ax = ((px < cx) ? (cx - px) : (px - cx)) - (hw - r);
                int qx = (ax > 0) ? ax : 0;
                int d;
                if (qx == 0 && qy == 0) d = -r;                 // deep inside
                else d = gfx_isqrt(qx * qx + qy * qy) - r;      // SDF (px)
                int t = 128 - d * 256;                          // (0.5 - d) 路 256
                if (t <= 0) continue;                           // outside shape
                if (t > 256) t = 256;                           // full coverage
                blend_pixel(px, py, c, (t * alpha) >> 8);
            }
        }
    }

    void draw_rect(int x, int y, int w, int h, Color c) {        for (int i = x; i < x + w; i++) { put_pixel(i, y, c); put_pixel(i, y + h - 1, c); }
        for (int i = y; i < y + h; i++) { put_pixel(x, i, c); put_pixel(x + w - 1, i, c); }
    }

    void draw_line(int x0, int y0, int x1, int y1, Color c) {
        int dx = x1 - x0; if (dx < 0) dx = -dx;
        int dy = y1 - y0; if (dy < 0) dy = -dy;
        int sx = (x0 < x1) ? 1 : -1;
        int sy = (y0 < y1) ? 1 : -1;
        int err = dx - dy;
        for (;;) {
            put_pixel(x0, y0, c);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 < dx)  { err += dx; y0 += sy; }
        }
    }

    // Integer square root (freestanding, no libm).  Used for true circular
    // rounded corners and the radial gradient helpers.
    static int gfx_isqrt(int v) {
        if (v <= 0) return 0;
        int res = 0;
        for (int bit = 1 << 15; bit != 0; bit >>= 1) {
            int t = res | bit;
            if (t * t <= v) res = t;
        }
        return res;
    }

    // Filled rounded rectangle (Win11 style corners) with full edge
    // anti-aliasing.  A signed-distance field to the rounded-rect outline
    // drives per-pixel coverage: the interior is opaque, the ~1px boundary
    // ring gets partial coverage (smooth against whatever is behind it), and
    // pixels outside the shape are skipped.  This replaces the old hard-edged
    // disc-corner fill so cards / windows / buttons get truly rounded, jaggy-
    // free corners.  gfx_isqrt only fires for the thin boundary ring, so the
    // large interior stays a fast opaque fill.
    void fill_rounded_rect(int x, int y, int w, int h, int r, Color c) {
        if (r > h / 2) r = h / 2;
        if (r > w / 2) r = w / 2;
        if (r <= 0) { fill_rect(x, y, w, h, c); return; }
        int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > width)  x1 = width;
        if (y1 > height) y1 = height;
        if (x1 <= x0 || y1 <= y0) return;
        int hw = w / 2, hh = h / 2;
        int cx = x + w / 2, cy = y + h / 2;
        for (int py = y0; py < y1; py++) {
            int ay = ((py < cy) ? (cy - py) : (py - cy)) - (hh - r);
            int qy = (ay > 0) ? ay : 0;
            for (int px = x0; px < x1; px++) {
                int ax = ((px < cx) ? (cx - px) : (px - cx)) - (hw - r);
                int qx = (ax > 0) ? ax : 0;
                if (qx == 0 && qy == 0) {            // deep inside: opaque
                    put_pixel(px, py, c);
                } else {
                    int d = gfx_isqrt(qx * qx + qy * qy) - r;  // SDF (px)
                    int t = 128 - d * 256;            // (0.5 - d) * 256
                    if (t <= 0) continue;             // outside shape
                    if (t >= 255) { put_pixel(px, py, c); continue; }
                    blend_pixel(px, py, c, t);
                }
            }
        }
    }

    // Anti-aliased rounded rectangle outline (1px stroke, SDF-driven so the
    // corners are smooth 鈥?no more staircase arcs).  Straight edges stay crisp
    // (axis-aligned, no jaggies); only the corner transition gets partial
    // coverage.  blend_pixel() at alpha 255 is an opaque replace, so the stroke
    // reads as solid except at the AA boundary ring.
    void draw_rounded_rect(int x, int y, int w, int h, int r, Color c) {
        if (r < 0) r = 0;
        if (r > h / 2) r = h / 2;
        if (r > w / 2) r = w / 2;
        if (r == 0) { draw_rect(x, y, w, h, c); return; }
        int x0 = x - 2, y0 = y - 2, x1 = x + w + 2, y1 = y + h + 2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > width)  x1 = width;
        if (y1 > height) y1 = height;
        if (x1 <= x0 || y1 <= y0) return;
        int hw = w / 2, hh = h / 2, cx = x + w / 2, cy = y + h / 2;
        for (int py = y0; py < y1; py++) {
            int ay0 = (py < cy) ? (cy - py) : (py - cy);
            int ay  = ay0 - (hh - r);
            int qy  = (ay > 0) ? ay : 0;
            for (int px = x0; px < x1; px++) {
                int ax0 = (px < cx) ? (cx - px) : (px - cx);
                int ax  = ax0 - (hw - r);
                int qx  = (ax > 0) ? ax : 0;
                int d   = (qx == 0 && qy == 0) ? -r : gfx_isqrt(qx * qx + qy * qy) - r;
                int dist = (d < 0) ? -d : d;          // |signed distance to boundary|
                int cov = 256 - dist * 256;            // 1px stroke: centre full, edge half
                if (cov <= 0) continue;
                if (cov > 256) cov = 256;
                blend_pixel(px, py, c, cov);
            }
        }
    }

    // ---- Inverse (XOR) rectangle 鈥?"鍙嶇煩褰? -----------------------------
    // Flips every pixel's RGB (keeps the high byte).  Re-drawing the same rect
    // restores the original, so it is ideal for selection / drag / focus
    // rectangles that must stand out over any background without a backing
    // store.  Bounds-clipped exactly like fill_rect.
    void invert_rect(int x, int y, int w, int h) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width)  w = width - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) return;
        for (int row = 0; row < h; row++) {
            uint32_t* p = backbuffer + (y + row) * width + x;
            for (int col = 0; col < w; col++) p[col] ^= 0x00FFFFFF;
        }
    }

    // Inverse (XOR) rectangle outline
    void invert_rect_outline(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        invert_rect(x, y, w, 1);
        invert_rect(x, y + h - 1, w, 1);
        invert_rect(x, y, 1, h);
        invert_rect(x + w - 1, y, 1, h);
    }


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
    static int gc_hash(int cp, int px) {
        unsigned h = (unsigned)((cp * 2654435761u) ^ (px * 40503u));
        return (int)(h % (unsigned)GC_HTAB);
    }
    static void gc_ensure_init(void) {
        if (g_gc_inited) return;
        for (int i = 0; i < GC_HTAB; i++) g_gc_tab[i] = -1;
        for (int i = 0; i < GC_N; i++) {
            g_gc[i].bmp = nullptr;
            g_gc[i].hnext = -1;
            g_gc[i].lru_prev = -1;
            g_gc[i].lru_next = -1;
        }
        g_gc_lru_head = g_gc_lru_tail = -1;
        g_gc_inited = true;
    }
    static void gc_lru_remove(int s) {
        int p = g_gc[s].lru_prev, n = g_gc[s].lru_next;
        if (p >= 0) g_gc[p].lru_next = n; else g_gc_lru_head = n;
        if (n >= 0) g_gc[n].lru_prev = p; else g_gc_lru_tail = p;
        g_gc[s].lru_prev = g_gc[s].lru_next = -1;
    }
    static void gc_lru_push_mru(int s) {
        g_gc[s].lru_prev = -1;
        g_gc[s].lru_next = g_gc_lru_head;
        if (g_gc_lru_head >= 0) g_gc[g_gc_lru_head].lru_prev = s;
        else g_gc_lru_tail = s;
        g_gc_lru_head = s;
    }
    static void gc_hash_remove(int s) {
        int h = gc_hash(g_gc[s].cp, g_gc[s].px);
        int cur = g_gc_tab[h], prev = -1;
        while (cur >= 0) {
            if (cur == s) {
                if (prev >= 0) g_gc[prev].hnext = g_gc[s].hnext;
                else g_gc_tab[h] = g_gc[s].hnext;
                g_gc[s].hnext = -1;
                return;
            }
            prev = cur; cur = g_gc[cur].hnext;
        }
    }
    static void gc_hash_insert(int s) {
        int h = gc_hash(g_gc[s].cp, g_gc[s].px);
        g_gc[s].hnext = g_gc_tab[h];
        g_gc_tab[h] = s;
    }

    static const uint8_t* gc_get(int cp, int px, int* w, int* h, int* xo, int* yo) {
        gc_ensure_init();
        int idx = gc_hash(cp, px);
        for (int s = g_gc_tab[idx]; s >= 0; s = g_gc[s].hnext) {
            if (g_gc[s].bmp && g_gc[s].cp == cp && g_gc[s].px == px) {
                gc_lru_remove(s);
                gc_lru_push_mru(s);
                *w = g_gc[s].w; *h = g_gc[s].h; *xo = g_gc[s].xo; *yo = g_gc[s].yo;
                return g_gc[s].bmp;
            }
        }
        return nullptr;
    }
    static const uint8_t* gc_put(int cp, int px, int w, int h, int xo, int yo, const uint8_t* src) {
        gc_ensure_init();
        int slot = -1;
        for (int i = 0; i < GC_N; i++) {        // prefer a free slot
            if (!g_gc[i].bmp) { slot = i; break; }
        }
        if (slot < 0) {                          // none free: evict LRU
            slot = g_gc_lru_tail;
            if (slot < 0) return nullptr;
            gc_hash_remove(slot);
            gc_lru_remove(slot);
            if (g_gc[slot].bmp) kfree(g_gc[slot].bmp);
        }
        uint8_t* b = (uint8_t*)kmalloc((uint32_t)(w * h));
        if (!b) return nullptr;
        for (int i = 0; i < w * h; i++) b[i] = src[i];
        g_gc[slot].cp = cp; g_gc[slot].px = px;
        g_gc[slot].w = w; g_gc[slot].h = h; g_gc[slot].xo = xo; g_gc[slot].yo = yo;
        g_gc[slot].bmp = b;
        gc_hash_insert(slot);
        gc_lru_push_mru(slot);
        return b;
    }

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
    static int cov_dil1(const uint8_t* src, int w, int h, int row, int col) {
        int m = 0;
        for (int dy = -1; dy <= 1; dy++) {
            int yy = row + dy; if (yy < 0 || yy >= h) continue;
            const uint8_t* p = src + (size_t)yy * w;
            for (int dx = -1; dx <= 1; dx++) {
                int xx = col + dx; if (xx < 0 || xx >= w) continue;
                if (p[xx] > m) m = p[xx];
            }
        }
        return m;
    }

    int draw_char(int x, int y, char ch, Color fg, Color bg) {
        uint8_t c = (uint8_t)ch;

        // (0) Native 16x16 8-bit-alpha bitmap (uniform with the CJK 24x24
        //     bitmap).  Grayscale-blended (alpha, not binary) so glyph edges
        //     are anti-aliased; preferred at the default 16px size.
        if (g_font_mode != 0 && g_font_px == AFONT_H &&
            c >= AFONT_FIRST && c < AFONT_FIRST + AFONT_COUNT) {
            int idx = c - AFONT_FIRST;
            const uint8_t* g = afont_alpha[idx];
            for (int row = 0; row < AFONT_H; row++) {
                const uint8_t* s = g + row * AFONT_W;
                for (int col = 0; col < AFONT_W; col++) {
                    int a = s[col];
                    if (a == 0) {
                        if (bg != (Color)-1) put_pixel(x + col, y + row, bg);
                    } else if (a >= 250) {
                        put_pixel(x + col, y + row, fg);
                    } else {
                        blend_pixel_lin(x + col, y + row, fg, a);
                    }
                }
            }
            return afont_adv[idx];
        }

        // (1) Vector outline via stb_truetype (64-bit kernel).
        if (g_font_mode != 0 && vec_ready()) {
            int w = 0, h = 0, xo = 0, yo = 0;
            const uint8_t* g = gc_get(c, g_font_px, &w, &h, &xo, &yo);
            bool cached = (g != nullptr);
            if (!g) {
                g = vec_glyph(c, g_font_px, &w, &h, &xo, &yo);
                if (g) {
                    const uint8_t* cp = gc_put(c, g_font_px, w, h, xo, yo, g);
                    if (cp) g = cp;   // use our cache-owned copy
                }
            }
            if (g) {
                int gx = x + xo;
                int gy = y;   // top-align inside the 16px cell to match ASCII (afont/fla16 draw at y)
                if (bg != (Color)-1) fill_rect(gx, gy, w, h, bg);
                // 1x grayscale antialiasing: one coverage byte per pixel, so
                // glyph edges stay sharp (no over-smoothing).  When rounding is
                // on, dilate the coverage by 1px (cov_dil1) to round the sharp
                // glyph corners -- bounds-safe (reads only within g,
                // neighbourhood clamped to [0,w)x[0,h)).
                const int rounding = g_font_round && w <= RD_MAXW && h <= RD_MAXW;
                for (int row = 0; row < h; row++) {
                    const uint8_t* src = g + row * w;
                    for (int col = 0; col < w; col++) {
                        int a = rounding ? cov_dil1(g, w, h, row, col)
                                         : src[col];
                        if (a > 4)
                            blend_pixel_lin(gx + col, gy + row, fg, a);
                    }
                }
                int adv = vec_advance(c, g_font_px);
                (void)cached;
                return adv > 0 ? adv : (w + 1);
            }
        }

        // (2) Baked 16px grayscale AA Latin (32-bit modern path).
        if (g_font_mode != 0 && g_fla16) {
            int w = 0, h = 0;
            const uint8_t* g = fla16_glyph(c, &w, &h);
            if (g) {
                const int rounding = g_font_round && w <= RD_MAXW && h <= RD_MAXW;
                // Pre-baked rounded copy (identical offsets): when present the
                // per-glyph dilation is a plain sample, not a 3x3 max.
                const uint8_t* rg = (rounding && g_fla16_round)
                                    ? (g_fla16_round + (int)(g - g_fla16)) : nullptr;
                if (bg != (Color)-1) fill_rect(x, y, w, h, bg);
                for (int row = 0; row < h; row++) {
                    const uint8_t* src = g + row * w;
                    for (int col = 0; col < w; col++) {
                        int a = rg ? rg[row * w + col]
                                   : (rounding ? cov_dil1(g, w, h, row, col) : sample_cov(src, w, col));
                        if (a > 4)
                            blend_subpixel(x + col, y + row, fg, a, a, a);
                    }
                }
                return fla16_advance(c);   // real per-glyph advance
            }
        }

        // (3) Legacy 1-bit 8x16 bitmap (BIOS default size) fallback.
        //    Kept as a thin 1-bit render (no鑶ㄨ儉 AA) -- the baked AA Latin
        //    font (font_la16.bin) is the real high-quality path.
        const uint8_t* glyph = font8x16[c];
        for (int row = 0; row < 16; row++) {
            uint8_t byte = glyph[row];   // 8 pixels, MSB first
            for (int col = 0; col < 8; col++) {
                if (byte & (0x80 >> col)) {
                    if (bg == (Color)-1) put_pixel(x + col, y + row, fg);
                    else blend_pixel(x + col, y + row, fg, 255);
                } else if (bg != (Color)-1) {
                    put_pixel(x + col, y + row, bg);
                }
            }
        }
        return 8;
    }

    void draw_text(int x, int y, const char* s, Color fg, Color bg) {
        int cx = x;
        while (*s) {
            if (*s == '\n') { cx = x; y += g_font_h; }
            else { cx += draw_char(cx, y, *s, fg, bg); }
            s++;
        }
    }

    void draw_text_transparent(int x, int y, const char* s, Color fg) {
        int cx = x;
        while (*s) {
            if (*s == '\n') { cx = x; y += g_font_h; }
            else { cx += draw_char(cx, y, *s, fg, (Color)-1); }
            s++;
        }
    }

    // Faux-bold: stroke the glyph 4x with 1px offsets then once centered.
    // Gives the UI modern weight for window titles / accent labels.
    void draw_text_bold(int x, int y, const char* s, Color fg) {
        draw_text_transparent(x - 1, y, s, fg);
        draw_text_transparent(x + 1, y, s, fg);
        draw_text_transparent(x, y - 1, s, fg);
        draw_text_transparent(x, y + 1, s, fg);
        draw_text_transparent(x, y, s, fg);
    }
    void draw_text_bold_bg(int x, int y, const char* s, Color fg, Color bg) {
        draw_text(x - 1, y, s, fg, bg);
        draw_text(x + 1, y, s, fg, bg);
        draw_text(x, y - 1, s, fg, bg);
        draw_text(x, y + 1, s, fg, bg);
        draw_text(x, y, s, fg, bg);
    }
    // Text with a soft offset shadow (modern glass / depth cue).
    // sh = shadow colour, so = shadow offset in px.
    void draw_text_shadow(int x, int y, const char* s, Color fg,
                          Color sh = 0x000000, int so = 1) {
        draw_text_transparent(x + so, y + so, s, sh);
        draw_text_transparent(x, y, s, fg);
    }
    void draw_text_shadow_bg(int x, int y, const char* s, Color fg, Color bg,
                             Color sh = 0x000000, int so = 1) {
        draw_text(x + so, y + so, s, sh, bg);
        draw_text(x, y, s, fg, bg);
    }

    // ================= CJK (GB2312 16x16) rendering =================
    static int zfont_find_unicode(uint32_t cp) {
        int lo = 0, hi = (int)zfont_count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (zfont_unicode[mid] == cp) return mid;
            if (zfont_unicode[mid] < cp) lo = mid + 1;
            else hi = mid - 1;
        }
        return -1;
    }

    static int zfont_runtime_lookup(uint32_t cp) {
        int lo = 0, hi = g_zf_n - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            uint32_t u = g_zf_uni[mid];
            if (u == cp) return mid;
            if (u < cp) lo = mid + 1; else hi = mid - 1;
        }
        return -1;
    }

    // Blit one char of the SFS 24x24 alpha font. Returns the advance, or 0 if
    // the runtime font is unavailable / the codepoint is absent (caller falls
    // through to the vector or embedded 16x16 path).
    int blit_cjk_alpha(int x, int y, uint32_t cp, Color fg, Color bg, bool transparent) {
        if (!g_zf_loaded || g_font_px != AFONT_H) return 0;
        int idx = zfont_runtime_lookup(cp);
        if (idx < 0) return 0;
        const uint8_t* g = g_zf_alpha + (uint32_t)idx * (uint32_t)g_zf_px * (uint32_t)g_zf_px;
        int px = g_zf_px;                  // source glyph size (16 = native)
        int dh = g_font_h;                 // target line height (16)
        if (dh > px) dh = px;              // never upscale
        // Sample the px*px glyph into a dh*dh cell (box area-average when
        // px > dh; identity 1:1 when px == dh, the native baked case).  The
        // old code merely centred the glyph on the line, which drew Chinese
        // ~50% taller than English.
        for (int row = 0; row < dh; row++) {
            int sy = (row * px + dh / 2) / dh;
            const uint8_t* s = g + (uint32_t)sy * px;
            int oy = y + row;
            for (int col = 0; col < dh; col++) {
                int sx = (col * px + dh / 2) / dh;
                int a = s[sx];
                if (a == 0) {
                    if (!transparent && bg != (Color)-1) put_pixel(x + col, oy, bg);
                } else if (a >= 250) {
                    put_pixel(x + col, oy, fg);
                } else if (transparent) {
                    blend_pixel_lin(x + col, oy, fg, a);
                } else {
                    blend_pixel_lin(x + col, oy, fg, a);
                }
            }
        }
        return dh;
    }

    // Draw one CJK glyph by Unicode codepoint (with background).
    // Priority mirrors draw_char() (ASCII): (1) the pre-baked 16x16 8-bit
    // alpha CJK bitmap (Noto Sans CJK, open license) at the native size --
    // baked AA is crisper than runtime rasterization, exactly like English;
    // (2) the stb_truetype vector path for sizes/pieces the baked font does
    // not cover (g_font_px != 16 or a non-GB2312 glyph); (3) the embedded
    // 16x16 1-bit font as the last-resort early-boot fallback.
    // Returns the horizontal advance (pixels).
    int draw_cjk(int x, int y, uint32_t cp, Color fg, Color bg) {
        // (1) Vector outline path (stb_truetype / msyh.ttf).  Microsoft YaHei
        //     ships full CJK outlines, so rasterizing at the native g_font_px is
        //     crisp at every size -- unlike the baked 24x24 bitmap, which the
        //     old priority-1 path had to box-DOWNSAMPLE to 16px, blurring every
        //     Chinese glyph ("矢量字体中文非常模糊").  ASCII already prefers the
        //     vector path (draw_char, line ~2456), so doing the same for CJK
        //     keeps the two consistent and sharp.
        if (g_font_mode != 0 && vec_ready()) {
            int w = 0, h = 0, xo = 0, yo = 0;
            const uint8_t* g = vec_glyph(cp, g_font_px, &w, &h, &xo, &yo);
            if (g) {
                int gx = x + xo;
                int gy = y;   // top-align inside the cell to match ASCII
                if (bg != (Color)-1) fill_rect(gx, gy, w, h, bg);
                // BUG-N2 fix: no 3x3 dilation for CJK -- see the detailed note
                // in draw_cjk_transparent().  At 16px the dilation merges the
                // tightly-spaced Chinese strokes into an unreadable blob.
                for (int row = 0; row < h; row++) {
                    const uint8_t* src = g + row * w;
                    for (int col = 0; col < w; col++) {
                        int a = src[col];
                        if (a > 4)
                            blend_pixel_lin(gx + col, gy + row, fg, a);
                    }
                }
                int adv = vec_advance(cp, g_font_px);
                return adv > g_font_px ? adv : g_font_px;
            }
        }
        // (2) Pre-baked 24x24 8-bit-alpha bitmap from SFS (Noto Sans CJK) -- the
        //     box-downsample to 16px is soft, so it is now a fallback only.
        {
            int adv = blit_cjk_alpha(x, y, cp, fg, bg, false);
            if (adv) return adv;
        }
        // (3) Embedded 16x16 1-bit font: last-resort early-boot fallback.
        int idx = zfont_find_unicode(cp);
        if (idx < 0) {                      // glyph not in embedded set: box
            fill_rect(x, y, 16, 16, bg);
            fill_rect(x, y, 15, 1, fg);
            fill_rect(x, y + 15, 16, 1, fg);
            fill_rect(x, y, 1, 16, fg);
            fill_rect(x + 15, y, 1, 16, fg);
            return 16;
        }
        const uint8_t* g = zfont_glyphs + idx * 32;
        for (int row = 0; row < 16; row++) {
            for (int half = 0; half < 2; half++) {
                uint8_t bits = g[row * 2 + half];
                for (int b = 0; b < 8; b++) {
                    int col = half * 8 + b;
                    Color c = (bits & (0x80 >> b)) ? fg : bg;
                    put_pixel(x + col, y + row, c);
                }
            }
        }
        return 16;
    }

    // Draw CJK glyph transparent (only fg pixels). Returns advance.
    // Priority mirrors draw_cjk(): baked 16x16 Noto bitmap first, then the
    // vector path, then the embedded 1-bit font.
    int draw_cjk_transparent(int x, int y, uint32_t cp, Color fg) {
        // (1) Vector outline path (stb_truetype / msyh.ttf) -- crisp native-size
        //     CJK, same rationale as draw_cjk() above.  This is the primary path
        //     now; the baked 24x24 bitmap is only a fallback.
        if (g_font_mode != 0 && vec_ready()) {
            int w = 0, h = 0, xo = 0, yo = 0;
            const uint8_t* g = vec_glyph(cp, g_font_px, &w, &h, &xo, &yo);
            if (g) {
                int gx = x + xo;
                int gy = y;
                // BUG-N2 fix: do NOT 3x3-dilate CJK.  cov_dil1() is a
                // morphological MAX over the 3x3 neighbourhood -- the "rounded
                // font" look works for Latin, whose strokes have wide gaps, but
                // a 16px Chinese glyph packs its strokes ~1px apart, so the
                // dilation floods the counters and solidifies the glyph into a
                // blob.  Measured on 用/户/名 at 16px: ink density jumps
                // 0.43->0.91, 0.24->0.62, 0.31->0.85 -- i.e. the strokes merge
                // and the character becomes unreadable mush, which is exactly
                // the "blurry Chinese" the user reported.  Render CJK at its
                // true coverage; the AA edges are already smooth.
                for (int row = 0; row < h; row++) {
                    const uint8_t* src = g + row * w;
                    for (int col = 0; col < w; col++) {
                        int a = src[col];
                        if (a > 4) blend_pixel_lin(gx + col, gy + row, fg, a);
                    }
                }
                int adv = vec_advance(cp, g_font_px);
                return adv > g_font_px ? adv : g_font_px;
            }
        }
        // (2) Pre-baked 24x24 8-bit-alpha bitmap from SFS (Noto Sans CJK).
        {
            int adv = blit_cjk_alpha(x, y, cp, fg, (Color)-1, true);
            if (adv) return adv;
        }
        // (3) Embedded 16x16 1-bit font: last-resort early-boot fallback.
        int idx = zfont_find_unicode(cp);
        if (idx < 0) return 16;
        const uint8_t* g = zfont_glyphs + idx * 32;
        for (int row = 0; row < 16; row++) {
            for (int half = 0; half < 2; half++) {
                uint8_t bits = g[row * 2 + half];
                for (int b = 0; b < 8; b++) {
                    if (bits & (0x80 >> b))
                        put_pixel(x + half * 8 + b, y + row, fg);
                }
            }
        }
        return 16;
    }

    // Draw a mixed UTF-8 string (ASCII 8x16 + CJK 16x16) with background.
    void draw_text_utf8(int x, int y, const char* s, Color fg, Color bg) {
        int cx = x;
        while (*s) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) {
                if (c == '\n') { cx = x; y += g_font_h; }
                else { cx += draw_char(cx, y, (char)c, fg, bg); }
                s++;
            } else if ((c & 0xF0) == 0xE0 &&
                       (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
                uint32_t cp = ((c & 0x0F) << 12) |
                              (((unsigned char)s[1] & 0x3F) << 6) |
                              ((unsigned char)s[2] & 0x3F);
                s += 3;
                cx += draw_cjk(cx, y, cp, fg, bg);
            } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
                s += 2;                    // 2-byte UTF-8 (accent, etc.) - skip
            } else s++;
        }
    }

    // Same but transparent (CJK glyphs only draw fg pixels).
    void draw_text_utf8_transparent(int x, int y, const char* s, Color fg) {
        int cx = x;
        while (*s) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) {
                if (c == '\n') { cx = x; y += g_font_h; }
                else { cx += draw_char(cx, y, (char)c, fg, (Color)-1); }
                s++;
            } else if ((c & 0xF0) == 0xE0 &&
                       (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
                uint32_t cp = ((c & 0x0F) << 12) |
                              (((unsigned char)s[1] & 0x3F) << 6) |
                              ((unsigned char)s[2] & 0x3F);
                s += 3;
                cx += draw_cjk_transparent(cx, y, cp, fg);
            } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
                s += 2;
            } else s++;
        }
    }

    void clear_screen(Color c) { fill_rect(0, 0, width, height, c); }

    // Force-zero the entire front-buffer (LFB). Used on GUI entry to wipe
    // any bootuefi/OVMF residue before the GUI draws over it.
    void force_clear_lfb(){
        if(!lfb || !width || !height || !pitch) return;
        uint8_t* fb = (uint8_t*)lfb;
        uint32_t bytes = (uint32_t)pitch * (uint32_t)height;
        for(uint32_t i = 0; i < bytes; i += 4){
            fb[i] = 0; fb[i+1] = 0; fb[i+2] = 0; fb[i+3] = 0;
        }
    }

    // Gradient fill (top to bottom)
    void fill_gradient(int x, int y, int w, int h, Color top, Color bot) {
        if (h <= 0) return;
        int tr = (int)((top >> 16) & 0xFF), tg = (int)((top >> 8) & 0xFF), tb = (int)(top & 0xFF);
        int br = (int)((bot >> 16) & 0xFF), bg = (int)((bot >> 8) & 0xFF), bb = (int)(bot & 0xFF);
        for (int row = 0; row < h; row++) {
            int r = tr + (br - tr) * row / h;
            int g = tg + (bg - tg) * row / h;
            int b = tb + (bb - tb) * row / h;
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            Color c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            fill_rect(x, y + row, w, 1, c);
        }
    }

    // 16.16 fixed-point sample coordinate for the bilinear stretch, computed
    // with 32-bit math only.  The naive form ((idx*src << 16) + (src << 15))/dst
    // needs a 64-bit divide (__divdi3), which a freestanding 32-bit kernel does
    // not link against.  Split into quotient + remainder so every intermediate
    // stays inside int32 -- bit-exact with the 64-bit expression:
    //     N = idx*src*2 + src
    //     result = ((N/dst) << 15) + (((N%dst) << 15) / dst) - 0x8000
    static inline int bilinear_fp(int idx, int src, int dst) {
        int n = idx * src * 2 + src;
        int q = n / dst;
        int r = n % dst;
        return (q << 15) + ((r << 15) / dst) - 0x8000;
    }

    // Stretch-blit a SFS texture into the backbuffer.
    //   fmt 0 = RGB565 (bilinear), fmt 1 = ARGB32 (nearest + alpha blend).
    void draw_image(const TexRec& t, int x, int y, int w, int h) {
        if (!t.loaded || !t.data || w <= 0 || h <= 0) return;
        int sx = x, sy = y, sw = w, sh = h;
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx + sw > (int)width)  sw = width - sx;
        if (sy + sh > (int)height) sh = height - sy;
        if (sw <= 0 || sh <= 0) return;
        const int tw = t.w, th = t.h;
        if (tw <= 0 || th <= 0) return;

        if (t.fmt == 1) {
            const uint8_t* src = t.data;
            for (int row = 0; row < sh; row++) {
                int tyi = (row * th) / sh;
                if (tyi >= th) tyi = th - 1;
                uint32_t* p = backbuffer + (sy + row) * width + sx;
                for (int col = 0; col < sw; col++) {
                    int txi = (col * tw) / sw;
                    if (txi >= tw) txi = tw - 1;
                    const uint8_t* q = src + (tyi * tw + txi) * 4;
                    uint32_t px = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                                  ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
                    int a = (int)((px >> 24) & 0xFF);
                    if (a == 0) continue;
                    uint32_t d = p[col];
                    int dr = (int)((d >> 16) & 0xFF), dg = (int)((d >> 8) & 0xFF), db = (int)(d & 0xFF);
                    int sr = (int)((px >> 16) & 0xFF), sg = (int)((px >> 8) & 0xFF), sb = (int)(px & 0xFF);
                    if (a == 255) { p[col] = px & 0x00FFFFFF; continue; }
                    p[col] = ((uint32_t)((sr * a + dr * (255 - a)) / 255) << 16) |
                             ((uint32_t)((sg * a + dg * (255 - a)) / 255) << 8) |
                             ((uint32_t)((sb * a + db * (255 - a)) / 255));
                }
            }
            return;
        }

        const uint16_t* src = (const uint16_t*)t.data;
        for (int row = 0; row < sh; row++) {
            int fy = bilinear_fp(row, th, sh);
            int y0 = fy >> 16; if (y0 < 0) y0 = 0;
            int y1 = y0 + 1; if (y1 >= th) y1 = th - 1;
            int yf = fy & 0xFFFF;
            const uint16_t* r0 = src + (uint32_t)y0 * tw;
            const uint16_t* r1 = src + (uint32_t)y1 * tw;
            uint32_t* p = backbuffer + (sy + row) * width + sx;
            for (int col = 0; col < sw; col++) {
                int fx = bilinear_fp(col, tw, sw);
                int x0 = fx >> 16; if (x0 < 0) x0 = 0;
                int x1 = x0 + 1; if (x1 >= tw) x1 = tw - 1;
                int xf = fx & 0xFFFF;
                uint16_t c00 = r0[x0], c01 = r0[x1], c10 = r1[x0], c11 = r1[x1];
                int r = tex_bilerp(tex_channel(c00, 0), tex_channel(c01, 0),
                                   tex_channel(c10, 0), tex_channel(c11, 0), xf, yf);
                int g = tex_bilerp(tex_channel(c00, 1), tex_channel(c01, 1),
                                   tex_channel(c10, 1), tex_channel(c11, 1), xf, yf);
                int b = tex_bilerp(tex_channel(c00, 2), tex_channel(c01, 2),
                                   tex_channel(c10, 2), tex_channel(c11, 2), xf, yf);
                p[col] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    // Draw a simple icon (colored square with letter)
    void draw_icon(int x, int y, int sz, Color bg, char letter, Color lc) {
        int r = sz / 4;
        if (r < 2) r = 2;
        // Contact shadow: alpha-blended so the surface behind shows through
        // (mirrors the WinHost Fluent tile; fill_* is hard-replace, so use
        // blend_rounded_rect).
        blend_rounded_rect(x + 1, y + 2, sz, sz, r, 0x000000, 18);
        // Base tile with Win11 rounded corners.
        fill_rounded_rect(x, y, sz, sz, r, bg);
        // Frosted 1px top highlight (subtle blend over the tile edge).
        blend_rounded_rect(x + 1, y + 1, sz - 2, sz - 2, r - 1, 0xFFFFFF, 26);
        // Draw letter centered
        int lx = x + (sz - g_font_w) / 2;
        int ly = y + (sz - g_font_h) / 2;
        draw_char(lx, ly, letter, lc, bg);
    }

    // Draw a progress bar
    void draw_progress(int x, int y, int w, int h, int pct, Color fill) {
        fill_rounded_rect(x, y, w, h, h/2, C_PROGRESS_BG);
        int fw = w * pct / 100;
        if (fw > 0) fill_rounded_rect(x, y, fw, h, h/2, fill);
    }

    // ---- New helper methods for UI refactoring ----

    // Fill a circle (for circular icons) - SDF anti-aliased, no math.h.
    // Inside pixels are opaque; the outer ~1px boundary ring gets partial
    // coverage so the disc no longer shows a hard staircase.
    void fill_circle(int cx, int cy, int r, Color c) {
        if (r <= 0) return;
        int x0 = cx - r - 1, y0 = cy - r - 1, x1 = cx + r + 1, y1 = cy + r + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > width)  x1 = width;
        if (y1 > height) y1 = height;
        if (x1 <= x0 || y1 <= y0) return;
        for (int py = y0; py < y1; py++) {
            int dy = py - cy;
            for (int px = x0; px < x1; px++) {
                int dx   = px - cx;
                int dist = gfx_isqrt(dx * dx + dy * dy);
                int ds   = dist - r;                 // <0 inside, >0 outside
                int cov  = 128 - ds * 256;           // inside -> clamp 256, edge 128, 0.5px AA out
                if (cov <= 0) continue;
                if (cov > 256) cov = 256;
                blend_pixel(px, py, c, cov);
            }
        }
    }

    // Draw circle outline - SDF anti-aliased 1px stroke (no midpoint/ jaggies).
    void draw_circle(int cx, int cy, int r, Color c) {
        if (r <= 0) return;
        int x0 = cx - r - 1, y0 = cy - r - 1, x1 = cx + r + 1, y1 = cy + r + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > width)  x1 = width;
        if (y1 > height) y1 = height;
        if (x1 <= x0 || y1 <= y0) return;
        for (int py = y0; py < y1; py++) {
            int dy = py - cy;
            for (int px = x0; px < x1; px++) {
                int dx   = px - cx;
                int dist = gfx_isqrt(dx * dx + dy * dy);
                int ds   = dist - r;
                int ad   = (ds < 0) ? -ds : ds;       // |distance to boundary|
                int cov  = 256 - ad * 256;            // 1px stroke: centre full, edge half
                if (cov <= 0) continue;
                if (cov > 256) cov = 256;
                blend_pixel(px, py, c, cov);
            }
        }
    }

    // Measure rendered text width using the real per-glyph advance (so
    // centering matches the actual draw, not a fixed-width assumption that
    // could let glyphs visually overlap).
    int text_width(const char* s) {
        int line_width = 0;
        int max_width = 0;
        const char* p = s;
        while (*p) {
            unsigned char c = (unsigned char)*p;
            // FIX(P2-1): drawing resets its x position at each newline.
            if (c == '\n') {
                if (line_width > max_width) max_width = line_width;
                line_width = 0;
                p++;
                continue;
            }
            if (c < 0x80) {
                if (g_font_mode != 0 && g_font_px == AFONT_H &&
                    c >= AFONT_FIRST && c < AFONT_FIRST + AFONT_COUNT)
                    line_width += afont_adv[c - AFONT_FIRST];
                else
                    line_width += vec_ready() ? vec_advance(c, g_font_px) : g_font_w;
                p++;
            } else if ((c & 0xE0) == 0xC0) { line_width += g_font_h; p += 2; }
            else if ((c & 0xF0) == 0xE0) {
                line_width += g_font_h;
                p += 3;
            }
            else { line_width += g_font_w; p++; }
        }
        return line_width > max_width ? line_width : max_width;
    }

    // Draw text centered in a region
    void draw_text_centered(int x, int y, int w, const char* s, Color fg) {
        int tw = text_width(s);
        int sx = x + (w - tw) / 2;
        if (sx < x) sx = x;
        draw_text_transparent(sx, y, s, fg);
    }

    // Draw text centered with background
    void draw_text_centered_bg(int x, int y, int w, const char* s, Color fg, Color bg) {
        int tw = text_width(s);
        int sx = x + (w - tw) / 2;
        if (sx < x) sx = x;
        draw_text(sx, y, s, fg, bg);
    }

    // Draw a card with shadow and rounded corners
    void draw_card(int x, int y, int w, int h, Color bg) {
        // Shadow
        for (int i = 0; i < 3; i++) {
            fill_rect(x + 2 + i, y + 2 + i, w, h, 0x10000000);
        }
        // Card background
        fill_rounded_rect(x, y, w, h, 6, bg);
    }

    // Draw a circular category icon with letter
    void draw_category_icon(int cx, int cy, int r, Color bg, char letter) {
        fill_circle(cx, cy, r, bg);
        // Draw letter centered
        int lx = cx - g_font_w / 2;
        int ly = cy - g_font_h / 2;
        draw_char(lx, ly, letter, COLOR_WHITE, bg);
    }

    // Draw a search bar (input field with magnifying glass icon)
    void draw_search_bar(int x, int y, int w, int h, const char* placeholder,
                         const char* text, int text_len, bool focused) {
        Color border = focused ? C_ACCENT : C_TM_SEARCH_BORDER;
        fill_rounded_rect(x, y, w, h, h/2, C_TM_SEARCH_BG);
        draw_rounded_rect(x, y, w, h, h/2, border);
        // Magnifying glass icon (simple circle + handle)
        int ix = x + 10, iy = y + h/2;
        draw_circle(ix, iy, 4, C_WIN_TEXT_SEC);
        draw_line(ix + 3, iy + 3, ix + 7, iy + 7, C_WIN_TEXT_SEC);
        // Text or placeholder
        int tx = x + 22;
        if (text_len > 0) {
            char tmp[128];
            int copy_len = text_len < 127 ? text_len : 127;
            memcpy_(tmp, text, copy_len);
            tmp[copy_len] = 0;
            draw_text_transparent(tx, y + (h - g_font_h) / 2, tmp, C_WIN_TEXT);
        } else if (placeholder) {
            draw_text_transparent(tx, y + (h - g_font_h) / 2, placeholder, C_WIN_TEXT_SEC);
        }
        // Cursor if focused
        if (focused) {
            int cx = tx + text_len * g_font_w;
            fill_rect(cx, y + 4, 1, h - 8, C_ACCENT);
        }
    }

    // Draw a table header row
    void draw_table_header(int x, int y, int w, int h, const char* headers[],
                           int col_widths[], int ncols) {
        fill_rect(x, y, w, h, C_TM_HEADER_BG);
        draw_line(x, y + h - 1, x + w, y + h - 1, C_WIN_BORDER);
        int cx = x + 4;
        for (int i = 0; i < ncols; i++) {
            draw_text_transparent(cx, y + (h - g_font_h) / 2, headers[i], C_TM_HEADER_TEXT);
            cx += col_widths[i];
        }
    }

    // Draw a shortcut tile (icon circle + label below)
    void draw_shortcut_tile(int x, int y, int w, int h, Color icon_color,
                            char letter, const char* label, bool hover) {
        Color bg = hover ? C_CARD_HOVER : C_CARD_BG;
        fill_rounded_rect(x, y, w, h, 8, bg);
        if (hover) draw_rounded_rect(x, y, w, h, 8, C_CARD_BORDER);
        // Icon circle
        int icon_r = 16;
        int icx = x + w / 2;
        int icy = y + 18;
        fill_circle(icx, icy, icon_r, icon_color);
        // Letter
        int lx = icx - g_font_w / 2;
        int ly = icy - g_font_h / 2;
        draw_char(lx, ly, letter, COLOR_WHITE, icon_color);
        // Label
        draw_text_centered(x, y + 38, w, label, C_SHORTCUT_TEXT);
    }

    // Draw a navigation tab
    void draw_nav_tab(int x, int y, int w, int h, const char* label, bool selected) {
        Color bg = selected ? C_PORTAL_TAB_SEL : C_PORTAL_TAB_BG;
        Color fg = selected ? C_PORTAL_TAB_TEXT_SEL : C_PORTAL_TAB_TEXT;
        fill_rounded_rect(x, y, w, h, 6, bg);
        draw_text_centered_bg(x, y + (h - g_font_h) / 2, w, label, fg, bg);
    }
};
