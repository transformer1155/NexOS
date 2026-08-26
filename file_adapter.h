// file_adapter.h -- GGUF blob I/O backend abstraction (Route A, phase 0).
//
// llama.cpp reads model files either via mmap (host) or a file stream; the
// GGUF parser never touches the OS file API directly.  We mirror that with a
// backend-agnostic byte reader so the SAME parser (gguf_loader.c) works over:
//   * a model already copied into RAM (mem backend), or
//   * the raw disk blob appended after the SFS region (disk backend).
//
// The disk backend serves the payload through a sector-read callback supplied
// by the kernel, so file_adapter.c stays free of any ATA/PCI knowledge and
// links cleanly into the freestanding 64-bit kernel.
#ifndef NEXOS_FILE_ADAPTER_H
#define NEXOS_FILE_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sector reader supplied by the kernel: read one 512-byte sector at `lba`
// into `buf` (256 uint16_t words).  Maps to kernel64's ata_read_sector.
typedef void (*gguf_sector_read_fn)(uint32_t lba, uint16_t* buf);

// Backend-agnostic byte reader for a GGUF blob.  All offsets are absolute
// within the blob (0 == first byte of the GGUF magic).
typedef struct gguf_io {
    uint64_t (*read)(struct gguf_io* io, uint64_t off, void* dst, uint64_t len);
    uint64_t (*size)(struct gguf_io* io);
    void*    ctx;        // backend-private
} gguf_io;

// In-RAM backend: the whole blob is already resident at `base`.
gguf_io* gguf_io_mem_create(const uint8_t* base, uint64_t size);
void     gguf_io_mem_destroy(gguf_io* io);

// Raw-disk backend: probe the "MINIMDL1" descriptor at the candidate header
// LBAs, then serve the payload through `read_fn`.  Returns NULL when no blob
// is present on disk.  `hdr_lbas` is a 0-terminated list, e.g. {16383,8191,4095,0}.
gguf_io* gguf_io_disk_create(const uint32_t* hdr_lbas, gguf_sector_read_fn read_fn);
void     gguf_io_disk_destroy(gguf_io* io);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_FILE_ADAPTER_H
