/* linux_manager.h - WSL2-style Linux Guest lifecycle manager for NexOS
 *
 * Owns the lifecycle of a nested Linux Guest (a second QEMU VM running
 * x11vnc).  NexOS cannot spawn a sibling VM itself (it is itself a guest),
 * so the actual launch/destroy lives in a host-side orchestrator that the
 * real machine runs.  NexOS talks to that orchestrator over a *serial*
 * control channel using a tiny line protocol:
 *
 *      "start"            -> host brings up the Linux Guest + VNC
 *      "stop"             -> host tears the Guest down
 *      "status"           -> host replies "ready" / "down" / "busy" / "error"
 *      "ready" (from host)-> VNC port is now open at 10.0.2.2:15900
 *
 * The serial transport here is a dedicated control UART (COM2, 0x2F8) so
 * the protocol stays byte-identical to a future virtio-serial backend; only
 * serial_putc()/serial_getc() need swapping to migrate to virtio-serial.
 *
 * VNC connection + framebuffer decode is performed by the vnc_client
 * plugin; linux_manager only:
 *   - sends start/stop to the host,
 *   - drives a non-blocking retry state machine that asks vnc_client to
 *     connect once the Guest is ready,
 *   - mirrors the live framebuffer (vnc_client writes it, gui.cpp reads it)
 *     so a NexOS window can composite the Linux desktop seamlessly.
 *
 * State machine (all non-blocking, ticked from the GUI idle loop):
 *      IDLE -> STARTING --(host "ready")--> VNC_CONNECTING
 *           -> STREAMING --(window close / stop) --> STOPPING -> IDLE
 */
#ifndef LINUX_MANAGER_H
#define LINUX_MANAGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LM_IDLE = 0,        /* no guest */
    LM_STARTING,        /* start cmd sent, waiting for host "ready" */
    LM_VNC_CONNECTING,  /* host ready, asking vnc_client to connect (retry) */
    LM_STREAMING,       /* VNC up, frames arriving */
    LM_STOPPING         /* stop cmd sent, waiting for host "down" */
} LinuxManagerState;

/* Shared framebuffer the vnc_client plugin decodes into.  gui.cpp blits
 * from here into a window's client area.  Big-endian/LE-native pixels are
 * stored as NexOS 0x00RRGGBB Color.  Locked only by the single GUI thread. */
typedef struct {
    int          fb_w;       /* remote RFB framebuffer width  (0 until known) */
    int          fb_h;       /* remote RFB framebuffer height (0 until known) */
    int          cap;        /* pixel capacity of fb_buf (== fb_w*fb_h) */
    uint32_t*    fb_buf;     /* decoded pixels, row-major, screen-sized copy */
    int          dirty_x, dirty_y, dirty_w, dirty_h; /* last dirty rect */
    int          have_frame; /* 1 once at least one frame arrived */
} LinuxGuestFB;

/* ---- lifecycle API (called by gui.cpp) ---- */
void     linux_manager_init(void);
/* User clicked "启动 Linux": send start, open the placeholder window. */
void     linux_manager_start(void);
/* Window closed / desktop shut down: send stop, disconnect VNC. */
void     linux_manager_stop(void);
/* Called every GUI frame (idle-safe). now_ms is a monotonic millisecond
 * clock supplied by the caller (gui.cpp uses g_wm.tick_ms_now()). Drives the
 * retry state machine. */
void     linux_manager_tick(uint32_t now_ms);
/* True while a Linux window is open and we should draw it. */
int      linux_manager_active(void);
/* Current high-level status string for the window title / watermark. */
const char* linux_manager_status_text(void);

/* ---- framebuffer access (called by vnc_client + gui.cpp) ---- */
LinuxGuestFB* linux_manager_fb(void);
/* vnc_client registers where it wants to write decoded pixels. Returns the
 * buffer it should use (caller keeps ownership of fb_buf via linux_manager). */
void     linux_manager_attach_vnc(void);   /* vnc plugin entered STREAMING */
void     linux_manager_detach_vnc(void);   /* vnc plugin disconnected */

/* ---- transport-agnostic control-channel injection ---------------------
 * The line protocol ("start"/"stop"/"status" -> "ready"/"busy"/"down"/"error")
 * is transport-independent.  Historically the bytes came from serial COM2;
 * with the built-in VMM they come from the guest's emulated vUART.  Feeding
 * a byte through this function is exactly equivalent to the byte arriving
 * from the UART, so both backends share one parser.
 * Returns 1 if a complete line was consumed (a state transition happened). */
int      linux_manager_feed_byte(int c);
/* Queue a host->guest reply (used by backends that must answer a "status"). */
void     linux_manager_push_reply(const char* line);

#ifdef __cplusplus
}
#endif
#endif /* LINUX_MANAGER_H */
