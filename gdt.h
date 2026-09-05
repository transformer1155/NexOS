#ifndef NexOS_GDT_H
#define NexOS_GDT_H

#include <stdint.h>

// Sets up the Global Descriptor Table with ring-0 (kernel) and ring-3 (user)
// code/data segments plus a TSS, and loads it. Required before any ring-3
// transition (Foundation 0 of the security model).
void gdt_init(void);

// Linux-compat TLS: point the DPL3 TLS descriptor (selector 0x3B) at the
// guest's thread-local storage block (syscall 243 set_thread_area).
// Returns 0 on success.
int gdt_set_tls(uint32_t base, uint32_t limit);

// Per-thread TLS pool (Stage 1): allocate/free a DPL3 TLS descriptor from
// the pool.  gdt_alloc_tls returns the GS selector (0x40+idx*8) or -1 if
// the pool is full; gdt_free_tls releases it.
int  gdt_alloc_tls(uint32_t base, uint32_t limit);
void gdt_free_tls(int selector);

#endif // NexOS_GDT_H
