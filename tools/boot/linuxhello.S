# Minimal Linux i386 ELF used to prove the MiniOS Linux-compat shim.
# It does write(1, "LINUX_OK", 8) then exit(0) via int 0x80.
# Linked non-PIE at 0x01400000 (inside the 32 MiB identity map).
    .text
    .globl _start
_start:
    movl $4, %eax        # sys_write
    movl $1, %ebx        # fd = 1 (stdout)
    movl $msg, %ecx      # buffer
    movl $8, %edx        # length
    int  $0x80
    movl $1, %eax        # sys_exit
    xorl %ebx, %ebx      # code 0
    int  $0x80
msg:
    .ascii "LINUX_OK"
