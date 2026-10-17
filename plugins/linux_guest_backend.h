/* linux_guest_backend.h - transport-agnostic backend for the WSL2-style
 * "Linux in a NexOS window" feature.
 *
 * WHY THIS EXISTS
 * ---------------
 * The GUI window (APP_LINUX_GUEST) and the linux_manager lifecycle state
 * machine must NOT know whether the remote Linux desktop arrives over a
 * network VNC connection (development / virtualized host) or over a shared
 * in-RAM framebuffer served by a NexOS-built-in micro-hypervisor (bare-metal
 * production: NexOS opens a real VMX/SVM guest and the guest draws into a
 * memory region NexOS maps directly).
 *
 * Everything in the GUI talks to the LinuxGuestBackend interface only.  Two
 * implementations exist today / tomorrow:
 *   - lgb_vnc : wraps the existing vnc_client plugin + linux_manager FSM
 *               (used while NexOS itself runs inside QEMU#1 dev/CI).
 *   - lgb_vmm : built-in VMM (Intel VT-x / AMD-V) + shared-framebuffer
 *               (target production on real hardware).
 *
 * Switching backends is a one-line lgb_set_backend() call and requires no
 * GUI or lifecycle changes.
 */
#ifndef LINUX_GUEST_BACKEND_H
#define LINUX_GUEST_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A backend is just a vtable of function pointers.  All are optional-safe:
 * the bridges below tolerate NULL entries (they then behave as "not ready"). */
typedef struct {
    /* one-time setup (resolve services, map guest RAM, ...). 0 = ok. */
    int  (*init)(void);
    /* pull the Linux Guest up (start VM / connect VNC). 0 = requested. */
    int  (*launch)(void);
    /* tear the guest down (stop VM / disconnect VNC). 0 = requested. */
    int  (*shutdown)(void);
    /* true while a Linux window should be drawn. */
    int  (*active)(void);
    /* advance async work; returns >0 if a new frame is available. */
    int  (*poll)(uint32_t now_ms);
    /* fetch the current framebuffer: *fb (0x00RRGGBB row-major), *w,*h.
     * returns 1 if connected and a frame exists, else 0. */
    int  (*read_fb)(void** fb, int* w, int* h);
    /* forward input: client x,y, button mask, keysym (negative = release). */
    int  (*send_input)(int mx, int my, int btn, int keysym);
    /* high-level status string for the window watermark/title. */
    const char* (*status)(void);
} LinuxGuestBackend;

/* Set the active backend. Pass NULL to clear (GUI then shows "Linux"). */
void lgb_set_backend(const LinuxGuestBackend* b);
/* Get the currently selected backend (may be NULL). */
const LinuxGuestBackend* lgb_get_backend(void);
/* Convenience: switch to the built-in VMM backend (vmm.h) when hardware
 * virtualization (Intel VT-x / AMD-V) is present; otherwise leave the
 * default VNC dev backend in place.  Call once at GUI bring-up. */
void lgb_use_vmm_if_available(void);

/* ---- convenience bridges (GUI calls these; they dispatch to the backend)
 * Each is a no-op-safe wrapper that falls back to sensible behaviour when no
 * backend is installed or the corresponding slot is NULL. ---- */
int  lg_init(void);
int  lg_launch(void);
int  lg_shutdown(void);
int  lg_active(void);
int  lg_poll(uint32_t now_ms);
int  lg_read_fb(void** fb, int* w, int* h);
int  lg_send_input(int mx, int my, int btn, int keysym);
const char* lg_status(void);

#ifdef __cplusplus
}
#endif
#endif /* LINUX_GUEST_BACKEND_H */
