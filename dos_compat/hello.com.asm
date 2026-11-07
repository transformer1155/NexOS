; =====================================================================
;  hello.com  -  NexOS DOS .COM demo (32-bit protected mode guest)
; ---------------------------------------------------------------------
;  IMPORTANT: NexOS enters ring-3 guests in 32-bit protected mode with a
;  flat GDT (all bases 0).  Real DOS ran .COM files in 16-bit real mode,
;  but there is no real-mode emulation here -- the CPU really is in
;  protected mode.  So this "DOS" program is assembled as bits 32.
;
;  Consequence: a literal port of a 16-bit .COM (e.g. `mov dx,msg` +
;  `int 21h`) gets decoded as garbage in 32-bit mode and hangs the
;  machine.  The INT 21h API is identical, but addressing must use
;  32-bit registers against flat linear addresses.
;
;  Addressing model
;  ----------------
;  The loader copies this image to LOADBASE (USER_BASE+0x100) and enters
;  with DS=ES=flat ring-3 data selector.  We assemble with
;  `org LOADBASE` so every label IS its final linear address -- no
;  segment arithmetic anywhere.
;
;  Build: nasm -f bin hello.com.asm -o hello.com
; =====================================================================

        bits 32
        org 0x04000100          ; == USER_BASE + 0x100 (loader destination)

start:
        ; --- AH=09h: print the banner ($-terminated) ---
        mov     edx, msg
        mov     ah, 0x09
        int     0x21

        ; --- AH=02h: print one char, proving the DL path ---
        mov     edx, '!'
        mov     ah, 0x02
        int     0x21

        ; CR/LF so the shell stays tidy
        mov     edx, 0x0D
        mov     ah, 0x02
        int     0x21
        mov     edx, 0x0A
        mov     ah, 0x02
        int     0x21

        ; --- AH=4Ch: terminate with code 0 ---
        mov     eax, 0x4C00
        int     0x21

        ; If AH=4Ch ever returned, RET lands on the loader's exit stub.
        ret

msg:    db      'Hello DOS (32-bit protected mode)', 0x0D, 0x0A, '$'