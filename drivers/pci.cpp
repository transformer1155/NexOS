#include "pci.h"
#include <stdint.h>

static inline void outl(uint16_t port, uint32_t val){ __asm__ __volatile__("outl %0,%1"::"a"(val),"Nd"(port)); }
static inline uint32_t inl(uint16_t port){ uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(port)); return v; }

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset){
	uint32_t addr = (uint32_t)(0x80000000u |
		((uint32_t)bus << 16) |
		((uint32_t)device << 11) |
		((uint32_t)func << 8) |
		(offset & 0xFC));
	outl(PCI_CONFIG_ADDRESS, addr);
	return inl(PCI_CONFIG_DATA);
}

int pci_scan(struct PciDevice* out, int max_devices){
	int found = 0;
	for (uint8_t bus = 0; bus < 256; bus++){
		for (uint8_t dev = 0; dev < 32; dev++){
			for (uint8_t func = 0; func < 8; func++){
				uint32_t v = pci_read_config_dword(bus, dev, func, 0);
				uint16_t vendor = (uint16_t)(v & 0xFFFF);
				if (vendor == 0xFFFF) {
					if (func == 0) break; /* no device, skip to next dev */
					else continue;
				}
				uint16_t device = (uint16_t)((v >> 16) & 0xFFFF);
				if (found < max_devices){
					out[found].bus = bus;
					out[found].device = dev;
					out[found].function = func;
					out[found].vendor_id = vendor;
					out[found].device_id = device;
				}
				found++;
				/* If function 0 indicates single-function device, skip remaining funcs */
				if (func == 0){
					uint32_t hdr = pci_read_config_dword(bus, dev, func, 0x0C);
					if (!((hdr >> 16) & 0x80)) break; /* single function */
				}
			}
		}
	}
	return found;
}
