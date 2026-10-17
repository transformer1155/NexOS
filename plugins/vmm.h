#include <stdint.h>

/* vmm.h - minimal built-in hypervisor core for the WSL2-style Linux Guest.
 *
 * PRODUCTION TARGET: NexOS deployed on bare metal.  NexOS becomes a small
 * type-2-style hypervisor: it opens a real hardware-virtualized guest
 * (Intel VT-x VMX or AMD-V SVM) and runs the Linux Guest inside it.  The
 * guest's framebuffer lives in a RAM region NexOS maps directly, so the GUI
 * window reads pixels with zero-copy (no network VNC).
 *
 * This header is intentionally tiny: the heavy lifting (VMCS/VMCB build,
 * vmlaunch/vmrun, VM-exit dispatch) lives in vmm.cpp and is exposed only
 * through the functions below.  Nothing here runs unless vmm_available()
 * returns true -- which, on the development QEMU host, it does NOT, because
 * QEMU does not expose VMX/SVM to the guest by default.  That keeps the
 * kernel bootable and the build green everywhere.
 *
 * Detected technologies:
 *   VMM_NONE  - no hardware virtualization (fall back to the VNC backend)
 *   VMM_VMX   - Intel VT-x
 *   VMM_SVM   - AMD-V
 */
#ifndef NEXOS_VMM_H
#define NEXOS_VMM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VMM_NONE = 0,
    VMM_VMX,
    VMM_SVM
} VmmKind;

/* One-time capability detection.  Cheap (a handful of CPUID leaves + MSR
 * reads); safe to call repeatedly.  Call vmm_init() before create_guest(). */
VmmKind vmm_detect(void);
VmmKind vmm_kind(void);                 /* last detected kind */
int     vmm_available(void);            /* != 0 if a hypervisor can run */

/* Initialise the virtualization subsystem (allocates the guest RAM region,
 * enables VMX/SVM if needed).  0 = ok, <0 = unavailable / error.  On a host
 * without hardware virtualization this simply returns -1 and does nothing. */
int     vmm_init(void);

/* Create a guest with `guest_pages` of physical RAM (4 KiB each).  Returns a
 * guest handle (opaque) or 0 on failure.  The guest is NOT running yet. */
void*   vmm_create_guest(uint32_t guest_pages);

/* Map the guest's shared framebuffer region for the GUI to read.  Returns a
 * kernel-virtual pointer and fills w/h (set at guest creation).  NULL if no
 * guest/framebuffer. */
void*   vmm_guest_fb(void* g, int* w, int* h);

/* Run the guest until the next VM-exit, then dispatch device emulation.
 * Returns the number of VM-exits handled (>=1) or <0 on fatal error. */
int     vmm_run_once(void* g);

/* ---- emulated devices -------------------------------------------------
 * The guest gets a 8250-compatible UART (vUART) carrying our line protocol,
 * and a block device (vDisk) backed by a NexOS file.  VM-exit dispatch
 * funnels guest port I/O here.
 */
int     vmm_uart_tx_pending(void* g);            /* 1 = a byte is available */
int     vmm_uart_tx_pop(void* g);                /* -1 if empty, else 0..255 */
void    vmm_uart_rx_push(void* g, uint8_t b);    /* host -> guest byte */
uint32_t vmm_uart_tx_count(void* g);             /* bytes guest wrote so far */

/* vDisk: attach a NexOS file as a raw sector image. */
int     vmm_disk_attach(void* g, const char* path, uint32_t sectors);
int     vmm_disk_read(void* g, uint64_t lba, void* buf, uint32_t count);
int     vmm_disk_ready(void* g);

/* Tear everything down. */
void    vmm_shutdown(void* g);

#ifdef __cplusplus
}
#endif
#endif /* NEXOS_VMM_H */
