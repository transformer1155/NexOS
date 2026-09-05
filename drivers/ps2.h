#ifndef NEXOS_PS2_H
#define NEXOS_PS2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2_init(void);
int ps2_has_scancode(void);
uint8_t ps2_read_scancode(void);

#ifdef __cplusplus
}
#endif

#endif // NEXOS_PS2_H
