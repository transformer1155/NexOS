; =====================================================================
;  menu32.asm  -  NexOS Win32 popup-menu subsystem test
; ---------------------------------------------------------------------
;  Exercises the user32 menu API bridge:
;    CreatePopupMenu / AppendMenuA / GetMenuItemCount / TrackPopupMenu
;  TrackPopupMenu() registers an async session rendered by gui.cpp;
;  clicking an item must deliver WM_COMMAND(id) to our WndProc, which
;  reports it through OutputDebugStringA (visible on the serial log).
;  Packed into a PE32 by tools/make_test_exe.py (imports from imports.inc).
; =====================================================================
BITS 32
%include "imports.inc"              ; generated: IMAGEBASE + IMP_* thunk VAs
ORG IMAGEBASE + 0x1000

; ---- Win32 constants ----
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define SW_SHOW             5
%define WM_DESTROY          0x0002
%define WM_PAINT            0x000F
%define WM_COMMAND          0x0111
%define MF_STRING           0x0000
%define MF_SEPARATOR        0x0800

; =====================================================================
;  Entry point (called by the loader as stdcall, no arguments)
; =====================================================================
_start:
    push  ebp
    mov   ebp, esp

    ; ---- banner ----
    push  szBanner
    call  [IMP_puts]
    add   esp, 4                      ; cdecl

    ; ---- hInstance = GetModuleHandleA(NULL) ----
    push  dword 0
    call  [IMP_GetModuleHandleA]
    mov   [hInst], eax

    ; ---- WNDCLASSA ----
    mov   dword [wc + 0],  0x0003     ; CS_HREDRAW | CS_VREDRAW
    mov   dword [wc + 4],  WndProc
    mov   dword [wc + 8],  0
    mov   dword [wc + 12], 0
    mov   eax, [hInst]
    mov   [wc + 16], eax
    mov   dword [wc + 20], 0          ; hIcon
    mov   dword [wc + 24], 0          ; hCursor
    mov   dword [wc + 28], 0          ; hbrBackground
    mov   dword [wc + 32], 0          ; lpszMenuName
    mov   dword [wc + 36], szClass    ; lpszClassName

    push  wc
    call  [IMP_RegisterClassA]
    test  eax, eax
    jz    .fail

    ; ---- CreateWindowExA(0, szClass, szTitle, WS_OVERLAPPEDWINDOW,
    ;                      900, 400, 360, 260, NULL, NULL, hInst, NULL)
    ; Window parked bottom-right so the desktop icon column (x 22..114)
    ; stays clickable in the tests.
    push  dword 0                     ; lpParam
    push  dword [hInst]               ; hInstance
    push  dword 0                     ; hMenu
    push  dword 0                     ; hWndParent
    push  dword 260                   ; nHeight
    push  dword 360                   ; nWidth
    push  dword 400                   ; y
    push  dword 900                   ; x
    push  dword WS_OVERLAPPEDWINDOW   ; dwStyle
    push  szTitle                     ; lpWindowName
    push  szClass                     ; lpClassName
    push  dword 0                     ; dwExStyle
    call  [IMP_CreateWindowExA]
    mov   [hWnd], eax
    test  eax, eax
    jz    .fail

    ; ---- build a popup menu ----
    call  [IMP_CreatePopupMenu]
    mov   [hMenu], eax
    test  eax, eax
    jz    .fail

    push  szItem1                     ; "Open"
    push  dword 1001
    push  dword MF_STRING
    push  dword [hMenu]
    call  [IMP_AppendMenuA]
    test  eax, eax
    jz    .fail

    push  szItem2                     ; "Edit"
    push  dword 1002
    push  dword MF_STRING
    push  dword [hMenu]
    call  [IMP_AppendMenuA]
    test  eax, eax
    jz    .fail

    push  dword 0                     ; separator
    push  dword 0
    push  dword MF_SEPARATOR
    push  dword [hMenu]
    call  [IMP_AppendMenuA]
    test  eax, eax
    jz    .fail

    push  szItem3                     ; "Delete"
    push  dword 1003
    push  dword MF_STRING
    push  dword [hMenu]
    call  [IMP_AppendMenuA]
    test  eax, eax
    jz    .fail

    ; ---- item count check ----
    push  dword [hMenu]
    call  [IMP_GetMenuItemCount]
    mov   [menuCount], eax

    ; ---- TrackPopupMenu(hMenu, 0, 300, 200, 0, hWnd, 0) ----
    push  dword 0
    push  dword [hWnd]
    push  dword 0
    push  dword 200
    push  dword 300
    push  dword 0
    push  dword [hMenu]
    call  [IMP_TrackPopupMenu]
    mov   [tracked], eax

    ; ---- report ----
    push  szTracked
    call  [IMP_puts]
    add   esp, 4
    push  dword [hWnd]
    call  [IMP_ShowWindow]
    push  dword [hWnd]
    call  [IMP_UpdateWindow]

    ; ---- message loop ----
.msgloop:
    push  dword 0
    push  dword 0
    push  dword 0
    push  msg
    call  [IMP_GetMessageA]
    test  eax, eax
    jz    .done
    push  msg
    call  [IMP_TranslateMessage]
    push  msg
    call  [IMP_DispatchMessageA]
    jmp   .msgloop

.done:
    mov   eax, 0
    mov   esp, ebp
    pop   ebp
    ret

.fail:
    push  szFail
    call  [IMP_puts]
    add   esp, 4
    mov   eax, 1
    mov   esp, ebp
    pop   ebp
    ret

; =====================================================================
;  WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)  -  stdcall
; =====================================================================
WndProc:
    push  ebp
    mov   ebp, esp
    push  ebx
    push  esi
    push  edi

    mov   eax, [ebp + 12]             ; msg

    cmp   eax, WM_PAINT
    je    .paint
    cmp   eax, WM_COMMAND
    je    .command
    cmp   eax, WM_DESTROY
    je    .destroy
    jmp   .defproc

.command:
    ; wp = menu command id.  Report it (fixed strings - the itoa helper
    ; has a relocation quirk under this PE packer, so avoid it here).
    cmp   dword [ebp + 16], 1002
    jne   .cmd_other
    push  szCmd1002
    call  [IMP_puts]
    add   esp, 4
    xor   eax, eax
    jmp   .ret
.cmd_other:
    push  szCmdOther
    call  [IMP_puts]
    add   esp, 4
    xor   eax, eax
    jmp   .ret

.destroy:
    push  dword 0
    call  [IMP_PostQuitMessage]
    xor   eax, eax
    jmp   .ret

.paint:
    ; Nothing to draw: the NexOS compositor fills the client area.
    ; (Do NOT call UpdateWindow here - that recurses forever.)
    xor   eax, eax
    jmp   .ret

.defproc:
    push  dword [ebp + 20]
    push  dword [ebp + 16]
    push  dword [ebp + 12]
    push  dword [ebp + 8]
    call  [IMP_DefWindowProcA]

.ret:
    pop   edi
    pop   esi
    pop   ebx
    mov   esp, ebp
    pop   ebp
    ret   16                          ; stdcall, 4 arguments

; =====================================================================
;  itoa_edi : EAX = value (unsigned), EDI = destination buffer
; =====================================================================
itoa_edi:
    push  ebx
    mov   ebx, edi
    mov   ecx, 0
    mov   esi, 10
.conv:
    xor   edx, edx
    div   esi
    add   dl, '0'
    push  edx
    inc   ecx
    test  eax, eax
    jnz   .conv
.emit:
    pop   edx
    mov   [ebx], dl
    inc   ebx
    dec   ecx
    jnz   .emit
    mov   byte [ebx], 0
    pop   ebx
    ret

; =====================================================================
;  Data
; =====================================================================
align 4
hInst        dd 0
hWnd         dd 0
hMenu        dd 0
menuCount    dd 0
tracked      dd 0

align 4
wc           times 40 db 0            ; WNDCLASSA
msg          times 28 db 0            ; MSG
numBuf       times 16 db 0

szClass      db 'NexOSMenuTest', 0
szTitle      db 'Menu API test', 0
szItem1      db 'Open', 0
szItem2      db 'Edit', 0
szItem3      db 'Delete', 0
szBanner     db '[menu32] entry reached', 0
szTracked    db '[menu32] tracked popup menu (TrackPopupMenu)', 0
szCmd1002    db '[menu32] WM_COMMAND id=1002', 0
szCmdOther   db '[menu32] WM_COMMAND other', 0
szFail       db '[menu32] FAILED', 0

TEXT_END:
