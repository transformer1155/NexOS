#ifndef NEXOS_PMM_H
#define NEXOS_PMM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void pmm_init(uint32_t phys_base, uint32_t phys_size);
uint32_t pmm_alloc_page(void);   /* returns physical address of 4KiB page or 0 on OOM */
void pmm_free_page(uint32_t phys_addr);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_PMM_H
