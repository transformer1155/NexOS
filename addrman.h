#ifndef ADDRMAN_H
#define ADDRMAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 *  Address Management Registry
 *
 *  A single source of truth for the kernel's important addresses.
 *  The framebuffer bug we just fixed (32-bit window vs 64-bit real
 *  address mismatch) existed precisely because every subsystem read
 *  magic numbers / VbeInfo fields directly.  This registry is the one
 *  place that says "the GUI draws to VIRTUAL address X, which maps to
 *  PHYSICAL address Y".  Each kernel binary (32-bit kernel.bin and the
 *  long-mode kernel64.bin are separate link units with separate .bss,
 *  so each populates its OWN copy from VbeInfo at its own init).
 * ------------------------------------------------------------------ */

typedef enum {
    ADDR_VBE_INFO = 0,    // VbeInfo scratch struct            (0x5000)
    ADDR_STAGE_MARK,      // boot-stage milestone byte         (0x5101)
    ADDR_FAULT_MARK,      // CPU fault marker byte             (0x5100)
    ADDR_VGA_TEXT,        // VGA text buffer                   (0xB8000)
    ADDR_FB_WIN,          // <4GB shadow window for >4GB FB    (0xF0000000)
    ADDR_FB_PHYS,         // REAL physical framebuffer (may be >4GB)
    ADDR_FB_VIRT,         // virtual address the GUI draws to
                          //   32-bit: the 0xF0000000 window
                          //   64-bit: the real (identity-mapped) high FB
    ADDR_HEAP,            // kernel heap base                  (0x300000)
    ADDR_KERNEL64_LOAD,   // where kernel64.bin is staged/entered (0x100000)
    ADDR_COUNT
} addr_key_t;

#define ADDR_FLAG_RAM   0x01   // ordinary RAM
#define ADDR_FLAG_MMIO  0x02   // memory-mapped I/O / device
#define ADDR_FLAG_FB    0x04   // framebuffer

/* Register / update an address. size is in bytes (0 = unknown). */
void addr_set(addr_key_t key, uint64_t phys, uint64_t virt, uint64_t size, uint32_t flags);
void addr_set_phys(addr_key_t key, uint64_t phys);
void addr_set_virt(addr_key_t key, uint64_t virt);
void addr_set_size(addr_key_t key, uint64_t size);
void addr_set_flags(addr_key_t key, uint32_t flags);

uint64_t addr_phys(addr_key_t key);
uint64_t addr_virt(addr_key_t key);
uint64_t addr_size(addr_key_t key);
uint32_t addr_flags(addr_key_t key);
int      addr_is_set(addr_key_t key);

/* typed convenience accessors */
static inline void*            addr_virt_ptr(addr_key_t key) { return (void*)(uintptr_t)addr_virt(key); }
static inline volatile uint8_t*  addr_virt_u8(addr_key_t key)  { return (volatile uint8_t*)(uintptr_t)addr_virt(key); }
static inline volatile uint32_t* addr_virt_u32(addr_key_t key) { return (volatile uint32_t*)(uintptr_t)addr_virt(key); }

const char* addr_name(addr_key_t key);

/* Dump the whole registry to the serial port (for the `mem` debug cmd). */
void addr_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* ADDRMAN_H */
