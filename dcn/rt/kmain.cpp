/* QEMU entry shim: init serial, run the test main(), then halt the VM. */
extern "C" void serial_init(void);
extern "C" int  main(void);

extern "C" void kmain(void){
  serial_init();
  int rc = main();
  (void)rc;
  /* brief delay so QEMU serial chardev drains before we exit */
  volatile unsigned long i;
  for (i = 0; i < 2000000UL; i++) { }
  /* isa-debug-exit (iobase 0xF4) -> clean QEMU shutdown + serial flush */
  __asm__ __volatile__("outb %0, %1" : : "a"((unsigned char)0), "d"((unsigned short)0xF4));
  for (;;) __asm__ __volatile__("hlt");
}
