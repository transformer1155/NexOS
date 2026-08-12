#ifndef NEXOS_PCI_H
#define NEXOS_PCI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PciDevice {
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint16_t vendor_id;
	uint16_t device_id;
};

/* Scan PCI buses 0..255 and fill up to max_devices entries. Returns number found. */
int pci_scan(struct PciDevice* out, int max_devices);
uint32_t pci_read_config_dword(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_PCI_H
