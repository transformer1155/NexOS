// memory_adapter.h -- ggml memory backend for the NexOS 64-bit kernel.
//
// Phase 0 of the strict Route A port (llama.cpp -> NexOS parallel ggml
// subsystem).  Wraps the kernel's PMM-backed big_alloc/big_free behind a
// ggml-style allocator and reserves a hook for the >4 GiB arena the 7B goal
// needs (phase 5).  Mirrors llama.cpp's ggml_backend_buft / ggml_alloc
// contract: one allocation primitive, backend-swappable.
//
// C-compatible: linked from both the C adapter files and kernel64.cpp (C++).
#ifndef NEXOS_MEMORY_ADAPTER_H
#define NEXOS_MEMORY_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Allocate `bytes` of kernel memory for model weights / tensors.
// Backed by the long-mode PMM (big_alloc).  The current PMM can only serve
// <4 GiB contiguous ranges (big_alloc takes a uint32_t), so requests above
// 4 GiB are routed to the registered "large" allocator (phase 5: 1 GiB-page
// arena) when one is installed, otherwise NULL.
void* ggml_alloc(uint64_t bytes);

// Free a block previously returned by ggml_alloc / ggml_alloc_large.
// `bytes` must match the value passed to the allocating call.
void  ggml_free(void* p, uint64_t bytes);

// Allocate >4 GiB (e.g. a 7B Q4_K_M weights blob ~4.x GiB).  Returns NULL
// unless a large allocator was installed via ggml_set_large_alloc().
void* ggml_alloc_large(uint64_t bytes);

// Install the kernel's >4 GiB allocator (called once at startup, phase 5+).
// The arena owner is also responsible for freeing those blocks.
void  ggml_set_large_alloc(void* (*fn)(uint64_t bytes));

#ifdef __cplusplus
}
#endif

#endif // NEXOS_MEMORY_ADAPTER_H
