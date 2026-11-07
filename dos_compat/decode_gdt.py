#!/usr/bin/env python3
"""Decode the NexOS GDT descriptors relevant to the ring-3 iret.

gdt_flat(access) builds:  ((0xCF << 48) | (access << 40) | 0x0000FFFF)

  bits 40..47 = access byte
  bits 48..55 = flags nibble (0xCF), i.e. G=1, D/B=1, L=0, AVL=0

The question this answers: is the ring-3 CODE descriptor a valid target for
an `iret` that is dropping to CPL 3?
"""

def decode(name, access, flags=0xCF):
    P    = (access >> 7) & 1
    DPL  = (access >> 5) & 3
    S    = (access >> 4) & 1
    typ  = access & 0xF
    G    = (flags >> 7) & 1
    DB   = (flags >> 6) & 1
    L    = (flags >> 5) & 1
    print(f"{name:8} access=0x{access:02X} flags=0x{flags:02X} "
          f"P={P} DPL={DPL} S={S} type=0x{typ:X} | G={G} D/B={DB} L={L}")

decode("kcode", 0x9A)
decode("kdata", 0x92)
decode("ucode", 0xFA)
decode("udata", 0xF2)
print()
print("ucode: S=1, type=0xA (code, non-conforming, readable) -> OK")
print("       DPL=3, L=0, D/B=1 -> 32-bit compat-mode code       -> OK")
print()
print("TSS descriptor type byte used by gdt.cpp: 0x89")
print("  0x89 -> P=1, DPL=0, S=0, type=9 = available 64-bit TSS")
print("  In LONG MODE that is the correct form.")
print("  In PURE 32-BIT protected mode (no IA-32e), type 9 is")
print("  'available 32-bit TSS' -- also legal. So the TSS type alone")
print("  does not explain the hang; what matters is whether the CPU is")
print("  actually in long mode, because the 16-byte descriptor and the")
print("  iret semantics differ between the two.")