/* Minimal <stdlib.h> for freestanding DCN test harness (QEMU/i686-elf).
 * Test-only bump allocator: malloc grows a static heap; free is a no-op.
 * The whole test suite allocates well under 4 MiB total, so this is safe. */
#include "stdlib.h"
#include "string.h"

#define DCN_HEAP_SIZE (512u * 1024u)
static unsigned char g_heap[DCN_HEAP_SIZE] __attribute__((aligned(16)));
static size_t g_off = 0;

void* malloc(size_t size){
  if (size == 0) size = 1;
  size_t aligned = (size + 15u) & ~((size_t)15u);
  if (g_off + aligned > DCN_HEAP_SIZE) return 0;
  void* p = (void*)&g_heap[g_off];
  g_off += aligned;
  return p;
}

void* calloc(size_t n, size_t size){
  void* p = malloc(n * size);
  if (p) memset(p, 0, n * size);
  return p;
}

void free(void* p){ (void)p; /* bump allocator: no-op */ }
