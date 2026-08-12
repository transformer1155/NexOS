#include "ata.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val){ __asm__ __volatile__("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline uint8_t inb(uint16_t port){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port){ uint16_t v; __asm__ __volatile__("inw %1,%0":"=a"(v):"Nd"(port)); return v; }

#define ATA_CMD_READ_PIO 0x20

int ata_pio_read_lba28(uint32_t lba, uint8_t count, void* buf){
	if (count == 0) return -1;
	uint8_t* out = (uint8_t*)buf;
	for (uint8_t c = 0; c < count; c++){
		uint32_t cur_lba = lba + c;
		/* select drive/head: 0xE0 for master, 0xF0 for slave; we use master */
		outb(0x1F6, 0xE0 | ((cur_lba >> 24) & 0x0F));
		outb(0x1F2, 1); /* sector count */
		outb(0x1F3, (uint8_t)(cur_lba & 0xFF));
		outb(0x1F4, (uint8_t)((cur_lba >> 8) & 0xFF));
		outb(0x1F5, (uint8_t)((cur_lba >> 16) & 0xFF));
		outb(0x1F7, ATA_CMD_READ_PIO);
		/* wait for BSY=0 and DRQ=1 */
		for (int i = 0; i < 100000; i++){
			uint8_t s = inb(0x1F7);
			if (!(s & 0x80) && (s & 0x08)) break;
		}
		/* read 256 words (512 bytes) */
		for (int i = 0; i < 256; i++){
			uint16_t w = inw(0x1F0);
			*out++ = (uint8_t)(w & 0xFF);
			*out++ = (uint8_t)((w >> 8) & 0xFF);
		}
	}
	return 0;
}
