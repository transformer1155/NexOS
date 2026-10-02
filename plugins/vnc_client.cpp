/* vnc_client.cpp - VNC RFB 3.8 Client Plugin for NexOS
 *
 * Connects to a remote VNC server and mirrors its framebuffer into NexOS GUI.
 * Uses net_guest_* TCP API (extern "C") for transport.
 *
 * Protocol: RFB 3.8 (Remote Framebuffer Protocol)
 * Supports: Raw encoding, CopyRect, RRE, Hextile
 *
 * Services:
 *   vnc.connect          args: "host:port" string
 *   vnc.disconnect
 *   vnc.poll             out: 1=frame received, 0=no data
 *   vnc.request_frame
 *   vnc.input            args: int[4] = {mx, my, buttons, key}
 *   vnc.status           out: "connected"/"disconnected"
 */
#include "plugin_manager.h"
#include "nexos_api.h"

/* ---- Kernel TCP API (extern from net.cpp) ------------------------------ */
extern "C" int  net_guest_connect(uint32_t ip, uint16_t port);
extern "C" int  net_guest_send(const void* data, int len);
extern "C" int  net_guest_recv(void* buf, int len);
extern "C" void net_guest_close(void);
extern "C" void net_log(const char* s);  /* serial output */

/* ---- Kernel heap (extern from kernel.cpp) ----------------------------- */
extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);

/* ---- GUI backend (cached after vnc_init) ------------------------------ */
static const NexosGuiAPI* g_gui_api = 0;
/* Bulk-blit service "gui.blit_pixels" (set in vnc_init).  When present we
 * copy a whole rectangle per call instead of one put_pixel() at a time. */
static svc_fn g_blit_svc = 0;

/* ---- freestanding helpers (no libc) ----------------------------------- */
static int my_strlen(const char* s){ int n=0; while(s[n]) n++; return n; }

static void my_memcpy(void* dst, const void* src, int n){
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (int i=0;i<n;i++) d[i]=s[i];
}
static void my_memmove(void* dst, const void* src, int n){
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d==s) return;
    if (d<s) for (int i=0;i<n;i++) d[i]=s[i];
    else     for (int i=n-1;i>=0;i--) d[i]=s[i];
}

static void log_str(const char* prefix, const char* s){
    net_log("[VNC] ");
    if (prefix) net_log(prefix);
    if (s) net_log(s);
    net_log("\n");
}

static void log_hex(const char* prefix, uint32_t v){
    char buf[32];
    int n = 0;
    const char* h = "0123456789ABCDEF";
    buf[n++]='0'; buf[n++]='x';
    for (int i=28;i>=0;i-=4) buf[n++]=h[(v>>i)&0xF];
    buf[n]=0;
    net_log("[VNC] ");
    if (prefix) { net_log(prefix); net_log("="); }
    net_log(buf);
    net_log("\n");
}

/* ---- VNC Protocol Constants ------------------------------------------- */
#define VNC_AUTH_NONE           1
#define VNC_AUTH_VNC            2
#define VNC_AUTH_FAIL           0
#define VNC_CLIENT_INIT_FLAG    1

/* Client-to-Server messages */
#define VNC_SET_PIXEL_FORMAT     0
#define VNC_SET_ENCODINGS       2
#define VNC_FRAMEBUFFER_REQUEST 3
#define VNC_KEY_EVENT           4
#define VNC_POINTER_EVENT       5

/* Server-to-Client messages */
#define VNC_FRAMEBUFFER_UPDATE  0

/* Encodings */
#define VNC_ENC_RAW             0
#define VNC_ENC_COPYRECT        1
#define VNC_ENC_RRE             2
#define VNC_ENC_CORRE           4
#define VNC_ENC_HEXTILE         5
#define VNC_ENC_ZLIB            6
#define VNC_ENC_TIGHT           7

/* ---- VNC Client State -------------------------------------------------- */
struct VncState {
    int      connected;
    uint32_t server_ip;
    uint16_t server_port;
    uint16_t fb_width;
    uint16_t fb_height;
    uint8_t  bpp;
    uint8_t  depth;
    uint8_t  big_endian;
    uint8_t  true_color;
    uint16_t red_max, green_max, blue_max;
    uint8_t  red_shift, green_shift, blue_shift;
    uint8_t  server_name[128];
};

static struct VncState g_vnc = {0};

/* ---- low-level I/O ---------------------------------------------------- */
static int vnc_read_exact(void* buf, int len) {
    int got = 0;
    uint8_t* p = (uint8_t*)buf;
    while (got < len) {
        int r = net_guest_recv(p + got, len - got);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

static int vnc_write_all(const void* data, int len) {
    int sent = 0;
    const uint8_t* p = (const uint8_t*)data;
    while (sent < len) {
        int r = net_guest_send(p + sent, len - sent);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

/* ---- connect + handshake ---------------------------------------------- */
static int parse_host_port(const char* s, uint32_t* out_ip, uint16_t* out_port) {
    *out_port = 5900;
    uint8_t octets[4] = {0};
    int idx = 0, val = 0;
    int has_port = 0;

    const char* colon = 0;
    for (const char* p = s; *p; p++)
        if (*p == ':' && (p == s || p[-1] != ':')) { colon = p; break; }

    for (const char* p = s; *p; p++) {
        if (colon && p >= colon) {
            if (*p == ':') continue;
            if (*p >= '0' && *p <= '9') {
                *out_port = *out_port * 10 + (*p - '0');
                has_port = 1;
            }
            continue;
        }
        if (*p == '.') {
            if (idx >= 4) return -1;
            octets[idx++] = (uint8_t)val;
            val = 0;
        } else if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
        } else {
            return -1;
        }
    }
    if (idx < 4) return -1;
    octets[3] = (uint8_t)val;
    *out_ip = ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
              ((uint32_t)octets[2] << 8)  | (uint32_t)octets[3];
    if (!has_port) *out_port = 5900;
    return 0;
}

static int vnc_handshake(void) {
    /* --- Protocol version (server sends "RFB 003.008\n") --- */
    uint8_t version[12];
    if (vnc_read_exact(version, 12) != 0) {
        log_str("error", "reading version");
        return -1;
    }
    version[11] = 0;
    log_str("server version: ", (const char*)version);

    /* Send our version */
    const char our_ver[] = "RFB 003.008\n";
    vnc_write_all(our_ver, 12);

    /* --- Authentication --- */
    uint8_t auth_type;
    if (vnc_read_exact(&auth_type, 1) != 0) {
        log_str("error", "reading auth");
        return -1;
    }

    if (auth_type == VNC_AUTH_NONE) {
        log_str("auth", "none");
    } else if (auth_type == VNC_AUTH_VNC) {
        log_str("auth", "VNC (sending empty)");
        uint8_t pw[16] = {0};
        vnc_write_all(pw, 16);
        uint32_t result;
        if (vnc_read_exact(&result, 4) != 0) return -1;
        if (result != 0) {
            log_str("error", "VNC auth failed");
            return -1;
        }
    } else {
        log_hex("unknown auth", auth_type);
        return -1;
    }

    /* --- Client Init --- */
    uint8_t cinit = VNC_CLIENT_INIT_FLAG;  /* shared */
    vnc_write_all(&cinit, 1);

    /* --- Server Init (24 bytes fixed + name) --- */
    uint8_t si[24];
    if (vnc_read_exact(si, 24) != 0) {
        log_str("error", "reading server init");
        return -1;
    }
    g_vnc.fb_width   = (si[0] << 8) | si[1];
    g_vnc.fb_height  = (si[2] << 8) | si[3];
    g_vnc.bpp        = si[4];
    g_vnc.depth      = si[5];
    g_vnc.big_endian = si[6];
    g_vnc.true_color = si[7];
    g_vnc.red_max    = (si[8] << 8) | si[9];
    g_vnc.green_max  = (si[10] << 8) | si[11];
    g_vnc.blue_max   = (si[12] << 8) | si[13];
    g_vnc.red_shift   = si[14];
    g_vnc.green_shift = si[15];
    g_vnc.blue_shift  = si[16];

    /* Server name */
    uint8_t nlf[4];
    if (vnc_read_exact(nlf, 4) != 0) return -1;
    uint32_t nlen = ((uint32_t)nlf[0] << 24) | ((uint32_t)nlf[1] << 16) |
                    ((uint32_t)nlf[2] << 8)  | (uint32_t)nlf[3];
    if (nlen > 127) nlen = 127;
    if (nlen > 0) {
        vnc_read_exact(g_vnc.server_name, nlen);
    }
    g_vnc.server_name[nlen] = 0;

    log_str("server: ", (const char*)g_vnc.server_name);
    log_hex("fb", (g_vnc.fb_width << 16) | g_vnc.fb_height);
    return 0;
}

static int vnc_send_prefs(void) {
    /* Set Pixel Format (20 bytes) */
    uint8_t fmt[20];
    fmt[0] = VNC_SET_PIXEL_FORMAT;
    fmt[1] = fmt[2] = fmt[3] = 0;
    fmt[4] = 32;    /* bpp */
    fmt[5] = 24;    /* depth */
    fmt[6] = 0;     /* big-endian */
    fmt[7] = 1;     /* true-color */
    fmt[8]  = 0; fmt[9]  = 0xFF;  /* red max */
    fmt[10] = 0; fmt[11] = 0xFF;  /* green max */
    fmt[12] = 0; fmt[13] = 0xFF;  /* blue max */
    fmt[14] = 16;   /* red shift */
    fmt[15] = 8;    /* green shift */
    fmt[16] = 0;    /* blue shift */
    fmt[17] = fmt[18] = fmt[19] = 0;
    vnc_write_all(fmt, 20);

    /* Set Encodings: Hextile + RRE, with Raw as the guaranteed fallback.
     * We deliberately do NOT request Zlib(6)/Tight(7): the kernel has no
     * zlib/inflate and cannot decode them.  Negotiating an efficient encoding
     * is the client-side half of fixing the "Raw-only" bandwidth blow-up
     * documented in docs/PERFORMANCE.md (stage 3). */
    uint8_t enc[4 + 3 * 4];
    enc[0] = VNC_SET_ENCODINGS;
    enc[1] = enc[2] = 0;
    enc[3] = 3;                         /* count = 3 */
    enc[4]  = enc[5]  = enc[6]  = 0; enc[7]  = VNC_ENC_HEXTILE;  /* pref 1 */
    enc[8]  = enc[9]  = enc[10] = 0; enc[11] = VNC_ENC_RRE;      /* pref 2 */
    enc[12] = enc[13] = enc[14] = 0; enc[15] = VNC_ENC_RAW;      /* fallback */
    vnc_write_all(enc, sizeof(enc));

    return 0;
}

static int vnc_send_fb_request(void) {
    uint8_t req[10];
    req[0] = VNC_FRAMEBUFFER_REQUEST;
    req[1] = 0;
    req[2] = req[3] = 0;
    req[4] = req[5] = 0;
    req[6] = (g_vnc.fb_width >> 8) & 0xFF;
    req[7] = g_vnc.fb_width & 0xFF;
    req[8] = (g_vnc.fb_height >> 8) & 0xFF;
    req[9] = g_vnc.fb_height & 0xFF;
    return vnc_write_all(req, 10);
}

static int vnc_send_pointer(uint8_t buttons, uint16_t x, uint16_t y) {
    uint8_t p[6];
    p[0] = VNC_POINTER_EVENT;
    p[1] = buttons;
    p[2] = (x >> 8) & 0xFF; p[3] = x & 0xFF;
    p[4] = (y >> 8) & 0xFF; p[5] = y & 0xFF;
    return vnc_write_all(p, 6);
}

static int vnc_send_key(uint8_t down, uint32_t keysym) {
    uint8_t k[8];
    k[0] = VNC_KEY_EVENT;
    k[1] = down ? 1 : 0;
    k[2] = k[3] = 0;
    k[4] = (keysym >> 24) & 0xFF;
    k[5] = (keysym >> 16) & 0xFF;
    k[6] = (keysym >> 8) & 0xFF;
    k[7] = keysym & 0xFF;
    return vnc_write_all(k, 8);
}

/* ---- Blit a pixel buffer into NexOS GUI --------------------------------
 * buf layout: Raw RFB pixel data as configured by SetPixelFormat:
 *   bpp=32, depth=24, big_endian=0, red_shift=16, green_shift=8, blue_shift=0
 * => each pixel is 4 bytes little-endian, read as uint32 = 0x00RRGGBB,
 *    which matches NexOS Color (ARGB, A=0 opaque).
 *
 * Fast path: when the "gui.blit_pixels" service is available we copy the whole
 * rectangle per call (a row-wise 32-bit copy into the backbuffer).  Fallback:
 * when that service is absent we fall back to one put_pixel() per pixel so the
 * plugin still works against older GUI builds.                      */
struct VncBlitArgs { int x, y, w, h; const uint32_t* pixels; };
static void vnc_blit_raw(uint16_t x_start, uint16_t y_start,
                         uint16_t w, uint16_t h, const uint8_t* buf) {
    const uint32_t* pixels = (const uint32_t*)buf;
    if (g_blit_svc) {
        VncBlitArgs a;
        a.x = (int)x_start; a.y = (int)y_start;
        a.w = (int)w; a.h = (int)h;
        a.pixels = pixels;
        g_blit_svc(&a, 0, 0);
        return;
    }
    /* Fallback: per-pixel (older GUI without gui.blit_pixels). */
    if (!g_gui_api || !g_gui_api->put_pixel) return;
    for (uint16_t yy = 0; yy < h; yy++) {
        for (uint16_t xx = 0; xx < w; xx++) {
            g_gui_api->put_pixel((int)(x_start + xx),
                                 (int)(y_start + yy),
                                 pixels[yy * w + xx]);
        }
    }
}

/* ---- pixel / fill helpers (stage-3 decode support) --------------------
 * vnc_read_pixel() reconstructs a 32-bit Color the SAME way the Raw path does
 * (direct little-endian cast of the 4 wire bytes), so RRE/Hextile fills stay
 * colour-consistent with Raw blits.  vnc_fill_rect() paints a solid rectangle
 * through the GUI vtable (same backbuffer the blit writes).                */
static uint32_t vnc_read_pixel(void) {
    uint8_t b[4];
    if (vnc_read_exact(b, 4) != 0) return 0;
    return ((uint32_t)b[0]) | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static void vnc_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t color) {
    if (g_gui_api && g_gui_api->fill_rect)
        g_gui_api->fill_rect((int)x, (int)y, (int)w, (int)h, (Color)color);
}

/* RRE (rise-and-run-length): one background fill + N solid sub-rectangles. */
static int vnc_decode_rre(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint32_t bg = vnc_read_pixel();
    uint8_t nbuf[4];
    if (vnc_read_exact(nbuf, 4) != 0) return -1;
    uint32_t n = ((uint32_t)nbuf[0] << 24) | ((uint32_t)nbuf[1] << 16) |
                 ((uint32_t)nbuf[2] << 8) | (uint32_t)nbuf[3];
    vnc_fill_rect(x, y, w, h, bg);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t pix = vnc_read_pixel();
        uint8_t s[8];
        if (vnc_read_exact(s, 8) != 0) return -1;
        uint16_t rx = (uint16_t)(((uint16_t)s[0] << 8) | s[1]);
        uint16_t ry = (uint16_t)(((uint16_t)s[2] << 8) | s[3]);
        uint16_t rw = (uint16_t)(((uint16_t)s[4] << 8) | s[5]);
        uint16_t rh = (uint16_t)(((uint16_t)s[6] << 8) | s[7]);
        vnc_fill_rect((uint16_t)(x + rx), (uint16_t)(y + ry), rw, rh, pix);
    }
    return 0;
}

/* Hextile: 16x16 tiles, each optionally Raw / bg / fg / coloured sub-rects. */
static int vnc_decode_hextile(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint32_t bg = 0, fg = 0;               /* persist across tiles */
    uint16_t tx = 0;
    while (tx < w) {
        uint16_t tw = (w - tx > 16) ? 16 : (uint16_t)(w - tx);
        uint16_t ty = 0;
        while (ty < h) {
            uint16_t th = (h - ty > 16) ? 16 : (uint16_t)(h - ty);
            uint8_t mask;
            if (vnc_read_exact(&mask, 1) != 0) return -1;
            if (mask & 0x01) {             /* Raw tile */
                int n = (int)tw * (int)th * 4;
                uint8_t* buf = (uint8_t*)kmalloc((uint32_t)n);
                if (!buf) return -1;
                int rem = n;
                while (rem > 0) {
                    int c = rem > 4096 ? 4096 : rem;
                    if (vnc_read_exact(buf + (n - rem), c) != 0) { kfree(buf); return -1; }
                    rem -= c;
                }
                vnc_blit_raw((uint16_t)(x + tx), (uint16_t)(y + ty), tw, th, buf);
                kfree(buf);
                ty += 16;
                continue;
            }
            if (mask & 0x02) bg = vnc_read_pixel();   /* BackgroundSpecified */
            if (mask & 0x04) fg = vnc_read_pixel();   /* ForegroundSpecified */
            /* RFB 3.8 §6.5.6: the ENTIRE tile is painted with the background
               colour first (bg persists across tiles when BackgroundSpecified
               is clear), then subrectangles are drawn on top. Skipping the
               fill when subrects are present would leave stale pixels. */
            vnc_fill_rect((uint16_t)(x + tx), (uint16_t)(y + ty), tw, th, bg);
            if (mask & 0x08) {                        /* AnySubrects */
                uint8_t cnt;
                if (vnc_read_exact(&cnt, 1) != 0) return -1;
                for (uint8_t s = 0; s < cnt; s++) {
                    uint32_t col = fg;
                    if (mask & 0x10) col = vnc_read_pixel();   /* SubrectsColoured */
                    uint8_t sub[2];
                    if (vnc_read_exact(sub, 2) != 0) return -1;
                    uint16_t rx = (uint16_t)((sub[0] >> 4) & 0xF);
                    uint16_t ry = (uint16_t)(sub[0] & 0xF);
                    uint16_t rw = (uint16_t)(((sub[1] >> 4) & 0xF) + 1);
                    uint16_t rh = (uint16_t)((sub[1] & 0xF) + 1);
                    vnc_fill_rect((uint16_t)(x + tx + rx), (uint16_t)(y + ty + ry),
                                  rw, rh, col);
                }
            }
            ty += 16;
        }
        tx += 16;
    }
    return 0;
}

/* ---- Handle one Framebuffer Update ------------------------------------ */
static int vnc_handle_fb_update(void) {
    uint8_t hdr[4];
    if (vnc_read_exact(hdr, 4) != 0) return -1;
    uint16_t nrects = (uint16_t)((hdr[2] << 8) | hdr[3]);

    for (uint16_t i = 0; i < nrects; i++) {
        uint8_t rh[12];
        if (vnc_read_exact(rh, 12) != 0) return -1;
        uint16_t x = (uint16_t)((rh[0] << 8) | rh[1]);
        uint16_t y = (uint16_t)((rh[2] << 8) | rh[3]);
        uint16_t w = (uint16_t)((rh[4] << 8) | rh[5]);
        uint16_t h = (uint16_t)((rh[6] << 8) | rh[7]);
        int32_t enc = ((int32_t)rh[8] << 24) | ((int32_t)rh[9] << 16) |
                      ((int32_t)rh[10] << 8) | (int32_t)rh[11];

        if (enc == VNC_ENC_RAW) {
            int bytes = (int)w * (int)h * 4;
            if (bytes <= 0) continue;
            uint8_t* buf = (uint8_t*)kmalloc((uint32_t)bytes);
            if (!buf) { log_str("error", "kmalloc failed"); return -1; }
            int remaining = bytes;
            while (remaining > 0) {
                int to_read = remaining > 4096 ? 4096 : remaining;
                if (vnc_read_exact(buf + (bytes - remaining), to_read) != 0) {
                    kfree(buf); return -1;
                }
                remaining -= to_read;
            }
            vnc_blit_raw(x, y, w, h, buf);
            kfree(buf);
        } else if (enc == VNC_ENC_RRE) {
            if (vnc_decode_rre(x, y, w, h) != 0) return -1;
        } else if (enc == VNC_ENC_HEXTILE) {
            if (vnc_decode_hextile(x, y, w, h) != 0) return -1;
        } else if (enc == VNC_ENC_COPYRECT) {
            /* Not requested (see vnc_send_prefs); skip defensively. */
            uint8_t src[4];
            vnc_read_exact(src, 4);
            log_hex("copyskip enc", (uint32_t)enc);
        } else {
            log_hex("skip enc", (uint32_t)enc);
            return -1;
        }
    }
    return 0;
}

/* ---- public services ------------------------------------------------- */
static int svc_connect(void* args, void* out, int outcap) {
    if (g_vnc.connected) {
        net_guest_close();
        g_vnc.connected = 0;
    }

    const char* host_port = args ? (const char*)args : "10.0.2.2:5900";
    uint32_t ip;
    uint16_t port;
    if (parse_host_port(host_port, &ip, &port) != 0) {
        log_str("error", "bad host:port");
        return -1;
    }
    g_vnc.server_ip = ip;
    g_vnc.server_port = port;

    log_str("connecting to ", host_port);
    if (net_guest_connect(ip, port) != 0) {
        log_str("error", "TCP connect failed");
        return -1;
    }
    log_str("TCP", "connected");

    if (vnc_handshake() != 0) return -1;
    if (vnc_send_prefs() != 0) return -1;

    g_vnc.connected = 1;
    log_str("OK", "connected");
    vnc_send_fb_request();
    return 0;
}

static int svc_disconnect(void* args, void* out, int outcap) {
    (void)args; (void)out; (void)outcap;
    net_guest_close();
    g_vnc.connected = 0;
    log_str("OK", "disconnected");
    return 0;
}

static int svc_poll(void* args, void* out, int outcap) {
    (void)args;
    if (!g_vnc.connected) return -1;
    int r = vnc_handle_fb_update();
    if (out && outcap >= 4) *(int*)out = (r == 0) ? 1 : 0;
    return r;
}

static int svc_request_frame(void* args, void* out, int outcap) {
    (void)args; (void)out; (void)outcap;
    if (!g_vnc.connected) return -1;
    return vnc_send_fb_request();
}

static int svc_input(void* args, void* out, int outcap) {
    (void)out; (void)outcap;
    if (!g_vnc.connected || !args) return -1;
    int32_t* a = (int32_t*)args;
    vnc_send_pointer((uint8_t)a[2], (uint16_t)a[0], (uint16_t)a[1]);
    if (a[3]) vnc_send_key(1, (uint32_t)a[3]);
    return 0;
}

static int svc_status(void* args, void* out, int outcap) {
    (void)args;
    if (out && outcap > 0) {
        const char* s = g_vnc.connected ? "connected" : "disconnected";
        char* o = (char*)out;
        int n = 0;
        while (s[n] && n < outcap-1) { o[n] = s[n]; n++; }
        o[n] = 0;
    }
    return 0;
}

/* ---- dispatch -------------------------------------------------------- */
static int vnc_call(Plugin* self, const char* method,
                    void* args, void* out, int outcap) {
    (void)self;
    if (!method) return -1;
    if (pm_strcmp(method, "connect") == 0)       return svc_connect(args, out, outcap);
    if (pm_strcmp(method, "disconnect") == 0)     return svc_disconnect(args, out, outcap);
    if (pm_strcmp(method, "poll") == 0)          return svc_poll(args, out, outcap);
    if (pm_strcmp(method, "request_frame") == 0) return svc_request_frame(args, out, outcap);
    if (pm_strcmp(method, "input") == 0)         return svc_input(args, out, outcap);
    if (pm_strcmp(method, "status") == 0)        return svc_status(args, out, outcap);
    return -2;
}

static int vnc_init(Plugin* self) {
    (void)self;
    g_vnc.connected = 0;
    /* Cache the GUI vtable (nexos.gui.api is published by gui.cpp at boot). */
    g_gui_api = (const NexosGuiAPI*)svc_lookup(NEXOS_GUI_API_SVC);
    if (!g_gui_api || !g_gui_api->put_pixel) {
        log_str("WARN", "no nexos.gui.api — blit will no-op");
    } else {
        log_str("OK", "gui.api ready");
    }
    /* Cache the bulk-blit service (stage-2 optimization).  Falls back to
     * per-pixel put_pixel when the service is unavailable. */
    g_blit_svc = svc_lookup("gui.blit_pixels");
    if (g_blit_svc) log_str("OK", "gui.blit_pixels ready");
    else             log_str("WARN", "no gui.blit_pixels — per-pixel fallback");
    log_str("OK", "init");
    return 0;
}

static void vnc_exit(Plugin* self) {
    (void)self;
    if (g_vnc.connected) net_guest_close();
    g_vnc.connected = 0;
    log_str("OK", "exit");
}

extern "C" const Plugin g_vnc_client = {
    .name     = "vnc_client",
    .version  = 0x0100,
    .provides = "vnc.connect,vnc.disconnect,vnc.poll,vnc.request_frame,vnc.input,vnc.status",
    .deps     = "",
    .init     = vnc_init,
    .exit     = vnc_exit,
    .call     = vnc_call,
};
