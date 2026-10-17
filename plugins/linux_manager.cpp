/* linux_manager.c - Linux Guest lifecycle (serial control channel + retry FSM)
 * See linux_manager.h for the architecture and the line protocol.
 *
 * Freestanding: kmalloc/kfree only, no libc, no blocking waits.
 * Self-contained: it talks to the host over a dedicated control UART (COM2 at
 * 0x2F8) and to the vnc_client plugin over the NexOS service registry, so it
 * needs no missing kernel.h / fragile extern glue.
 */
#include "linux_manager.h"
#include "plugin_manager.h"   /* svc_fn, svc_lookup, svc_register */

#include <stdint.h>

/* ---- vnc_client framebuffer accessor (defined in vnc_client.cpp) ------- */
extern "C" int vnc_session_fb(void** fb_out, int* w_out, int* h_out);

/* ---- kernel heap (extern from kernel.cpp) ----------------------------- */
extern "C" void* kmalloc(uint32_t size);
extern "C" void  kfree(void* ptr);
/* Framebuffer-class mirror buffer uses bigbuf (PMM-backed, outside the GUI heap). */
extern "C" void* bigbuf_alloc(uint32_t size);
extern "C" void  bigbuf_free(void* ptr);

/* ---- dedicated control UART COM2 = 0x2F8 -------------------------------
 * Replace lm_serial_putc/getc with virtio-serial read/write to migrate
 * transport; the line protocol above stays byte-identical. */
#ifndef LM_COM2_PORT
#define LM_COM2_PORT 0x2F8
#endif
#define LM_COM2_LSR   (LM_COM2_PORT + 5)
#define LM_LSR_RXRDY  0x01
#define LM_LSR_TXEMPTY 0x20

static inline uint8_t lm_inb(uint16_t p) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(p) : "memory");
    return v;
}
static inline void lm_outb(uint16_t p, uint8_t v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(p) : "memory");
}

static inline void lm_serial_putc(char c) {
    while (!(lm_inb(LM_COM2_LSR) & LM_LSR_TXEMPTY)) { /* spin only on the FIFO, not the NIC */ }
    lm_outb(LM_COM2_PORT, (uint8_t)c);
}
static inline int lm_serial_getc(void) {
    if (lm_inb(LM_COM2_LSR) & LM_LSR_RXRDY) return (int)lm_inb(LM_COM2_PORT);
    return -1;
}
static void lm_serial_puts(const char* s) {
    while (*s) lm_serial_putc(*s++);
}

/* ---- host reachability of the Guest's VNC ----
 * Nested topology: the Linux Guest is QEMU#2, x11vnc listens on :5900 inside
 * it, and the host orchestrator forwards it with hostfwd=tcp::15900-:5900.
 * From NexOS (QEMU#1) the SLIRP gateway 10.0.2.2 reaches the host, so we
 * connect to 10.0.2.2:15900. */
#define LM_VNC_HOSTPORT "10.0.2.2:15900"

/* ---- vnc_client service handles (resolved lazily on first use) -------- */
static svc_fn g_vnc_connect   = 0;
static svc_fn g_vnc_disconnect= 0;
static svc_fn g_vnc_poll      = 0;
static int   g_vnc_svc_resolved = 0;

static void lm_resolve_vnc(void) {
    if (g_vnc_svc_resolved) return;
    g_vnc_connect    = svc_lookup("vnc.connect");
    g_vnc_disconnect = svc_lookup("vnc.disconnect");
    g_vnc_poll       = svc_lookup("vnc.poll");
    g_vnc_svc_resolved = 1;
}

/* ---- module state ---- */
static LinuxManagerState g_lm_state = LM_IDLE;
static LinuxGuestFB      g_lm_fb    = {0};
static int               g_lm_window_open = 0;   /* a Linux window exists */
static uint32_t          g_lm_retry_ms = 0;      /* ms accumulator for 500ms */
static uint32_t          g_lm_last_tick = 0;     /* last now_ms */
static int               g_lm_vnc_connected = 0;
/* tiny host-reply ring so the serial poll can reassemble a line */
static char  g_lm_line[64];
static int   g_lm_line_n = 0;

/* ---- framebuffer helpers ---- */
LinuxGuestFB* linux_manager_fb(void) { return &g_lm_fb; }

static void lm_ensure_fb(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_lm_fb.fb_buf && g_lm_fb.fb_w == w && g_lm_fb.fb_h == h) return;
    int cap = w * h;
    if (!g_lm_fb.fb_buf || cap > g_lm_fb.cap) {
        if (g_lm_fb.fb_buf) bigbuf_free(g_lm_fb.fb_buf);
        g_lm_fb.fb_buf = (uint32_t*)bigbuf_alloc((uint32_t)cap * sizeof(uint32_t));
        g_lm_fb.cap = (g_lm_fb.fb_buf) ? cap : 0;
    }
    if (g_lm_fb.fb_buf) { g_lm_fb.fb_w = w; g_lm_fb.fb_h = h; }
}

static void lm_ingest_fb(void) {
    /* The vnc plugin owns the decoded pixels; mirror them into our shared
     * LinuxGuestFB so gui.cpp has one stable place to read from. */
    void* fb = 0; int w = 0, h = 0;
    if (vnc_session_fb(&fb, &w, &h) <= 0 || !fb || w <= 0 || h <= 0) return;
    lm_ensure_fb(w, h);
    if (g_lm_fb.fb_buf && g_lm_fb.fb_w == w && g_lm_fb.fb_h == h) {
        const uint32_t* src = (const uint32_t*)fb;
        for (int i = 0; i < w * h; i++) g_lm_fb.fb_buf[i] = src[i];
        g_lm_fb.have_frame = 1;
    }
}

void linux_manager_attach_vnc(void) { g_lm_vnc_connected = 1; }
void linux_manager_detach_vnc(void) { g_lm_vnc_connected = 0; }

/* ---- line protocol send ---- */
static void lm_send_cmd(const char* cmd) {
    lm_serial_puts(cmd);
    lm_serial_putc('\n');
    lm_serial_puts("[LM] -> host: ");
    lm_serial_puts(cmd);
    lm_serial_puts("\n");
}

static int my_strlen(const char* s){ int n=0; while(s[n]) n++; return n; }

static int lm_line_eq(const char* s) {
    int n = my_strlen(s);
    for (int i = 0; i < n; i++)
        if (g_lm_line[i] != s[i]) return 0;
    return (g_lm_line[n] == 0 || g_lm_line[n] == '\r');
}

/* ---- host reply parser (called when a full line is buffered) ---- */
static void lm_on_host_line(void) {
    if (lm_line_eq("ready") && g_lm_state == LM_STARTING) {
        g_lm_state = LM_VNC_CONNECTING;
        g_lm_retry_ms = 0;
    } else if (lm_line_eq("down") && g_lm_state == LM_STOPPING) {
        g_lm_state = LM_IDLE;
    } else if (lm_line_eq("busy") && g_lm_state == LM_STARTING) {
        /* host still booting; stay in STARTING, will retry "status" */
    } else if (lm_line_eq("error")) {
        if (g_lm_state == LM_STARTING || g_lm_state == LM_VNC_CONNECTING)
            g_lm_state = LM_STOPPING;  /* give up; ask host to stop */
    }
}

/* ---- lifecycle API ---- */
void linux_manager_init(void) {
    g_lm_state = LM_IDLE;
    g_lm_window_open = 0;
    g_lm_vnc_connected = 0;
    g_lm_fb.fb_buf = 0; g_lm_fb.cap = 0; g_lm_fb.fb_w = 0; g_lm_fb.fb_h = 0;
    g_lm_fb.have_frame = 0;
    g_lm_fb.dirty_x = g_lm_fb.dirty_y = g_lm_fb.dirty_w = g_lm_fb.dirty_h = 0;
    g_lm_line_n = 0;
    lm_resolve_vnc();
}

void linux_manager_start(void) {
    if (g_lm_state != LM_IDLE) return;
    g_lm_state = LM_STARTING;
    g_lm_window_open = 1;
    g_lm_retry_ms = 0;
    lm_send_cmd("start");
}

void linux_manager_stop(void) {
    if (g_lm_state == LM_IDLE) return;
    g_lm_window_open = 0;
    g_lm_state = LM_STOPPING;
    lm_resolve_vnc();
    if (g_lm_vnc_connected && g_vnc_disconnect) g_vnc_disconnect(0, 0, 0);
    g_lm_vnc_connected = 0;
    lm_send_cmd("stop");
}

int linux_manager_active(void) { return g_lm_window_open; }

/* Transport-agnostic line assembler: identical logic to the serial drain
 * below, but callable from any backend (vUART, test harness, ...). */
int linux_manager_feed_byte(int c) {
    if (c < 0) return 0;
    if (c == '\n' || c == '\r') {
        if (g_lm_line_n > 0) {
            g_lm_line[g_lm_line_n] = 0;
            lm_on_host_line();
            g_lm_line_n = 0;
            return 1;
        }
        return 0;
    }
    if (g_lm_line_n < 63) g_lm_line[g_lm_line_n++] = (char)c;
    return 0;
}

/* Queue a reply line for the transport to carry back to the guest/host. */
void linux_manager_push_reply(const char* line) {
    if (!line) return;
    lm_serial_puts(line);
    lm_serial_putc('\n');
}

void linux_manager_tick(uint32_t now_ms) {
    lm_resolve_vnc();

    /* drain any host serial replies (non-blocking) */
    int c;
    while ((c = lm_serial_getc()) >= 0) {
        linux_manager_feed_byte(c);
    }

    uint32_t dt = (now_ms && g_lm_last_tick) ? (now_ms - g_lm_last_tick) : 16;
    g_lm_last_tick = now_ms;

    switch (g_lm_state) {
        case LM_VNC_CONNECTING:
            g_lm_retry_ms += dt;
            if (g_lm_retry_ms >= 500) {        /* retry every 500 ms */
                g_lm_retry_ms = 0;
                if (g_vnc_connect) g_vnc_connect((void*)LM_VNC_HOSTPORT, 0, 0);
                /* connection is async; vnc.poll drives it to STREAMING */
                linux_manager_attach_vnc();
                g_lm_state = LM_STREAMING;
            }
            break;
        case LM_STREAMING:
            if (g_vnc_poll) {
                int r = g_vnc_poll(0, 0, 0);
                if (r < 0) {                   /* vnc dropped; re-enter retry */
                    linux_manager_detach_vnc();
                    g_lm_state = LM_VNC_CONNECTING;
                    g_lm_retry_ms = 500;       /* retry immediately next tick */
                } else {
                    lm_ingest_fb();
                }
            }
            break;
        case LM_STOPPING:
            /* wait for host "down"; if no reply, settle after a timeout */
            g_lm_retry_ms += dt;
            if (g_lm_retry_ms > 2000) g_lm_state = LM_IDLE;
            break;
        case LM_STARTING:
        case LM_IDLE:
        default:
            break;
    }
}

const char* linux_manager_status_text(void) {
    switch (g_lm_state) {
        case LM_IDLE:            return "Linux";
        case LM_STARTING:        return "Starting Linux...";
        case LM_VNC_CONNECTING:  return "Connecting to Linux...";
        case LM_STREAMING:       return "Linux";
        case LM_STOPPING:        return "Stopping Linux...";
    }
    return "Linux";
}

/* ---- Service-thunk wrappers (svc_fn signature) -------------------------- */
static int linux_manager_tick_svc(void* args, void* out, int outcap) {
    (void)out; (void)outcap;
    uint32_t now = args ? *(uint32_t*)args : 0;
    linux_manager_tick(now);
    return 0;
}
static int linux_manager_start_svc(void* args, void* out, int outcap) {
    (void)args; (void)out; (void)outcap;
    linux_manager_start();
    return 0;
}
static int linux_manager_stop_svc(void* args, void* out, int outcap) {
    (void)args; (void)out; (void)outcap;
    linux_manager_stop();
    return 0;
}
static int linux_manager_active_svc(void* args, void* out, int outcap) {
    (void)args;
    if (out && outcap >= 4) *(int*)out = linux_manager_active();
    return 0;
}
static int linux_manager_status_svc(void* args, void* out, int outcap) {
    (void)args;
    if (out && outcap >= 4) *(const char**)out = linux_manager_status_text();
    return 0;
}
static int linux_manager_fb_svc(void* args, void* out, int outcap) {
    (void)args;
    if (out && outcap >= 4) *(LinuxGuestFB**)out = linux_manager_fb();
    return 0;
}

/* ---- Plugin descriptor -------------------------------------------------- */
static int lm_init(Plugin* self) {
    (void)self;
    linux_manager_init();
    svc_register("linux_manager.tick",   (svc_fn)linux_manager_tick_svc);
    svc_register("linux_manager.start",  (svc_fn)linux_manager_start_svc);
    svc_register("linux_manager.stop",   (svc_fn)linux_manager_stop_svc);
    svc_register("linux_manager.active", (svc_fn)linux_manager_active_svc);
    svc_register("linux_manager.status", (svc_fn)linux_manager_status_svc);
    svc_register("linux_manager.fb",     (svc_fn)linux_manager_fb_svc);
    return 0;
}

static int lm_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self;
    if (pm_strcmp(method, "init") == 0)     return lm_init(self);
    if (pm_strcmp(method, "start") == 0)    { linux_manager_start(); return 0; }
    if (pm_strcmp(method, "stop") == 0)     { linux_manager_stop(); return 0; }
    if (pm_strcmp(method, "tick") == 0)     { uint32_t now = args ? *(uint32_t*)args : 0; linux_manager_tick(now); return 0; }
    if (pm_strcmp(method, "active") == 0)   { if (out && outcap >= 4) *(int*)out = linux_manager_active(); return 0; }
    if (pm_strcmp(method, "status") == 0)   { if (out && outcap >= 4) *(const char**)out = linux_manager_status_text(); return 0; }
    if (pm_strcmp(method, "fb") == 0)       { if (out && outcap >= 4) *(LinuxGuestFB**)out = linux_manager_fb(); return 0; }
    return -1;
}

extern "C" const Plugin g_linux_manager = {
    .name     = "linux_manager",
    .version  = 0x0100,
    .provides = "linux_manager.tick,linux_manager.start,linux_manager.stop,"
                "linux_manager.active,linux_manager.status,linux_manager.fb",
    .deps     = "vnc_client",
    .init     = lm_init,
    .exit     = 0,
    .call     = lm_call,
};
