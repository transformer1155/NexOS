// nexos_compat.cpp — AI-assisted Win32 compat layer (D/E class) — MINIMAL STUB.
//
// The real implementation (PE checkpoint/resume via setjmp/longjmp, fault
// observation, E1 heap/PMM health sampler, D4 fault recovery) is built on
// top of these entry points.  For now the symbols are provided as no-ops so
// the kernel compiles and links; the behaviour is filled in by the layer
// owner.  Keeping the stubs means kernel64.cpp's call sites are stable.
#include "nexos_compat.h"
#include "osapi.h"   // osapi_mem_stats_attach

// Stored so the hook pointers are "used" (no unused-variable warnings) and
// available for the real implementation to consult.
static compat_read_hook_t  g_compat_read  = 0;
static compat_write_hook_t g_compat_write = 0;

void compat_init(void){
    // No-op stub.  Real layer arms its state here.
}

void compat_attach_volume(compat_read_hook_t read_fn, compat_write_hook_t write_fn){
    g_compat_read  = read_fn;
    g_compat_write = write_fn;
}

void compat_on_fault(int intno, int errcode, uint64_t rip, uint64_t cr2){
    (void)intno; (void)errcode; (void)rip; (void)cr2;
    // No-op stub.  Real layer records the fault for the external AI.
}

// compat_abort_pe() is meant to longjmp back into the PE resume checkpoint.
// Without the checkpoint machinery present, a no-op is the safe behaviour:
// the caller (fault_common) parks the machine on a real fault anyway.
void compat_abort_pe(void){
    // No-op stub.
}
