#!/usr/bin/env python3
"""Show the RAM requirement implied by vmm_init's user-region mapping.

vmm_init() identity-maps these 4 MiB PSE pages:
    0x04000000 .. 0x08000000   ring-3 user region   (PG_USER)
    0x08000000 .. 0x10000000   linux/kexec band    (PG_USER, same loop)
    0x10000000 .. 0x18000000   supervisor heap     (PG_RW, no PG_USER)

Identity mapping means VA == PA, so every byte the kernel touches in those
bands must be REAL installed RAM.  `cmd_user` places the ring-3 stack top at
USER_END - 16 = 0x08000000 - 16, i.e. just under the 128 MiB line.
"""

USER_BASE = 0x04000000
USER_END  = 0x08000000

print("ring-3 stack top used by cmd_user / probe default:")
print(f"  0x{USER_END - 16:08X}  = {(USER_END - 16) / 1048576:.2f} MiB")
print()
print("physical RAM needed for that address to exist:")
print(f"  > {USER_END / 1048576:.0f} MiB   (the stack sits just below the line)")
print()
for mb in (128, 192, 256, 512):
    ram = mb * 1048576
    ok_exec = USER_BASE < ram
    ok_stk  = (USER_END - 16) < ram
    print(f"  -m {mb:3d}M : RAM top = 0x{ram:08X}   "
          f"image@USER_BASE {'OK ' if ok_exec else 'MISSING'}   "
          f"stack@0x{USER_END-16:08X} {'OK ' if ok_stk else 'MISSING'}")
print()
print("128 MiB is exactly the boundary: the stack slot at 0x07FFFFF0 is the")
print("LAST 16 bytes of RAM.  Writing the return address there is legal, but")
print("any push below it, or the very first pop, runs off the end of RAM")
print("into unmapped physical space -> the CPU cannot fetch/translate it")
print("and the machine stops with no fault report (nothing is mapped).")
print()
print("=> NexOS needs -m 256M (or more) for its own ring-3 stack choice to")
print("   be backed by memory.  The project test script still passes -m 128M,")
print("   which is stale relative to vmm_init.")