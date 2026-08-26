// memory_adapter.c -- ggml memory backend for the NexOS 64-bit kernel.
//
// Strict Route A port, phase 0.  See memory_adapter.h.  Freestanding: no
// libc, no STL.  Compiled with the 64-bit C++ driver (CC64) in a C-compatible
// subset, so everything is declared extern "C".
#include "memory_adapter.h"

// big_alloc/big_free are implemented in kernel64.cpp with extern "C" linkage
// (the long-mode PMM, identity-mapped so the returned pointer is directly
// usable).  They take a uint32_t size -> capped at 4 GiB per allocation.
extern "C" void* big_alloc(uint32_t bytes);
extern "C" void  big_free(void* p, uint32_t bytes);

// Optional >4 GiB arena owner (phase 5).  NULL until installed.
static void* (*g_large_alloc)(uint64_t bytes) = 0;

void ggml_set_large_alloc(void* (*fn)(uint64_t bytes)){
    g_large_alloc = fn;
}

void* ggml_alloc(uint64_t bytes){
    if (bytes == 0) return 0;
    if (bytes > 0xFFFFFFFFull){
        // Needs the >4 GiB arena.
        return g_large_alloc ? g_large_alloc(bytes) : 0;
    }
    return big_alloc((uint32_t)bytes);
}

void ggml_free(void* p, uint64_t bytes){
    if (!p) return;
    if (bytes > 0xFFFFFFFFull){
        // Large blocks are owned/freed by the arena installer; the kernel PMM
        // cannot represent them, so do not call big_free on them.
        return;
    }
    big_free(p, (uint32_t)bytes);
}

void* ggml_alloc_large(uint64_t bytes){
    return g_large_alloc ? g_large_alloc(bytes) : 0;
}
