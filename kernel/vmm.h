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

/* Control helpers */
void vmm_set_cr3(uint32_t pd_phys);
uint32_t vmm_get_cr3(void);
uint32_t vmm_get_pd_phys(void);

/* Activate page tables: if enable_paging==0, only load CR3; if 1, attempt to enable PG.
 * Returns 0 on success, negative on error. By default this will NOT enable paging
 * to avoid disturbing the current execution environment; callers should invoke
 * with enable_paging=0 when only switching page tables.
 */
int vmm_activate_page_tables(int enable_paging);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_VMM_H
