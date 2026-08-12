#ifndef NexOS_MM_H
#define NexOS_MM_H

#include <stdint.h>

// Ring-3 accessible memory region (identity-mapped, PG_USER).
// QEMU -m 128M => RAM is 0x00000000..0x08000000, so this whole
// region is backed by real RAM.  The page tables are set up inside
// kernel.cpp's vmm_init() (which maps the first 32 MiB supervisor +
// this user region); this header just publishes the bounds so the
// GDT / syscall / loader code can agree on where user space lives.
#define USER_BASE  0x04000000u   // 64 MiB
#define USER_SIZE  0x04000000u   // 64 MiB
#define USER_END   (USER_BASE + USER_SIZE)

#endif // NexOS_MM_H
