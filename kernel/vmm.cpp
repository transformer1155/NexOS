#include "vmm.h"
#include <stdint.h>
#include "../mm.h"
#include "pmm.h"

/* Simple 32-bit paging helpers.
 * Assumptions:
 * - Kernel runs in 32-bit compatibility mode with identity-mapped physical
 *   memory while bootloader is active. This implementation uses physical
 *   addresses as direct pointers (identity) to allocate and manage page
 *   tables. It does not enable paging; it only prepares page-directory and
 *   page-tables and can install them to CR3 if requested.
 * - PTE/PDE format: present, rw, user bits in low 12 bits.
 */

#define PAGE_SIZE 0x1000
#define PDE_ENTRIES 1024
#define PTE_ENTRIES 1024

static uint32_t g_pgdir_phys = 0;

static inline void outl(uint16_t port, uint32_t val){ __asm__ __volatile__("outl %0,%1"::"a"(val),"Nd"(port)); }
static inline uint32_t inl_port(uint16_t port){ uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

static inline void invlpg(void* m){ __asm__ __volatile__("invlpg (%0)"::"r"(m):"memory"); }

void vmm_init(void){
	if (g_pgdir_phys) return; /* already inited */
	uint32_t pd = pmm_alloc_page();
	if (!pd) return; /* OOM -> leave as stub */
	/* zero the page directory */
	uint32_t *pdv = (uint32_t*)(uintptr_t)pd;
	for (int i = 0; i < PDE_ENTRIES; i++) pdv[i] = 0;
	g_pgdir_phys = pd;
}

int vmm_map_page(uint32_t phys_addr, uint32_t virt_addr, uint32_t flags){
	if (!g_pgdir_phys) vmm_init();
	if (!g_pgdir_phys) return -1; /* cannot allocate page directory */

	uint32_t pd_index = (virt_addr >> 22) & 0x3FF;
	uint32_t pt_index = (virt_addr >> 12) & 0x3FF;

	uint32_t *pd = (uint32_t*)(uintptr_t)g_pgdir_phys;
	uint32_t pde = pd[pd_index];
	uint32_t pt_phys;
	if (!(pde & 1)){
		/* allocate a new page table */
		pt_phys = pmm_alloc_page();
		if (!pt_phys) return -2; /* OOM */
		/* zero page table */
		uint32_t *ptv = (uint32_t*)(uintptr_t)pt_phys;
		for (int i = 0; i < PTE_ENTRIES; i++) ptv[i] = 0;
		/* set PDE: physical addr | flags (present) */
		pd[pd_index] = (pt_phys & 0xFFFFF000) | (flags & 0xFFF) | 0x1;
		pde = pd[pd_index];
	} else {
		pt_phys = pde & 0xFFFFF000;
	}

	uint32_t *pt = (uint32_t*)(uintptr_t)pt_phys;
	pt[pt_index] = (phys_addr & 0xFFFFF000) | (flags & 0xFFF) | 0x1;

	/* Invalidate TLB for this page */
	invlpg((void*)virt_addr);
	return 0;
}

int vmm_identity_map_range(uint32_t phys_start, uint32_t phys_size){
	/* Map phys_start..phys_start+phys_size identity into same virtual addresses. */
	if (!g_pgdir_phys) vmm_init();
	if (!g_pgdir_phys) return -1;
	uint32_t start = phys_start & 0xFFFFF000;
	uint32_t end = (phys_start + phys_size + 0xFFF) & 0xFFFFF000;
	for (uint32_t a = start; a < end; a += PAGE_SIZE){
		int r = vmm_map_page(a, a, 0x2); /* RW */
		if (r) return r;
	}
	return 0;
}

void vmm_set_cr3(uint32_t pd_phys){
	/* Load CR3 with the physical address of the page directory.
	 * Caller must ensure the page directory and tables are ready.
	 */
	__asm__ __volatile__("mov %0, %%cr3" :: "r"(pd_phys));
}

uint32_t vmm_get_cr3(void){
	uint32_t val;
	__asm__ __volatile__("mov %%cr3, %0" : "=r"(val));
	return val;
}
