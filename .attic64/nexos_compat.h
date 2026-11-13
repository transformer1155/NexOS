// nexos_compat.h — AI-assisted Win32 compat layer (D/E class) entry points.
//
// This is a minimal stub header.  The full layer (PE checkpoint/resume,
// fault observation, the D4 fault-recovery machinery) is implemented in
// nexos_compat.cpp.  Only the kernel-facing entry points are declared here
// so kernel64.cpp compiles and links even before the layer is fleshed out.
#ifndef NEXOS_COMPAT_H
#define NEXOS_COMPAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Drive the layer's per-frame pump (called from the kernel frame loop).
void compat_init(void);

// Hand the layer the MKFS data-volume read/write hooks so it can watch the
// PE's on-disk footprint.
typedef int  (*compat_read_hook_t)(const char* name, void* buf, int cap);
typedef int  (*compat_write_hook_t)(const char* name, const unsigned char* data, int len);
void compat_attach_volume(compat_read_hook_t read_fn, compat_write_hook_t write_fn);

// Fault observation hooks, invoked from fault_common() in kernel64.cpp.
void compat_on_fault(int intno, int errcode, uint64_t rip, uint64_t cr2);
void compat_abort_pe(void);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_COMPAT_H
