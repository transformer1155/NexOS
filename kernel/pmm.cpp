#include "pmm.h"
#include <stdint.h>
#include "mm.h"

/* Simple physical page allocator: bump + free-stack for reclaimed pages.
 * Not a production allocator, but sufficient for P1 testing.
 */

static uint32_t g_phys_start = 0;
static uint32_t g_phys_end = 0;
static uint32_t g_next_free = 0; /* next free physical address (4KiB aligned) */

#define PMM_FREE_STACK_CAP 1024
static uint32_t g_free_stack[PMM_FREE_STACK_CAP];
static int g_free_top = 0;

void pmm_init(uint32_t phys_base, uint32_t phys_size){
	/* align base up to 4KiB and size down to multiple of 4KiB */
	uint32_t base = (phys_base + 0xFFF) & ~0xFFFu;
	uint32_t end = (phys_base + phys_size) & ~0xFFFu;
	g_phys_start = base;
	g_phys_end = end;
	g_next_free = base;
	g_free_top = 0;
}

uint32_t pmm_alloc_page(void){
	if (g_free_top > 0){
		return g_free_stack[--g_free_top];
	}
	if (g_next_free + 0x1000 > g_phys_end) return 0; /* OOM */
	uint32_t r = g_next_free;
	g_next_free += 0x1000;
	return r;
}

void pmm_free_page(uint32_t phys_addr){
	if (phys_addr < g_phys_start || phys_addr >= g_phys_end) return;
	if (g_free_top < PMM_FREE_STACK_CAP) g_free_stack[g_free_top++] = phys_addr;
}
