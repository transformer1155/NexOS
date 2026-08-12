#ifndef NexOS_GDT_H
#define NexOS_GDT_H

// Sets up the Global Descriptor Table with ring-0 (kernel) and ring-3 (user)
// code/data segments plus a TSS, and loads it. Required before any ring-3
// transition (Foundation 0 of the security model).
void gdt_init(void);

#endif // NexOS_GDT_H
