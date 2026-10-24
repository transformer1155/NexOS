/* linux_guest_backend.cpp - backend dispatcher + the default VNC backend.
 *
 * The dispatcher (lg_* bridges) is backend-agnostic: it forwards every call
 * to the currently installed LinuxGuestBackend.  When no backend is set it
 * degrades to "inactive / no frame" so the GUI simply renders a watermark.
 *
 * lgb_vnc is the development/virtualized backend: it routes the interface to
 * the already-shipping vnc_client plugin + linux_manager lifecycle FSM.  It
 * keeps the existing behaviour byte-for-byte so the current build stays green.
 *
 * A future lgb_vmm (built-in hypervisor + shared framebuffer) registers the
 * same struct and is selected with lgb_set_backend(); nothing else changes.
 */
#include "linux_guest_backend.h"
#include "linux_manager.h"      /* lifecycle FSM + framebuffer accessor */
#include "vmm.h"                /* built-in hypervisor (bare-metal backend) */
#include "guest_blob.h"         /* minimal guest payload (guest_smoke.asm) */
#include "plugin_manager.h"    /* svc_fn, svc_lookup */

#include <stdint.h>

/* ---- vnc_client services (resolved lazily) ----------------------------- */
extern "C" int  vnc_session_fb(void** fb_out, int* w_out, int* h_out);
extern "C" void vnc_disconnect(void);

static svc_fn g_vnc_input = 0;
static svc_fn g_vnc_poll  = 0;
static int   g_svc_ready  = 0;

static void lgb_resolve_vnc(void) {
    if (g_svc_ready) return;
    g_vnc_input = svc_lookup("vnc.input");
    g_vnc_poll  = svc_lookup("vnc.poll");
    g_svc_ready = 1;
}

/* ---- the VNC backend vtable -------------------------------------------- */
static int  vnc_b_init(void)   { linux_manager_init(); return 0; }
static int  vnc_b_launch(void) { linux_manager_start(); return 0; }
static int  vnc_b_shutdown(void){ linux_manager_stop(); return 0; }
static int  vnc_b_active(void) { return linux_manager_active(); }
static int  vnc_b_poll(uint32_t now_ms) {
    linux_manager_tick(now_ms);          /* drives connect/retry/ingest */
    lgb_resolve_vnc();
    if (g_vnc_poll) return g_vnc_poll(0, 0, 0);  /* 1 = frame arrived */
    return 0;
}
static int  vnc_b_read_fb(void** fb, int* w, int* h) {
    return vnc_session_fb(fb, w, h);     /* 1 if connected + frame */
}
static int  vnc_b_send_input(int mx, int my, int btn, int keysym) {
    lgb_resolve_vnc();
    if (!g_vnc_input) return -1;
    int32_t a[4] = { mx, my, btn, keysym };
    return g_vnc_input(a, 0, 0);
}
static const char* vnc_b_status(void) { return linux_manager_status_text(); }

static const LinuxGuestBackend g_lgb_vnc = {
    vnc_b_init, vnc_b_launch, vnc_b_shutdown, vnc_b_active, vnc_b_poll,
    vnc_b_read_fb, vnc_b_send_input, vnc_b_status,
};

/* ---- dispatcher state -------------------------------------------------- */
static const LinuxGuestBackend* g_backend = &g_lgb_vnc;  /* default = VNC */

void lgb_set_backend(const LinuxGuestBackend* b) { g_backend = b; }
const LinuxGuestBackend* lgb_get_backend(void)  { return g_backend; }

/* ---- no-op-safe bridges (used by gui.cpp) ------------------------------ */
int lg_init(void) {
    if (g_backend && g_backend->init) return g_backend->init();
    return 0;
}
int lg_launch(void) {
    if (g_backend && g_backend->launch) return g_backend->launch();
    return 0;
}
int lg_shutdown(void) {
    if (g_backend && g_backend->shutdown) return g_backend->shutdown();
    return 0;
}
int lg_active(void) {
    if (g_backend && g_backend->active) return g_backend->active();
    return 0;
}
int lg_poll(uint32_t now_ms) {
    if (g_backend && g_backend->poll) return g_backend->poll(now_ms);
    return 0;
}
int lg_read_fb(void** fb, int* w, int* h) {
    if (g_backend && g_backend->read_fb) return g_backend->read_fb(fb, w, h);
    if (fb) *fb = 0; if (w) *w = 0; if (h) *h = 0;
    return 0;
}
int lg_send_input(int mx, int my, int btn, int keysym) {
    if (g_backend && g_backend->send_input)
        return g_backend->send_input(mx, my, btn, keysym);
    return -1;
}
const char* lg_status(void) {
    if (g_backend && g_backend->status) return g_backend->status();
    return "Linux";
}

/* ---- the built-in VMM backend (bare-metal production) ------------------ *
 * This wraps the vmm module.  On real hardware with Intel VT-x / AMD-V the
 * guest is a real virtual machine and its framebuffer is shared RAM that the
 * GUI reads directly (zero-copy).  Until the real VMX/SVM entry path is
 * finished (Phase 1 + 2), and on the development QEMU host that lacks
 * hardware virtualization, launch() simply validates the shared-FB path by
 * drawing a test pattern.  This proves the "NexOS window <-> guest RAM"
 * data flow end to end without needing a booted Linux yet.            */
static void* g_vmm_guest = 0;

static int  vmm_b_init(void)   {
    /* Only usable where hardware virtualization exists; otherwise no-op. */
    if (!vmm_available()) return -1;
    return vmm_init();
}
static int  vmm_b_launch(void) {
    if (!vmm_available()) return -1;
    if (!g_vmm_guest) g_vmm_guest = vmm_create_guest(0);
    if (!g_vmm_guest) return -1;

    /* Load the minimal guest program (guest_blob.h, assembled from
     * plugins/guest_smoke.asm) at GUEST_CODE_PHYS.  It paints the shared
     * framebuffer, emits "ready" on the vUART, then traps via the exit port.
     *
     * Today the payload runs on real hardware only (vmm_run_once needs the
     * VMX/SVM entry path).  Where no hardware virtualization exists, the
     * launch reports failure and the caller keeps using the VNC backend. */
    if (vmm_load_blob(g_vmm_guest, GUEST_CODE_PHYS, guest_blob,
                      (uint32_t)GUEST_BLOB_LEN) != 0) {
        return -1;
    }
    /* Run the guest.  On real hardware this enters the VM; the guest paints
     * the framebuffer and writes "ready" to the vUART before trapping back. */
    int r = vmm_run_once(g_vmm_guest);
    if (r < 0) return -1;
    return 0;
}
static int  vmm_b_shutdown(void) {
    if (g_vmm_guest) { vmm_shutdown(g_vmm_guest); g_vmm_guest = 0; }
    return 0;
}
static int  vmm_b_active(void) {
    return g_vmm_guest ? 1 : 0;
}
static int  vmm_b_poll(uint32_t now_ms) {
    (void)now_ms;
    if (!g_vmm_guest) return 0;
    /* Dispatch VM-exits.  Today this advances the emulated devices; once the
     * real VMX/SVM entry path is live it also services guest port I/O. */
    vmm_run_once(g_vmm_guest);
    /* Drain the guest's vUART into the shared line-protocol parser.  This is
     * the same parser the serial COM2 transport feeds, so "start"/"stop"/
     * "status" arriving from inside the VM behave identically. */
    for (;;) {
        int c = vmm_uart_tx_pop(g_vmm_guest);
        if (c < 0) break;
        linux_manager_feed_byte(c);
    }
    return 1;
}
static int  vmm_b_read_fb(void** fb, int* w, int* h) {
    if (!g_vmm_guest) { if (fb) *fb = 0; if (w) *w = 0; if (h) *h = 0; return 0; }
    void* p = vmm_guest_fb(g_vmm_guest, w, h);
    if (fb) *fb = p;
    return (p && *w > 0 && *h > 0) ? 1 : 0;
}
static int  vmm_b_send_input(int mx, int my, int btn, int keysym) {
    if (!g_vmm_guest) return -1;
    /* Keyboard goes to the guest as vUART bytes (same stream as a serial
     * console).  Pointer motion will move to a virtual HID device once the
     * guest-side driver exists; encode it as a compact escape sequence now so
     * the guest side is testable. */
    if (keysym != 0) {
        int ks = keysym < 0 ? -keysym : keysym;
        if (ks < 0x100) vmm_uart_rx_push(g_vmm_guest, (uint8_t)ks);
        return 0;
    }
    if (mx >= 0 && my >= 0 && mx < 65536 && my < 65536) {
        vmm_uart_rx_push(g_vmm_guest, 0x1B);            /* ESC = pointer event */
        vmm_uart_rx_push(g_vmm_guest, (uint8_t)(mx & 0xFF));
        vmm_uart_rx_push(g_vmm_guest, (uint8_t)(my & 0xFF));
        vmm_uart_rx_push(g_vmm_guest, (uint8_t)(btn & 0xFF));
    }
    return 0;
}
static const char* vmm_b_status(void) {
    if (!vmm_available()) return "Linux (VMM n/a: no VT-x/AMD-V)";
    return g_vmm_guest ? "Linux (VMM)" : "Starting Linux (VMM)...";
}

static const LinuxGuestBackend g_lgb_vmm = {
    vmm_b_init, vmm_b_launch, vmm_b_shutdown, vmm_b_active, vmm_b_poll,
    vmm_b_read_fb, vmm_b_send_input, vmm_b_status,
};

/* Selector: pick the VMM backend when hardware virtualization is present.
 * Runs a full bring-up probe (init + VMRUN of the smoke guest) once at GUI
 * bring-up so the serial log carries the SVM bring-up result even before any
 * Linux window is opened -- the SVM plumbing is debugged from boot logs. */
void lgb_use_vmm_if_available(void) {
    if (!vmm_available()) {
        pm_serial("[VMM2] hw virtualization: none (falling back to VNC)\n");
        return;
    }
    pm_serial("[VMM2] hw virtualization detected, probing VMRUN...\n");
    if (vmm_b_init() != 0) {
        pm_serial("[VMM2] vmm_init failed\n");
        return;
    }
    /* Probe: load + run the smoke guest once. */
    g_vmm_guest = vmm_create_guest(0);
    if (!g_vmm_guest) { pm_serial("[VMM2] guest alloc failed\n"); return; }
    if (vmm_load_blob(g_vmm_guest, GUEST_CODE_PHYS, guest_blob,
                      (uint32_t)GUEST_BLOB_LEN) != 0) {
        pm_serial("[VMM2] blob load failed\n");
        return;
    }
    int r = vmm_run_once(g_vmm_guest);
    if (r < 0) pm_serial("[VMM2] VMRUN probe FAILED (see [VMM] dump above)\n");
    else {
        pm_serial("[VMM2] VMRUN probe OK: guest entered and exited\n");
        /* Verify the guest actually did its job:
         *  - vUART bytes ("ready") should sit in the tx ring,
         *  - framebuffer pixel(0,0) should be the smoke gradient (blue 0x80). */
        uint32_t tx = vmm_uart_tx_count(g_vmm_guest);
        pm_serial("[VMM2] vuart tx bytes: ");
        { char nb[12]; int ni=0,t=(int)tx; if(!t)nb[ni++]='0'; else{char tmp[10];int tn=0;while(t){tmp[tn++]=('0'+t%10);t/=10;}while(tn)nb[ni++]=tmp[--tn];} nb[ni]=0;
          for(int i=0;i<ni;i++) pm_serial(""); }
        pm_serial("\n");
        int w=0,h=0; uint32_t* fb=(uint32_t*)vmm_guest_fb(g_vmm_guest,&w,&h);
        if (fb) {
            uint32_t p0=fb[0], pm=fb[(h/2)*w+w/2];
            pm_serial("[VMM2] fb pixel(0,0)=0x");
            static const char* hx="0123456789ABCDEF";
            for(int i=7;i>=0;i--) { char c[2]={hx[(p0>>(i*4))&0xF],0}; pm_serial(c); }
            pm_serial(" pixel(mid)=0x");
            for(int i=7;i>=0;i--) { char c[2]={hx[(pm>>(i*4))&0xF],0}; pm_serial(c); }
            pm_serial("\n");
        }
        /* drain any vUART text the guest emitted */
        for (;;) {
            int c = vmm_uart_tx_pop(g_vmm_guest);
            if (c < 0) break;
            char cc[2] = { (char)c, 0 };
            pm_serial(cc);
        }
        pm_serial("\n");
        lgb_set_backend(&g_lgb_vmm);
    }
}
