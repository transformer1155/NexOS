; =====================================================================
;  userdemo.S  -  ring-3 sandbox test payload (Foundation 0)
; ---------------------------------------------------------------------
;  Runs in ring 3 with root_path = /apps/userdemo and uid 1000.
;  It proves four things over the int 0x80 ABI:
;    1. ring-3 code executes at all                       -> RING3_OK
;    2. syscalls return a real value in eax (fd)          -> open succeeds
;    3. a file INSIDE the sandbox can be read             -> SANDBOX_FILE_OK
;    4. absolute + ".." escapes out of the sandbox fail   -> DENY/ESCAPE ok
;  If the sandbox ever breaks, step 3/4 print SANDBOX_LEAK and the
;  headless test fails.
;
;  nasm syntax (the Makefile's AS is nasm, not gas).
;  Everything lives in .text so `objcopy -O binary` yields one flat blob.
; =====================================================================

BITS 32
section .text
global _start

; --- syscall numbers (Linux i386) ---
%define SYS_EXIT   1
%define SYS_READ   3
%define SYS_WRITE  4
%define SYS_OPEN   5
%define SYS_CLOSE  6

; puts(edx=len, ecx=ptr) helper via macro to keep the flow readable
%macro SAY 2
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, %1
    mov edx, %2
    int 0x80
%endmacro

_start:
    SAY msg_ring3, msg_ring3_len

; ---------------------------------------------------------------------
; 1. open a file INSIDE our sandbox, using a relative path.
;    "appdata.txt" -> /apps/userdemo/appdata.txt  -> GRANTED
; ---------------------------------------------------------------------
    mov eax, SYS_OPEN
    mov ebx, path_in
    xor ecx, ecx                 ; O_RDONLY
    int 0x80
    cmp eax, 0
    jl  .open_failed
    mov edi, eax                 ; keep the fd

    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, filebuf
    mov edx, 64
    int 0x80
    cmp eax, 0
    jle .read_failed

    mov edx, eax                 ; echo exactly what we read
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, filebuf
    int 0x80

    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    jmp .escape_abs

.open_failed:
    SAY msg_openfail, msg_openfail_len
    jmp .escape_abs

.read_failed:
    SAY msg_readfail, msg_readfail_len

; ---------------------------------------------------------------------
; 2. try an ABSOLUTE path outside the sandbox: /system/passwd
; ---------------------------------------------------------------------
.escape_abs:
    mov eax, SYS_OPEN
    mov ebx, path_abs
    xor ecx, ecx
    int 0x80
    cmp eax, 0
    jl  .abs_denied
    SAY msg_leak, msg_leak_len   ; got a fd -> sandbox is broken
    jmp .escape_dots
.abs_denied:
    SAY msg_deny_abs, msg_deny_abs_len

; ---------------------------------------------------------------------
; 3. try to climb out with ".." : ../../system/passwd
; ---------------------------------------------------------------------
.escape_dots:
    mov eax, SYS_OPEN
    mov ebx, path_dots
    xor ecx, ecx
    int 0x80
    cmp eax, 0
    jl  .dots_denied
    SAY msg_leak, msg_leak_len
    jmp .ask_deny
.dots_denied:
    SAY msg_deny_dots, msg_deny_dots_len

; =====================================================================
;  Security doc 3.3 - Y/N consent prompt
; ---------------------------------------------------------------------
;  /home/user/notes.txt is outside our sandbox but it is USER DATA, not
;  a credential store, so the kernel must ASK instead of hard-denying.
;  The headless test drives the answers with real keystrokes:
;    4. first  attempt -> user presses N  -> denied  -> PERM_DENY_OK
;    5. second attempt -> user presses A  -> allowed -> USERDOC_CONTENT
;    6. third  attempt -> cached, NO prompt at all   -> PERM_CACHED_OK
; =====================================================================

; --- 4. expect a prompt, answered N ----------------------------------
.ask_deny:
    mov eax, SYS_OPEN
    mov ebx, path_doc
    xor ecx, ecx
    int 0x80
    cmp eax, 0
    jl  .ask_deny_ok
    mov edi, eax                     ; got a fd after the user said no
    SAY msg_leak, msg_leak_len
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    jmp .ask_allow
.ask_deny_ok:
    SAY msg_perm_deny, msg_perm_deny_len

; --- 5. expect a prompt, answered A (allow + remember) ---------------
.ask_allow:
    mov eax, SYS_OPEN
    mov ebx, path_doc
    xor ecx, ecx
    int 0x80
    cmp eax, 0
    jl  .ask_allow_failed
    mov edi, eax

    mov eax, SYS_READ
    mov ebx, edi
    mov ecx, filebuf
    mov edx, 64
    int 0x80
    cmp eax, 0
    jle .ask_allow_failed
    mov edx, eax
    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, filebuf
    int 0x80

    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    SAY msg_perm_allow, msg_perm_allow_len
    jmp .ask_cached
.ask_allow_failed:
    SAY msg_perm_afail, msg_perm_afail_len

; --- 6. must hit the remembered grant, with no prompt ----------------
.ask_cached:
    mov eax, SYS_OPEN
    mov ebx, path_doc
    xor ecx, ecx
    int 0x80
    cmp eax, 0
    jl  .cached_failed
    mov edi, eax
    mov eax, SYS_CLOSE
    mov ebx, edi
    int 0x80
    SAY msg_perm_cached, msg_perm_cached_len
    jmp .done
.cached_failed:
    SAY msg_perm_cfail, msg_perm_cfail_len

; ---------------------------------------------------------------------
.done:
    mov eax, SYS_EXIT
    xor ebx, ebx
    int 0x80
    hlt                          ; never reached

; ---------------------------------------------------------------------
;  data (kept in .text so the flat binary stays contiguous)
; ---------------------------------------------------------------------
msg_ring3:      db "RING3_OK", 0x0A
msg_ring3_len   equ $ - msg_ring3

msg_openfail:   db "SANDBOX_OPEN_FAIL", 0x0A
msg_openfail_len equ $ - msg_openfail

msg_readfail:   db "SANDBOX_READ_FAIL", 0x0A
msg_readfail_len equ $ - msg_readfail

msg_leak:       db "SANDBOX_LEAK", 0x0A
msg_leak_len    equ $ - msg_leak

msg_deny_abs:   db "DENY_ABS_OK", 0x0A
msg_deny_abs_len equ $ - msg_deny_abs

msg_deny_dots:  db "DENY_DOTDOT_OK", 0x0A
msg_deny_dots_len equ $ - msg_deny_dots

msg_perm_deny:  db "PERM_DENY_OK", 0x0A
msg_perm_deny_len equ $ - msg_perm_deny

msg_perm_allow: db "PERM_ALLOW_OK", 0x0A
msg_perm_allow_len equ $ - msg_perm_allow

msg_perm_afail: db "PERM_ALLOW_FAIL", 0x0A
msg_perm_afail_len equ $ - msg_perm_afail

msg_perm_cached: db "PERM_CACHED_OK", 0x0A
msg_perm_cached_len equ $ - msg_perm_cached

msg_perm_cfail: db "PERM_CACHED_FAIL", 0x0A
msg_perm_cfail_len equ $ - msg_perm_cfail

path_in:        db "appdata.txt", 0
path_abs:       db "/system/passwd", 0
path_dots:      db "../../system/passwd", 0
path_doc:       db "/home/user/notes.txt", 0

filebuf:        times 64 db 0
