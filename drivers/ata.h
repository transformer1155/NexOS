#ifndef NEXOS_ATA_H
#define NEXOS_ATA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read `count` sectors starting at `lba` into `buf`. Returns 0 on success. */
int ata_pio_read_lba28(uint32_t lba, uint8_t count, void* buf);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_ATA_H
