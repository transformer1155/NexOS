#ifndef NEXOS_VMM_H
#define NEXOS_VMM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal VMM interface for mapping pages. Implementations may be no-op
 * if kernel runs with identity-mapped physical memory.
 */

void vmm_init(void);
int vmm_map_page(uint32_t phys_addr, uint32_t virt_addr, uint32_t flags);
int vmm_identity_map_range(uint32_t phys_start, uint32_t phys_size);
uint32_t vmm_get_phys(uint32_t virt);   /* returns physical address or 0 if unmapped */

#ifdef __cplusplus
}
#endif

#endif // NEXOS_VMM_H
