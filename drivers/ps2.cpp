#include "ps2.h"
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val){ __asm__ __volatile__("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline uint8_t inb(uint16_t port){ uint8_t v; __asm__ __volatile__("inb %1,%0":"=a"(v):"Nd"(port)); return v; }

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64

void ps2_init(void){
	/* Flush any pending data */
	for (int i = 0; i < 100; i++){
		uint8_t s = inb(PS2_STATUS_PORT);
		if (s & 1) { (void)inb(PS2_DATA_PORT); }
	}
	/* Minimal init: enable IRQ and scanning is left to BIOS/keyboard controller */
}

int ps2_has_scancode(void){
	uint8_t s = inb(PS2_STATUS_PORT);
	return (s & 1) ? 1 : 0;
}

uint8_t ps2_read_scancode(void){
	while (!ps2_has_scancode()) ;
	return inb(PS2_DATA_PORT);
}
