; =====================================================================
;  fileio.com  -  NexOS DOS file-API demo (32-bit protected mode guest)
; ---------------------------------------------------------------------
;  IMPORTANT: this must be assembled as bits 32.
;
;  NexOS enters ring-3 guests in 32-bit protected mode with a flat GDT
;  (all bases 0) and there is no real-mode emulation.  A 16-bit .COM
;  port is therefore decoded as garbage: `mov dx,msg` (BA imm16) becomes
;  `mov edx,<garbage32>`, and execution walks into invalid memory.
;  This file originally had exactly that bug -- it faulted at
;  EIP=USER_BASE+0x10C where `mov cx,0x0000` had been mis-decoded.
;
;  Addressing model: assemble with `org 0x04000100` (the loader's
;  destination) so every label IS its final linear address.  DS:DX then
;  points straight at the data -- no segment arithmetic anywhere.
;
;  Exercises the INT 21h file API end to end:
;     AH=3Ch create  -> lands in RAMFS (SFS stays read-only)
;     AH=40h write
;     AH=3Eh close
;     AH=3Dh open
;     AH=3Fh read
;
;  Build: nasm -f bin fileio.com.asm -o fileio.com
; =====================================================================

        bits 32
        org 0x04000100          ; == USER_BASE + 0x100

start:
        ; --- banner ---
        mov     edx, banner
        mov     ah, 0x09
        int     0x21

        ; --- AH=3Ch create DOSFILE.TXT (DS:DX=name, CX=attr) ---
        mov     edx, fname
        xor     ecx, ecx                ; attribute: normal
        mov     ah, 0x3C
        int     0x21
        jc      create_failed
        mov     [handle], ax            ; AX = handle on success

        mov     edx, created
        mov     ah, 0x09
        int     0x21

        ; --- AH=40h write payload (BX=handle, DS:DX=buf, CX=count) ---
        movzx   ebx, word [handle]
        mov     edx, payload
        mov     ecx, payload_len
        mov     ah, 0x40
        int     0x21
        jc      write_failed

        ; --- AH=3Eh close (BX=handle) ---
        movzx   ebx, word [handle]
        mov     ah, 0x3E
        int     0x21

        mov     edx, wrote
        mov     ah, 0x09
        int     0x21

        ; --- AH=3Dh reopen read-only (DS:DX=name, AL=0) ---
        mov     edx, fname
        mov     eax, 0x3D00             ; AH=3Dh, AL=0 (read only)
        int     0x21
        jc      open_failed
        mov     [handle], ax

        ; --- AH=3Fh read it back (BX=handle, DS:DX=buf, CX=count) ---
        movzx   ebx, word [handle]
        mov     edx, readbuf
        mov     ecx, 64
        mov     ah, 0x3F
        int     0x21
        jc      read_failed
        mov     [nread], ax

        ; terminate the buffer with '$' so AH=09h can print it
        movzx   ebx, word [nread]
        mov     byte [readbuf + ebx], '$'

        ; --- AH=3Eh close again ---
        movzx   ebx, word [handle]
        mov     ah, 0x3E
        int     0x21

        mov     edx, readback
        mov     ah, 0x09
        int     0x21

        mov     edx, readbuf
        mov     ah, 0x09
        int     0x21

        mov     edx, crlf
        mov     ah, 0x09
        int     0x21

        ; --- exit 0 ---
        mov     eax, 0x4C00
        int     0x21
        ret

; ---- error paths -------------------------------------------------
create_failed:
        mov     edx, e_create
        jmp     fail_common
write_failed:
        mov     edx, e_write
        jmp     fail_common
open_failed:
        mov     edx, e_open
        jmp     fail_common
read_failed:
        mov     edx, e_read
fail_common:
        mov     ah, 0x09
        int     0x21
        mov     eax, 0x4C01             ; exit code 1 marks failure
        int     0x21
        ret

; ---- data --------------------------------------------------------
banner:   db      'FILEIO.COM: exercising INT 21h file API', 0x0D, 0x0A, '$'
created:  db      '  create DOSFILE.TXT  ok', 0x0D, 0x0A, '$'
wrote:    db      '  write + close       ok', 0x0D, 0x0A, '$'
readback: db      '  read back: "', '$'
crlf:     db      '"', 0x0D, 0x0A, 'FILEIO.COM: done', 0x0D, 0x0A, '$'

e_create: db      '  create FAILED', 0x0D, 0x0A, '$'
e_write:  db      '  write FAILED', 0x0D, 0x0A, '$'
e_open:   db      '  reopen FAILED', 0x0D, 0x0A, '$'
e_read:   db      '  read FAILED', 0x0D, 0x0A, '$'

fname:    db      'DOSFILE.TXT', 0
payload:  db      'ramfs-overlay-works'
payload_len equ $ - payload

handle:   dw      0
nread:    dw      0
readbuf:  times 80 db 0