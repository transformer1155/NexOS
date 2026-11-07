#ifndef DOS_INT21_ISR_H
#define DOS_INT21_ISR_H

#include <stdint.h>

// INT 21h (and INT 20h) entry point installed in the IDT by kernel.cpp.
// Defined in dos_int21_isr.cpp; written in asm because it must preserve
// every guest register across the call.
extern "C" void isr_int21(void);
extern "C" void isr_int20(void);

// C-side dispatcher.  `regs` is the block saved by the asm stub's pushal.
extern "C" void dos_int21_dispatch(uint32_t* regs);

#endif // DOS_INT21_ISR_H