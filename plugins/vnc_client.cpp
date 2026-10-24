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
extern "C" int  net_guest_connect_async(uint32_t ip, uint16_t port);
extern "C" int  net_guest_connect_state(void);   /* 0 idle,1 connecting,2 established,-1 error */
extern "C" void net_guest_connect_abort(void);
extern "C" int  net_guest_send(const void* data, int len);
extern "C" int  net_guest_recv_nowait(void* buf, int len);
extern "C" int  net_guest_peek(void);
extern "C" void net_guest_close(void);
extern "C" void net_poll(void);
extern "C" void net_log(const char* s);  /* serial output */

/* ---- Forward declarations (used before their definitions) -------------- */
static int  vnc_send_prefs(void);
extern "C" void vnc_disconnect(void);
extern "C" int  vnc_connect(uint32_t ip, uint16_t port);

/* ---- Kernel heap (extern from kernel.cpp) ----------------------------- */
extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);
/* Framebuffer-class buffers (>1 MiB) come from bigbuf (PMM-backed, outside
 * the tight GUI heap) so they don't trigger the [HEAP] OOM diagnostic. */
extern "C" void* bigbuf_alloc(uint32_t size);
extern "C" void  bigbuf_free(void* ptr);

/* ---- GUI backend (cached after vnc_init) ------------------------------ */
static const NexosGuiAPI* g_gui_api = 0;
/* Bulk-blit service "gui.blit_pixels" (set in vnc_init).  When present we
 * copy a whole rectangle per call instead of one put_pixel() at a time. */
static svc_fn g_blit_svc = 0;
/* NexOS framebuffer dimensions (queried from "gui.screen_size").  Used to
 * boundary-check every Raw rectangle so a frame larger than / off-screen vs
 * the local VRAM can never scribble past the backbuffer.  Fall back to the
 * remote fb size if the service is unavailable (still a finite bound). */
static int g_screen_w = 0;
static int g_screen_h = 0;

/* ---- freestanding helpers (no libc) ----------------------------------- */
static int my_strlen(const char* s){ int n=0; while(s[n]) n++; return n; }

/* unsigned decimal -> ASCII into buf, returns length (no NUL terminator
 * written), so it can be chained.  Used to format an IP for logging. */
static int my_itoa(char* buf, uint32_t v){
    char tmp[16]; int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    int n = 0;
    while (t) buf[n++] = tmp[--t];   /* reverse */
    return n;
}

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
#define VNC_BELL                2
#define VNC_SERVER_CUT_TEXT     3

/* Encodings */
#define VNC_ENC_RAW             0
#define VNC_ENC_COPYRECT        1
#define VNC_ENC_RRE             2
#define VNC_ENC_CORRE           4
#define VNC_ENC_HEXTILE         5
#define VNC_ENC_ZLIB            6
#define VNC_ENC_TIGHT           7

/* ---- VNC Client State -------------------------------------------------- */
/* Connection is driven as a NON-BLOCKING state machine so a single GUI tick
 * can advance it without ever freezing the desktop.  net_guest_connect() in
 * net.cpp BLOCKS the caller for up to 5M poll iterations, so we must NOT call
 * it from the render loop; instead we send the SYN (via a new async connect
 * primitive) and then poll completion from vnc_poll(). */
typedef enum {
    VNC_ST_IDLE = 0,
    VNC_ST_CONNECTING,    /* SYN sent, awaiting ESTABLISHED */
    VNC_ST_HANDSHAKE,     /* socket up, RFB handshake in progress */
    VNC_ST_RUNNING,       /* established; framebuffer updates flow */
    VNC_ST_ERROR
} VncConnState;

struct VncState {
    int      connected;        /* 1 once VNC_ST_RUNNING */
    VncConnState cstate;
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

    /* Shared decode buffer: RFB pixels are decoded here (0x00RRGGBB) and the
     * GUI compositor (draw_linux_guest) blits from it per-window.  Owned by
     * the vnc plugin; allocated lazily once ServerInit reports the size. */
    uint32_t* fb;
    int       fb_cap;          /* capacity in pixels */
    /* handshake step counter for the non-blocking handshake driver */
    int       hs_step;
};

static struct VncState g_vnc = {0};

/* ---- async TCP connect primitive (declared in net.cpp) ----------------- */
extern "C" int  net_guest_connect_async(uint32_t ip, uint16_t port);
extern "C" int  net_guest_connect_state(void);  /* 0=idle 1=connecting 2=established -1=error */
extern "C" void net_guest_connect_abort(void);

/* ---- low-level I/O ---------------------------------------------------- */
/* The VNC plugin runs inside the NexOS render loop, so it must NEVER block.
 * net_guest_recv_nowait() copies only bytes already buffered and returns 0
 * when nothing has arrived yet (no pumping, no spinning).  To make forward
 * progress we call net_poll() ONCE before each non-blocking read so a
 * pending packet is pulled into the RX buffer.  This caps each read at one
 * network tick and keeps the handshake driven entirely by vnc_poll().  A
 * `guard` bound protects against a stalled peer so we never spin forever. */
extern "C" int net_guest_recv_nowait(void* buf, int len);

static int vnc_recv_buf(void* buf, int len) {
    int got = 0;
    uint8_t* p = (uint8_t*)buf;
    int guard = 0;
    while (got < len) {
        int r = net_guest_recv_nowait(p + got, len - got);
        if (r < 0) return -1;            /* disconnected */
        if (r == 0) {
            if (++guard > 1024) return -1;   /* stalled: give up */
            net_poll();                       /* one tick to pull a packet */
            continue;
        }
        got += r;
        guard = 0;
    }
    return 0;
}

/* Read exactly len bytes (non-blocking, one-tick-per-miss).  Returns 0 on
 * success, -1 on disconnect or stall. */
static int vnc_read_exact(void* buf, int len) {
    return vnc_recv_buf(buf, len);
}

static int vnc_write_all(const void* data, int len) {
    int sent = 0;
    const uint8_t* p = (const uint8_t*)data;
    while (sent < len) {
        int r = net_guest_send(p + sent, len - sent);
        if (r < 0) return -1;
        if (r == 0) continue;
        sent += r;
    }
    return 0;
}

/* ---- connect + handshake ---------------------------------------------- */
static int parse_host_port(const char* s, uint32_t* out_ip, uint16_t* out_port) {
    uint16_t port = 0;                  /* accumulated from the ":port" digits */
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
                /* Accumulate into a LOCAL variable starting from 0: using
                 * *out_port (pre-seeded with the 5900 default) as the
                 * accumulator turned "15900" into 60828 (5900->59001->191->
                 * 1919->19190->60828 mod 2^16) and the SYN went to a dead
                 * port. */
                port = (uint16_t)(port * 10 + (*p - '0'));
                has_port = 1;
            }
            continue;
        }
        if (*p == '.') {
            if (idx >= 3) return -1;   /* a.b.c.d has exactly 3 dots */
            octets[idx++] = (uint8_t)val;
            val = 0;
        } else if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
        } else {
            return -1;
        }
    }
    if (idx != 3) return -1;           /* need all four octets (3 dots seen) */
    if (val > 255) return -1;
    octets[3] = (uint8_t)val;
    *out_ip = ((uint32_t)octets[0] << 24) | ((uint32_t)octets[1] << 16) |
              ((uint32_t)octets[2] << 8)  | (uint32_t)octets[3];
    *out_port = has_port ? port : 5900;
    return 0;
}

/* Network byte order (big-endian) -> host.  RFB is always big-endian on the
 * wire; the kernel may be little-endian, so convert explicitly. */
static uint32_t vnc_ntohl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

/* ---- Non-blocking RFB handshake ---------------------------------------
 * Driven one step per vnc_poll() call (see vnc_handshake_step) so the GUI
 * never blocks.  Static scratch buffers hold partial reads between steps. */
static uint8_t  hs_version[12];
static uint8_t  hs_sec_list[255];
static uint8_t  hs_si[24];
static uint8_t  hs_nlf[4];
static uint32_t hs_rlen;

static int vnc_handshake_step(void) {
    switch (g_vnc.hs_step) {
    case 0: /* read protocol version (12 bytes) */
        if (vnc_read_exact(hs_version, 12) != 0) { log_str("error", "read version"); return -1; }
        hs_version[11] = 0;
        log_str("server version: ", (const char*)hs_version);
        { const char our[] = "RFB 003.008\n"; vnc_write_all(our, 12); }
        g_vnc.hs_step = 1; return 0;

    case 1: { /* read security type count */
        uint8_t nsec;
        if (vnc_read_exact(&nsec, 1) != 0) { log_str("error", "read nsec"); return -1; }
        if (nsec == 0) {
            uint32_t rlen;
            if (vnc_read_exact(&rlen, 4) == 0) {
                rlen = vnc_ntohl(rlen);
                char reason[256];
                int rl = (rlen < 255) ? (int)rlen : 255;
                if (rl > 0 && vnc_read_exact(reason, rl) == 0) { reason[rl] = 0; log_str("reject", reason); }
            }
            return -1;
        }
        log_hex("nsec=", nsec);
        hs_sec_list[0] = nsec;
        g_vnc.hs_step = 2; return 0;
    }

    case 2: { /* read security type list + pick None */
        uint8_t nsec = hs_sec_list[0];
        if (vnc_read_exact(hs_sec_list + 1, nsec) != 0) { log_str("error", "read sec list"); return -1; }
        uint8_t chosen = 0;
        for (int i = 0; i < nsec; i++)
            if (hs_sec_list[1 + i] == VNC_AUTH_NONE) { chosen = VNC_AUTH_NONE; break; }
        if (!chosen) { log_str("error", "no None auth"); return -1; }
        uint8_t resp = VNC_AUTH_NONE;
        vnc_write_all(&resp, 1);
        g_vnc.hs_step = 3; return 0;
    }

    case 3: /* read SecurityResult (4 bytes) */
        if (vnc_read_exact(&hs_rlen, 4) != 0) { log_str("error", "read sec result"); return -1; }
        hs_rlen = vnc_ntohl(hs_rlen);
        if (hs_rlen != 0) { log_str("error", "auth failed"); return -1; }
        g_vnc.hs_step = 4; return 0;

    case 4: { /* client init + read server init header (24 bytes) */
        uint8_t cinit = VNC_CLIENT_INIT_FLAG;
        vnc_write_all(&cinit, 1);
        if (vnc_read_exact(hs_si, 24) != 0) { log_str("error", "read server init"); return -1; }
        g_vnc.fb_width   = (uint16_t)(((uint16_t)hs_si[0] << 8) | hs_si[1]);
        g_vnc.fb_height  = (uint16_t)(((uint16_t)hs_si[2] << 8) | hs_si[3]);
        g_vnc.bpp        = hs_si[4];
        g_vnc.depth      = hs_si[5];
        g_vnc.big_endian = hs_si[6];
        g_vnc.true_color = hs_si[7];
        g_vnc.red_max    = (uint16_t)(((uint16_t)hs_si[8]  << 8) | hs_si[9]);
        g_vnc.green_max  = (uint16_t)(((uint16_t)hs_si[10] << 8) | hs_si[11]);
        g_vnc.blue_max   = (uint16_t)(((uint16_t)hs_si[12] << 8) | hs_si[13]);
        g_vnc.red_shift   = hs_si[14];
        g_vnc.green_shift = hs_si[15];
        g_vnc.blue_shift  = hs_si[16];
        log_hex("fb", ((uint32_t)g_vnc.fb_width << 16) | g_vnc.fb_height);

        /* (Re)allocate the shared decode buffer to fit the remote framebuffer. */
        int need = (int)g_vnc.fb_width * (int)g_vnc.fb_height;
        if (need > g_vnc.fb_cap) {
            if (g_vnc.fb) bigbuf_free(g_vnc.fb);
            g_vnc.fb = (uint32_t*)bigbuf_alloc((uint32_t)(need * (int)sizeof(uint32_t)));
            g_vnc.fb_cap = g_vnc.fb ? need : 0;
        }
        g_vnc.hs_step = 5; return 0;
    }

    case 5: { /* read server name (4-byte len + bytes) */
        if (vnc_read_exact(hs_nlf, 4) != 0) { log_str("error", "read name len"); return -1; }
        uint32_t nlen = ((uint32_t)hs_nlf[0] << 24) | ((uint32_t)hs_nlf[1] << 16) |
                        ((uint32_t)hs_nlf[2] << 8)  | (uint32_t)hs_nlf[3];
        if (nlen > 127) nlen = 127;
        if (nlen > 0) {
            if (vnc_read_exact(g_vnc.server_name, (int)nlen) != 0) { log_str("error","read name"); return -1; }
        }
        g_vnc.server_name[nlen] = 0;
        log_str("server: ", (const char*)g_vnc.server_name);
        g_vnc.hs_step = 6; return 0;
    }

    case 6: /* send SetPixelFormat + SetEncodings (Raw only) */
        return (vnc_send_prefs() == 0) ? 1 : -1;

    default: return -1;
    }
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

    /* Set Encodings: Raw + CopyRect + RRE + Hextile.
     * The kernel has no zlib/inflate, so we exclude Zlib/Tight.  The
     * decoders for RRE, Hextile and CopyRect are already compiled in (see
     * vnc_decode_rre / vnc_decode_hextile / CopyRect case in the update
     * dispatcher).  Raw is kept as baseline.  Four encodings total.
     *
     * (Previous version advertised Raw-only; x11vnc uses RRE/Hextile
     * aggressively for Chromium motion regions, so enabling them gives 5-10x
     * higher effective frame rate for moving content.) */
    uint8_t enc[4 + 4 * 4];
    enc[0] = VNC_SET_ENCODINGS;
    enc[1] = enc[2] = 0;
    enc[3] = 4;                         /* count = 4 */
    enc[4] = enc[5] = enc[6] = 0; enc[7] = VNC_ENC_RAW;      /* Raw (0) */
    enc[8] = enc[9] = enc[10] = 0; enc[11] = VNC_ENC_COPYRECT;/* CopyRect (1) */
    enc[12]= enc[13]=enc[14] = 0; enc[15] = VNC_ENC_RRE;     /* RRE (2) */
    enc[16]= enc[17]=enc[18] = 0; enc[19] = VNC_ENC_HEXTILE; /* Hextile (5) */
    vnc_write_all(enc, sizeof(enc));

    return 0;
}

/* FramebufferUpdateRequest (RFB type 3).  incremental=0 asks for the full
 * frame (used once at connect); incremental=1 asks only for pixels that
 * change after this point.  Region covers the whole server framebuffer. */
static int vnc_send_fb_request_int(uint8_t incremental) {
    uint8_t req[10];
    req[0] = VNC_FRAMEBUFFER_REQUEST;
    req[1] = incremental;                 /* 0 = full, 1 = incremental */
    req[2] = req[3] = 0;                  /* x (big-endian) */
    req[4] = req[5] = 0;                  /* y (big-endian) */
    req[6] = (g_vnc.fb_width >> 8) & 0xFF;
    req[7] = g_vnc.fb_width & 0xFF;
    req[8] = (g_vnc.fb_height >> 8) & 0xFF;
    req[9] = g_vnc.fb_height & 0xFF;
    return vnc_write_all(req, 10);
}

/* Full (non-incremental) update request — used once right after connect. */
static int vnc_send_fb_request(void) {
    return vnc_send_fb_request_int(0);
}

/* Incremental update request — re-arms the server after each poll. */
static int vnc_send_fb_request_incremental(void) {
    return vnc_send_fb_request_int(1);
}

/* Send a PointerEvent (RFB type 5).  Coordinates are absolute, in the
 * remote framebuffer's coordinate space, and MUST be big-endian on the
 * wire.  button_mask is the RFB button bitmask (bit0=left, bit1=middle,
 * bit2=right).  Signature: vnc_send_pointer(x, y, button_mask). */
static int vnc_send_pointer(int x, int y, uint8_t button_mask) {
    if (x < 0) x = 0; else if (x > 0xFFFF) x = 0xFFFF;
    if (y < 0) y = 0; else if (y > 0xFFFF) y = 0xFFFF;
    uint8_t p[6];
    p[0] = VNC_POINTER_EVENT;
    p[1] = button_mask;
    p[2] = (uint8_t)((x >> 8) & 0xFF); p[3] = (uint8_t)(x & 0xFF);
    p[4] = (uint8_t)((y >> 8) & 0xFF); p[5] = (uint8_t)(y & 0xFF);
    return vnc_write_all(p, 6);
}

/* Send a KeyEvent (RFB type 4).  keysym is the 32-bit X11/RFB keysym,
 * big-endian on the wire.  is_down=true for press, false for release.
 * Signature: vnc_send_key(keysym, is_down). */
static int vnc_send_key(uint32_t keysym, int is_down) {
    uint8_t k[8];
    k[0] = VNC_KEY_EVENT;
    k[1] = is_down ? 1 : 0;
    k[2] = k[3] = 0;
    k[4] = (uint8_t)((keysym >> 24) & 0xFF);
    k[5] = (uint8_t)((keysym >> 16) & 0xFF);
    k[6] = (uint8_t)((keysym >> 8) & 0xFF);
    k[7] = (uint8_t)(keysym & 0xFF);
    return vnc_write_all(k, 8);
}

/* Reusable raw-rectangle decode buffer.  Raw rectangles can be up to the
 * whole framebuffer (3 MiB at 1024x768), which is framebuffer-class: take it
 * from bigbuf (PMM-backed) ONCE and grow it only when a bigger rectangle
 * arrives.  Allocating/freeing per frame would churn the ~10 MiB kernel heap
 * and fragment it (and OOM the GUI). */
static uint8_t* g_raw_buf = 0;
static uint32_t g_raw_cap = 0;

static uint8_t* raw_buf_get(uint32_t bytes) {
    if (bytes <= g_raw_cap) return g_raw_buf;
    if (g_raw_buf) { bigbuf_free(g_raw_buf); g_raw_buf = 0; g_raw_cap = 0; }
    g_raw_buf = (uint8_t*)bigbuf_alloc(bytes);
    if (g_raw_buf) g_raw_cap = bytes;
    return g_raw_buf;
}

/* ---- Blit a pixel buffer into NexOS GUI --------------------------------
 * buf layout: Raw RFB pixel data as configured by SetPixelFormat:
 *   bpp=32, depth=24, big_endian=0, red_shift=16, green_shift=8, blue_shift=0
 * => each pixel is 4 bytes little-endian, read as uint32 = 0x00RRGGBB,
 *    which matches NexOS Color (ARGB, A=0 opaque).                      */
struct VncBlitArgs { int x, y, w, h; const uint32_t* pixels; };

static void vnc_blit_raw(uint16_t x_start, uint16_t y_start,
                         uint16_t w, uint16_t h, const uint8_t* buf) {
    int x = (int)x_start, y = (int)y_start, cw = (int)w, ch = (int)h;
    /* Decode into the shared VncState::fb buffer (NOT the screen).  The GUI
     * compositor reads g_vnc.fb and blits it into the Linux window's client
     * area with letterbox scaling.  Every write is bounded to the buffer. */
    if (!g_vnc.fb || g_vnc.fb_cap <= 0) return;

    int bw = (int)g_vnc.fb_width, bh = (int)g_vnc.fb_height;
    if (bw <= 0 || bh <= 0) return;
    if (x < 0)      { cw += x; x = 0; }
    if (y < 0)      { ch += y; y = 0; }
    if (cw <= 0 || ch <= 0) return;
    if (x + cw > bw) cw = bw - x;
    if (y + ch > bh) ch = bh - y;
    if (cw <= 0 || ch <= 0) return;

    const uint32_t* src_px = (const uint32_t*)buf;
    int skip_cols = x - (int)x_start;
    int skip_rows = y - (int)y_start;
    const uint32_t* src = src_px + (uint64_t)skip_rows * (uint64_t)w + skip_cols;

    for (int row = 0; row < ch; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= bh) continue;
        const uint32_t* srow = src + (uint64_t)row * (uint64_t)w;
        uint32_t* dbase = g_vnc.fb + (uint64_t)dy * (uint64_t)bw;
        for (int col = 0; col < cw; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= bw) continue;
            dbase[dx] = srow[col + skip_cols];
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
    if (!g_vnc.fb || g_vnc.fb_cap <= 0) return;
    int bw = (int)g_vnc.fb_width, bh = (int)g_vnc.fb_height;
    if (bw <= 0 || bh <= 0) return;
    int rx = (int)x, ry = (int)y, rw = (int)w, rh = (int)h;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rw <= 0 || rh <= 0) return;
    if (rx + rw > bw) rw = bw - rx;
    if (ry + rh > bh) rh = bh - ry;
    for (int row = 0; row < rh; row++) {
        int dy = ry + row;
        if (dy < 0 || dy >= bh) continue;
        uint32_t* dbase = g_vnc.fb + (uint64_t)dy * (uint64_t)bw;
        for (int col = 0; col < rw; col++) {
            int dx = rx + col;
            if (dx < 0 || dx >= bw) continue;
            dbase[dx] = color;
        }
    }
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
                uint8_t* buf = raw_buf_get((uint32_t)n);
                if (!buf) return -1;
                int rem = n;
                while (rem > 0) {
                    int c = rem > 4096 ? 4096 : rem;
                    if (vnc_read_exact(buf + (n - rem), c) != 0) { return -1; }
                    rem -= c;
                }
                vnc_blit_raw((uint16_t)(x + tx), (uint16_t)(y + ty), tw, th, buf);
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

/* ---- Receive and dispatch ONE server message ---------------------------
 * Reads a single server-to-client message, dispatches it, and returns.
 * Returns:
 *   1  : a framebuffer update was applied (screen changed)
 *   0  : a non-update message was consumed (Bell / CutText)
 *  -1  : connection/protocol error
 * Only FramebufferUpdate (type 0) paints; Bell and ServerCutText are silently
 * drained so the stream keeps flowing.  Returns -1 with no read when no data
 * is available (vnc_read_exact tolerates transient r==0, but a true -1 from
 * the socket means the peer went away). */
static int vnc_recv_one(void) {
    uint8_t hdr[4];
    if (vnc_read_exact(hdr, 4) != 0) return -1;

    uint8_t type   = hdr[0];
    if (type == VNC_FRAMEBUFFER_UPDATE) {
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
                uint8_t* buf = raw_buf_get((uint32_t)bytes);
                if (!buf) { log_str("error", "raw buf alloc failed"); return -1; }
                int remaining = bytes;
                while (remaining > 0) {
                    int to_read = remaining > 4096 ? 4096 : remaining;
                    if (vnc_read_exact(buf + (bytes - remaining), to_read) != 0) {
                        return -1;
                    }
                    remaining -= to_read;
                }
                vnc_blit_raw(x, y, w, h, buf);   /* boundary-checked */
            } else if (enc == VNC_ENC_RRE) {
                if (vnc_decode_rre(x, y, w, h) != 0) return -1;
            } else if (enc == VNC_ENC_HEXTILE) {
                if (vnc_decode_hextile(x, y, w, h) != 0) return -1;
            } else if (enc == VNC_ENC_COPYRECT) {
                /* CopyRect: copy a rectangle from the decoded framebuffer
                 * at (src_x, src_y) to (x, y) size (w, h).  Uses direct fb
                 * indexing — no additional network I/O needed (the source
                 * pixels are already in g_vnc.fb from a previous update). */
                uint8_t src[4];
                if (vnc_read_exact(src, 4) != 0) return -1;
                uint16_t sx = (uint16_t)((src[0] << 8) | src[1]);
                uint16_t sy = (uint16_t)((src[2] << 8) | src[3]);
                int bw = (int)g_vnc.fb_width, bh = (int)g_vnc.fb_height;
                if (bw > 0 && bh > 0) {
                    int rx = (int)x, ry = (int)y, rw = (int)w, rh = (int)h;
                    if (rw > bw - rx) rw = bw - rx; if (rw < 0) rw = 0;
                    if (rh > bh - ry) rh = bh - ry; if (rh < 0) rh = 0;
                    if (rw > 0 && rh > 0) {
                        /* CopyRect can overlap source/dest — copy bottom-up
                         * when src is above dest to avoid smearing */
                        int drow = (sy < ry) ? -1 : 1;
                        int srow = (sy < ry) ? rh - 1 : 0;
                        for (int row = 0; row < rh; row++) {
                            int ty = ry + ((sy < ry) ? (rh - 1 - row) : row);
                            int ts = sy + ((sy < ry) ? (rh - 1 - row) : row);
                            uint32_t* dst_row = g_vnc.fb + (uint64_t)ty * bw + rx;
                            uint32_t* src_row = g_vnc.fb + (uint64_t)ts * bw + sx;
                            for (int col = 0; col < rw; col++)
                                dst_row[col] = src_row[col];
                        }
                    }
                }
            } else {
                log_hex("skip enc", (uint32_t)enc);
                return -1;
            }
        }
        return 1;
    } else if (type == VNC_BELL) {
        return 0;                                /* no payload */
    } else if (type == VNC_SERVER_CUT_TEXT) {
        uint8_t pad[3];
        uint8_t lenbuf[4];
        if (vnc_read_exact(pad, 3) != 0) return -1;     /* 3 reserved bytes */
        if (vnc_read_exact(lenbuf, 4) != 0) return -1;  /* big-endian length */
        uint32_t len = ((uint32_t)lenbuf[0] << 24) | ((uint32_t)lenbuf[1] << 16) |
                       ((uint32_t)lenbuf[2] << 8) | (uint32_t)lenbuf[3];
        /* Drain the clipboard text (bounded) so it cannot desync the stream. */
        while (len > 0) {
            uint8_t chunk[256];
            uint32_t c = len > 256 ? 256 : len;
            if (vnc_read_exact(chunk, (int)c) != 0) return -1;
            len -= c;
        }
        return 0;
    }
    log_hex("unknown msg type", (uint32_t)type);
    return -1;                                  /* unknown: abort to be safe */
}

/* ---- vnc_poll(): NON-BLOCKING state machine driver ---------------------
 * extern "C" entry point called once per GUI tick.  It advances exactly one
 * step of the connection (TCP connect / RFB handshake / one server message)
 * and returns immediately — never spinning, never freezing the desktop.
 * Returns:
 *    1 : a framebuffer update was applied this tick
 *    0 : in-progress or a non-update message consumed (Bell / CutText)
 *   -1 : not connected or a fatal stream error
 *
 * The TCP connect itself is non-blocking: vnc_connect() only queues a SYN via
 * net_guest_connect_async() and marks cstate=VNC_ST_CONNECTING.  We then poll
 * completion with net_guest_connect_state() (which pumps net_poll() once per
 * call).  Once ESTABLISHED we run the stepwise RFB handshake; once that
 * completes we enter RUNNING and start consuming FramebufferUpdates. */
extern "C" int vnc_poll(void) {
    /* Demo auto-connect: NEXOS_VNC_DEMO builds call vnc.connect on the FIRST
     * poll tick.  plugins_boot runs before the NIC is up, so an SYN sent from
     * there is malformed and the SLIRP gateway answers RST; by the first GUI
     * poll the NE2000 is initialized and 10.0.2.2 (SLIRP gw) is reachable. */
#ifdef NEXOS_VNC_DEMO
    if (g_vnc.cstate == VNC_ST_IDLE || g_vnc.cstate == VNC_ST_ERROR) {
        static int s_demo_done = 0;
        if (!s_demo_done) {
            s_demo_done = 1;
            uint32_t ip = (10u << 24) | (0u << 16) | (2u << 8) | 2u;
            vnc_connect(ip, 15900);
        }
    }
#endif
    switch (g_vnc.cstate) {
    case VNC_ST_IDLE:
    case VNC_ST_ERROR:
        return -1;

    case VNC_ST_CONNECTING: {
        int st = net_guest_connect_state();   /* also pumps net_poll() once */
        if (st < 0) {                          /* -1: RST / failed */
            log_str("error", "TCP connect failed");
            g_vnc.cstate = VNC_ST_ERROR;
            return -1;
        }
        if (st == 2) {                         /* ESTABLISHED */
            g_vnc.hs_step = 0;
            g_vnc.cstate = VNC_ST_HANDSHAKE;
            log_str("TCP", "connected");
        }
        return 0;                              /* still connecting */
    }

    case VNC_ST_HANDSHAKE: {
        int h = vnc_handshake_step();
        if (h < 0) {
            log_str("error", "handshake failed");
            g_vnc.cstate = VNC_ST_ERROR;
            net_guest_close();
            return -1;
        }
        if (h == 1) {                          /* handshake complete */
            g_vnc.connected = 1;
            g_vnc.cstate = VNC_ST_RUNNING;
            /* Required diagnostic: "[VNC] Handshake OK 1024x768" */
            {
                char msg[64]; int n = 0;
                const char* p = "Handshake OK ";
                while (*p) msg[n++] = *p++;
                n += my_itoa(msg + n, g_vnc.fb_width);
                msg[n++] = 'x';
                n += my_itoa(msg + n, g_vnc.fb_height);
                msg[n] = 0;
                log_str("", msg);
            }
            vnc_send_fb_request();             /* ask for the first full frame */
        }
        return 0;                              /* progress, not yet streaming */
    }

    case VNC_ST_RUNNING: {
        if (!g_vnc.connected) return -1;
        int r = vnc_recv_one();                /* EXACTLY one server message */
        if (r < 0) {
            g_vnc.connected = 0;
            g_vnc.cstate = VNC_ST_ERROR;
            net_guest_close();
            return -1;
        }
        /* Re-arm: ask for the next incremental update. */
        vnc_send_fb_request_incremental();
        if (r == 1) {
            /* Required diagnostic: "[VNC] Frame updated" (throttled: raw 3 MiB
             * full-frame floods the serial log, so print once per 64 frames). */
            static int s_frame_log_ctr = 0;
            if ((s_frame_log_ctr++ & 63) == 0)
                log_str("", "Frame updated");
        }
        return (r == 1) ? 1 : 0;
    }
    }
    return -1;
}

/* ---- public services ------------------------------------------------- */
/* vnc_connect(ip, port): connect to a VNC server by numeric IP + port and
 * complete the full RFB 3.8 handshake + initial prefs.  ip/port are host
 * byte order (matches net_guest_connect's convention).  Returns 0 on
 * success, -1 on failure (TCP or RFB).  Safe to call from kernel syscall
 * code that already has a resolved IP. */
extern "C" int vnc_connect(uint32_t ip, uint16_t port) {
    if (g_vnc.connected || g_vnc.cstate == VNC_ST_CONNECTING ||
        g_vnc.cstate == VNC_ST_HANDSHAKE) {
        vnc_disconnect();   /* tear down any prior session first */
    }
    g_vnc.server_ip = ip;
    g_vnc.server_port = port;

    char dbg[32];
    int n = 0;
    const uint8_t* o = (const uint8_t*)&ip;
    n += my_itoa(dbg + n, o[0]); dbg[n++] = '.';
    n += my_itoa(dbg + n, o[1]); dbg[n++] = '.';
    n += my_itoa(dbg + n, o[2]); dbg[n++] = '.';
    n += my_itoa(dbg + n, o[3]);
    dbg[n] = 0;
    log_str("connecting to ", dbg);
    /* Diagnostic: the exact port handed to the TCP layer. */
    {
        char pbuf[32]; int n2 = 0;
        n2 += my_itoa(pbuf + n2, (uint32_t)port);
        pbuf[n2] = 0;
        log_str("port=", pbuf);
    }

    /* Non-blocking: queue SYN, return immediately.  vnc_poll() drives the
     * remainder of the connect + handshake from the GUI tick. */
    if (net_guest_connect_async(ip, port) != 0) {
        log_str("error", "TCP connect failed");
        g_vnc.cstate = VNC_ST_ERROR;
        return -1;
    }
    g_vnc.cstate = VNC_ST_CONNECTING;
    g_vnc.connected = 0;
    return 0;   /* "connect in progress" — not a failure */
}

extern "C" void vnc_disconnect(void) {
    if (g_vnc.connected || g_vnc.cstate == VNC_ST_CONNECTING ||
        g_vnc.cstate == VNC_ST_HANDSHAKE) {
        net_guest_close();
    }
    if (g_vnc.cstate == VNC_ST_CONNECTING) {
        net_guest_connect_abort();
    }
    g_vnc.connected = 0;
    g_vnc.cstate = VNC_ST_IDLE;
    g_vnc.hs_step = 0;
}

/* Expose the shared decode buffer + dimensions to the GUI compositor. */
extern "C" int vnc_session_fb(void** fb_out, int* w_out, int* h_out) {
    if (fb_out) *fb_out = g_vnc.fb;
    if (w_out)  *w_out  = g_vnc.fb_width;
    if (h_out)  *h_out  = g_vnc.fb_height;
    return (g_vnc.connected && g_vnc.fb) ? 1 : 0;
}

static int svc_connect(void* args, void* out, int outcap) {
    const char* host_port = args ? (const char*)args : "10.0.2.2:5900";
    /* Diagnostic: dump the raw bytes so a bad literal/pointer is visible. */
    {
        char dump[80]; int n = 0;
        const char* p = "parse input: ";
        while (*p) dump[n++] = *p++;
        for (int i = 0; i < 16 && host_port[i]; i++) {
            dump[n++] = host_port[i];
        }
        dump[n++] = ' '; dump[n++] = '(';
        for (int i = 0; i < 16 && host_port[i]; i++) {
            uint8_t c = (uint8_t)host_port[i];
            const char* h = "0123456789ABCDEF";
            dump[n++] = h[(c >> 4) & 0xF]; dump[n++] = h[c & 0xF]; dump[n++] = ' ';
        }
        dump[n++] = ')'; dump[n] = 0;
        log_str("", dump);
    }
    uint32_t ip;
    uint16_t port;
    if (parse_host_port(host_port, &ip, &port) != 0) {
        log_str("error", "bad host:port");
        return -1;
    }
    return vnc_connect(ip, port);
}

static int svc_disconnect(void* args, void* out, int outcap) {
    (void)args; (void)out; (void)outcap;
    vnc_disconnect();
    log_str("OK", "disconnected");
    return 0;
}

static int svc_poll(void* args, void* out, int outcap) {
    (void)args;
    int r = vnc_poll();
    if (out && outcap >= 4) *(int*)out = (r == 1) ? 1 : 0;
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
    /* a[0]=x, a[1]=y, a[2]=button_mask, a[3]=keysym (0 = none).
     * a[3] < 0 signals a key release (keysym in -a[3]); >=0 a key press. */
    vnc_send_pointer((int)a[0], (int)a[1], (uint8_t)a[2]);
    if (a[3] != 0)
        vnc_send_key((uint32_t)(a[3] > 0 ? a[3] : -a[3]), a[3] > 0);
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
    /* Publish the call-table entries as services too.  linux_manager.cpp and
     * gui.cpp reach this plugin through svc_lookup("vnc.connect"/"vnc.poll"/
     * "vnc.input") -- without these registrations every lookup fails and the
     * Linux-Guest pipeline silently never runs. */
    svc_register("vnc.connect",    (svc_fn)svc_connect);
    svc_register("vnc.disconnect", (svc_fn)svc_disconnect);
    svc_register("vnc.poll",       (svc_fn)svc_poll);
    svc_register("vnc.request_frame", (svc_fn)svc_request_frame);
    svc_register("vnc.input",      (svc_fn)svc_input);
    svc_register("vnc.status",     (svc_fn)svc_status);
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

    /* Query the NexOS screen size so vnc_blit_raw() can boundary-check every
     * rectangle against the local VRAM (prevents overrun when a server
     * framebuffer is larger than the guest display).  The "gui.screen_size"
     * service returns int[2] = {width, height}. */
    svc_fn screen_svc = svc_lookup("gui.screen_size");
    if (screen_svc) {
        int dims[2] = {0, 0};
        if (screen_svc(dims, dims, sizeof(dims)) == 0) {
            g_screen_w = dims[0];
            g_screen_h = dims[1];
            log_hex("screen size", (uint32_t)(g_screen_w << 16) | (uint32_t)g_screen_h);
        }
    }
    if (g_screen_w <= 0 || g_screen_h <= 0) {
        /* Fall back to the remote framebuffer size (still a finite bound). */
        g_screen_w = (int)g_vnc.fb_width;
        g_screen_h = (int)g_vnc.fb_height;
        log_str("WARN", "no gui.screen_size — bound by server fb");
    }
    log_str("OK", "init");
    return 0;
}

static void vnc_exit(Plugin* self) {
    (void)self;
    if (g_vnc.connected) net_guest_close();
    g_vnc.connected = 0;
    if (g_vnc.fb) { bigbuf_free(g_vnc.fb); g_vnc.fb = 0; g_vnc.fb_cap = 0; }
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
