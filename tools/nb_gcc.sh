#!/bin/bash
gcc --version | head -1
cat > /tmp/nk.c <<'C'
extern "C" void __attribute__((naked)) nk_test(unsigned long* c){
    __asm__ __volatile__("movq %rsp, %rax\n ret\n");
    __builtin_unreachable();
}
C
g++ -m64 -ffreestanding -nostdlib -c /tmp/nk.c -o /tmp/nk.o 2>&1 | head -10 && echo "NAKED_64_OK" || echo "NAKED_64_FAIL"
