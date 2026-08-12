#include "pmm.h"
#include <stdint.h>
#include "../mm.h"

/* Bitmap-based physical page allocator. Each bit represents one 4KiB page.
 * Pros: predictable allocation, reuse, O(1) alloc/free with a free hint.
 * This implementation places the bitmap at the start of managed region.
 */



static uint32_t g_phys_start = 0;
static uint32_t g_phys_end = 0;
static uint32_t g_page_count = 0;
static uint32_t *g_bitmap = 0; /* points to words inside managed region */
static uint32_t g_bitmap_words = 0;
static uint32_t g_free_hint = 0; /* word index hint to speed up search */

static inline void bit_set(uint32_t idx){ g_bitmap[idx >> 5] |= (1u << (idx & 31)); }
static inline void bit_clear(uint32_t idx){ g_bitmap[idx >> 5] &= ~(1u << (idx & 31)); }
static inline int bit_test(uint32_t idx){ return (g_bitmap[idx >> 5] >> (idx & 31)) & 1; }

void pmm_init(uint32_t phys_base, uint32_t phys_size){
	/* align base up to 4KiB and size down to multiple of 4KiB */
	uint32_t base = (phys_base + 0xFFF) & ~0xFFFu;
	uint32_t end = (phys_base + phys_size) & ~0xFFFu;
	g_phys_start = base;
	g_phys_end = end;

	g_page_count = (g_phys_end - g_phys_start) / 0x1000;
	g_bitmap_words = (g_page_count + 31) / 32;

	/* Place bitmap at start of region */
	g_bitmap = (uint32_t*)(uintptr_t)g_phys_start;
	/* Clear bitmap */
	for (uint32_t i = 0; i < g_bitmap_words; i++) g_bitmap[i] = 0;

	/* Reserve bitmap pages themselves as used */
	uint32_t bitmap_bytes = g_bitmap_words * 4;
	uint32_t bitmap_pages = (bitmap_bytes + 0xFFF) / 0x1000;
	for (uint32_t i = 0; i < bitmap_pages; i++) bit_set(i);

	g_free_hint = bitmap_pages >> 5; /* start searching after bitmap */
}

uint32_t pmm_alloc_page(void){
	if (!g_bitmap) return 0;
	for (uint32_t w = g_free_hint; w < g_bitmap_words; w++){
		uint32_t v = ~g_bitmap[w];
		if (v){
			/* find first set bit in v */
			uint32_t bit = __builtin_ctz(v);
			uint32_t idx = (w << 5) + bit;
			if (idx >= g_page_count) return 0;
			bit_set(idx);
			g_free_hint = w;
			return g_phys_start + idx * 0x1000;
		}
	}
	/* wrap-around search */
	for (uint32_t w = 0; w < g_free_hint; w++){
		uint32_t v = ~g_bitmap[w];
		if (v){
			uint32_t bit = __builtin_ctz(v);
			uint32_t idx = (w << 5) + bit;
			if (idx >= g_page_count) return 0;
			bit_set(idx);
			g_free_hint = w;
			return g_phys_start + idx * 0x1000;
		}
	}
	return 0; /* OOM */
}

void pmm_free_page(uint32_t phys_addr){
	if (phys_addr < g_phys_start || phys_addr >= g_phys_end) return;
	uint32_t idx = (phys_addr - g_phys_start) / 0x1000;
	if (!bit_test(idx)) return; /* double free guard */
	bit_clear(idx);
	uint32_t w = idx >> 5;
	if (w < g_free_hint) g_free_hint = w;
}
