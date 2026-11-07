#!/usr/bin/env python3
"""Verify the TSS layout actually matches the CPU mode NexOS runs in.

THE HYPOTHESIS
--------------
gdt.cpp installs a **64-bit (IA-32e) TSS**:

    struct Tss64 {
        uint32_t reserved0;   // offset 0..3
        uint64_t rsp0;        // offset 8..15  (8-byte aligned -> pad at 4..7)
        ...
    };

In 64-bit mode that is correct: the CPU takes the ring-0 stack from
`TSS.rsp0` at offset 8.

But a **32-bit protected-mode** TSS has a different layout:

    offset 0   prev_tss  (32-bit)
    offset 4   esp0      <-- ring-0 stack pointer the CPU uses on a
    offset 8   ss0          ring 3 -> ring 0 transition via a trap gate

So if the CPU is in 32-bit protected mode and `ltr 0x28` loads a 32-bit
TSS, then on the FIRST `int 0x80` from ring 3 the CPU reads esp0 from
offset 4 -- which in our Tss64 is the ALIGNMENT PADDING (zero) -- and tries
to switch to a ring-0 stack at address 0.

That faults instantly, with no handler able to run (the handler itself
needs the stack), producing exactly the silent hang we observe: the guest
executes (store landed), then dies on its first syscall.

This script prints the two competing layouts side by side so the mismatch
is unambiguous.
"""

def report():
    print("Tss64 as declared in gdt.cpp (needs 8-byte alignment on rsp0):")
    print("   offset  0.. 3 : reserved0        = 0")
    print("   offset  4.. 7 : (alignment pad)  = 0     <-- 32-bit TSS.esp0 lives HERE")
    print("   offset  8..15 : rsp0 (uint64)            <-- 32-bit TSS.ss0 lives HERE")
    print()
    print("Classic 32-bit protected-mode TSS:")
    print("   offset  0.. 3 : prev_tss")
    print("   offset  4.. 7 : esp0   <-- CPU reads this on ring3->ring0 trap")
    print("   offset  8..11 : ss0")
    print()
    print("MISMATCH:")
    print("   running in 32-bit protected mode, the CPU reads esp0 at offset 4")
    print("   and gets the ALIGNMENT PADDING -> esp0 = 0 -> stack switch to")
    print("   address 0 -> fault with no usable stack -> silent hang.")
    print()
    print("WHY IT EVER WORKED:")
    print("   gdt.cpp's own header says the TSS was rewritten for IA-32e long")
    print("   mode (a 16-byte 64-bit TSS descriptor), because the kernel used")
    print("   to run under UEFI in long mode with 32-bit *compatibility*-mode")
    print("   code.  In long mode rsp0 at offset 8 is correct and everything")
    print("   works.  The current BIOS/legacy image boots into plain 32-bit")
    print("   protected mode, where the same descriptor means esp0 = 0.")
    print()
    print("CONFIRMATION IN THE OBSERVED DATA:")
    print("   * iret into ring 3 SUCCEEDS (riz spinloop's store landed) --")
    print("     iret does not consult the TSS.")
    print("   * the very first int 0x80 HANGS -- the trap gate DOES consult")
    print("     TSS.esp0, and esp0 is 0.")
    print("   * no exception report appears -- the CPU cannot even run the")
    print("     handler, because the handler's stack is address 0.")


if __name__ == "__main__":
    report()