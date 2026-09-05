/* COM1 (0x3F8) polled serial for QEMU capture. */
#include "serial.h"

static inline void outb(unsigned short port, unsigned char v){
  __asm__ __volatile__("outb %0, %1" : : "a"(v), "d"(port));
}
static inline unsigned char inb(unsigned short port){
  unsigned char v;
  __asm__ __volatile__("inb %1, %0" : "=a"(v) : "d"(port));
  return v;
}

void serial_init(void){
  outb(0x3FB, 0x80);  /* DLAB on */
  outb(0x3F8, 0x01);  /* divisor low  (115200) */
  outb(0x3F9, 0x00);  /* divisor high */
  outb(0x3FB, 0x03);  /* 8 data bits, no parity, 1 stop */
  outb(0x3F9, 0x00);  /* disable interrupts */
}

void serial_putc(char c){
  while (!(inb(0x3F8 + 5) & 0x20)) { /* wait for THR empty */ }
  outb(0x3F8, (unsigned char)c);
}
