#include "vmm.h"
#include <stdint.h>
#include "mm.h"

/* Minimal stub VMM: for initial implementation we assume kernel runs
 * with identity-mapped pages provided by bootloader/UEFI. vmm_map_page
 * returns success without changing page tables. Later this file will
 * implement real page table manipulation.
 */

void vmm_init(void){
	/* no-op for now */
}

int vmm_map_page(uint32_t phys_addr, uint32_t virt_addr, uint32_t flags){
	(void)phys_addr; (void)virt_addr; (void)flags;
	/* TODO: implement real mapping. Return 0 = success. */
	return 0;
}

int vmm_identity_map_range(uint32_t phys_start, uint32_t phys_size){
	(void)phys_start; (void)phys_size;
	/* assume identity mapping already exists */
	return 0;
}
