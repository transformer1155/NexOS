// cpu_host.c — probe host CPUID to compare with VBox detection
// Build: gcc -O0 cpu_host.c -o cpu_host.exe
#include <stdio.h>
#include <stdint.h>

static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

#define BIT(x) (1u << (x))

int main(void) {
    uint32_t a,b,c,d;

    printf("=== Leaf 0x00000000 (max standard) ===\n");
    cpuid(0,0,&a,&b,&c,&d);
    printf("  eax=0x%08x (max std leaf)\n", a);
    printf("  ebx=%c%c%c%c edx=%c%c%c%c ecx=%c%c%c%c\n",
           (b>>24)&0xff,(b>>16)&0xff,(b>>8)&0xff,b&0xff,
           (d>>24)&0xff,(d>>16)&0xff,(d>>8)&0xff,d&0xff,
           (c>>24)&0xff,(c>>16)&0xff,(c>>8)&0xff,c&0xff);

    printf("\n=== Leaf 0x80000000 (max ext) ===\n");
    cpuid(0x80000000,0,&a,&b,&c,&d);
    printf("  eax=0x%08x (max ext leaf)\n", a);

    printf("\n=== Leaf 0x80000001 (SVM/extensions) ===\n");
    cpuid(0x80000001,0,&a,&b,&c,&d);
    printf("  eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x\n", a,b,c,d);
    printf("  SVM (ECX bit 2): %s\n", (c & BIT(2)) ? "YES" : "no");
    printf("  SVM-Lock (ECX bit 3): %s\n", (c & BIT(3)) ? "YES" : "no");
    printf("  OSVW (ECX bit 12): %s\n", (c & BIT(12)) ? "YES" : "no");

    printf("\n=== Leaf 0x8000000A (SVM features) ===\n");
    cpuid(0x8000000A,0,&a,&b,&c,&d);
    printf("  eax=0x%08x (SVM version)\n", a);
    printf("  ebx=0x%08x (SVM features 1)\n", b);
    printf("    - NPT (bit 0): %s\n", (b & BIT(0)) ? "YES" : "no");
    printf("    - LbrVirt (bit 1): %s\n", (b & BIT(1)) ? "YES" : "no");
    printf("    - SVML (bit 2): %s\n", (b & BIT(2)) ? "YES" : "no");
    printf("    - NRIPS (bit 3): %s\n", (b & BIT(3)) ? "YES" : "no");
    printf("    - TscRateMsr (bit 4): %s\n", (b & BIT(4)) ? "YES" : "no");
    printf("    - VmcbClean (bit 5): %s\n", (b & BIT(5)) ? "YES" : "no");
    printf("    - PauseFilter (bit 6): %s\n", (b & BIT(6)) ? "YES" : "no");
    printf("    - TlbFlush (bit 7): %s\n", (b & BIT(7)) ? "YES" : "no");
    printf("    - AssistMask (bit 8): %s\n", (b & BIT(8)) ? "YES" : "no");
    printf("    - VSpecCtrl (bit 9): %s\n", (b & BIT(9)) ? "YES" : "no");
    printf("    - VirtVMCB (bit 10): %s\n", (b & BIT(10)) ? "YES" : "no");
    printf("    - VirtLoadStore (bit 11): %s\n", (b & BIT(11)) ? "YES" : "no");
    printf("  edx=0x%08x (SVM features 2)\n", d);
    printf("    - DecodeAssists (bit 0): %s\n", (d & BIT(0)) ? "YES" : "no");
    printf("    - PauseFilterThreshold (bit 1): %s\n", (d & BIT(1)) ? "YES" : "no");
    printf("    - IBPB (bit 12): %s\n", (d & BIT(12)) ? "YES" : "no");

    printf("\n=== Leaf 0x8000001E (SVM NPT / nested virt) ===\n");
    cpuid(0x8000001E,0,&a,&b,&c,&d);
    printf("  eax=0x%08x (NP levels)\n", a);
    printf("    = %u levels of nested page table\n", (a & 0xff));
    printf("  ebx=0x%08x (PA max / HAT)\n", b);
    printf("  ecx=0x%08x (Guest PAT / HAT)\n", c);
    printf("  edx=0x%08x (Invariant TSC / RDTSCP)\n", d);

    printf("\n=== Leaf 0x8000001F (AMD secure features / SEV) ===\n");
    cpuid(0x8000001F,0,&a,&b,&c,&d);
    printf("  eax=0x%08x ebx=0x%08x ecx=0x%08x edx=0x%08x\n", a,b,c,d);

    return 0;
}
