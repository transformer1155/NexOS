; =====================================================================
;  minprobe.S  -  minimal ring-3 probe for bisecting the iret regression
; ---------------------------------------------------------------------
;  Deliberately does the LEAST possible work:
;     1. write one byte to fd 1 via SYS_WRITE (proves ring-3 executes)
;     2. SYS_EXIT                        (proves the unwind works)
;
;  No data reads, no VFS, no sandbox, no file I/O.  If this hangs, the
;  fault is in enter_user's iret itself.  If it prints and exits cleanly,
;  the loader/payload is at fault instead.
;
;  Built as a flat 32-bit blob loaded at USER_BASE.
; =====================================================================

BITS 32
section .text
global _start

%define SYS_EXIT   1
%define SYS_WRITE  4

_start:
        ; SYS_WRITE(fd=1, buf=msg, len=msg_len)
        mov     eax, SYS_WRITE
        mov     ebx, 1
        mov     ecx, msg
        mov     edx, msg_len
        int     0x80

        ; SYS_EXIT(0)
        mov     eax, SYS_EXIT
        xor     ebx, ebx
        int     0x80

        ; unreachable: spin so a stray fetch does not run off into garbage
        jmp     $

msg:    db      'MINPROBE_OK', 0x0A
msg_len equ $ - msg