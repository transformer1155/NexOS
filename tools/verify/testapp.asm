; =====================================================================
;  testapp.asm  -  NexOS Win32 subsystem demo application
; ---------------------------------------------------------------------
;  A real 32-bit Windows GUI program written in raw NASM.  It is packed
;  into a genuine PE32 (i386) executable by tools/make_test_exe.py, which
;  also generates imports.inc (the IAT addresses) and the .reloc section.
;
;  What it exercises in the NexOS Win32 subsystem:
;    kernel32 : GetModuleHandleA, GetEnvironmentVariableA, GetComputerNameA,
;               GetSystemDirectoryA, GetWindowsDirectoryA, GetTickCount
;    advapi32 : RegOpenKeyExA, RegQueryValueExA, RegCloseKey
;    user32   : RegisterClassA, CreateWindowExA (top-level + BUTTON child),
;               ShowWindow, UpdateWindow, GetMessageA, TranslateMessage,
;               DispatchMessageA, DefWindowProcA, PostQuitMessage,
;               BeginPaint, EndPaint, FillRect
;    gdi32    : CreateSolidBrush, SetTextColor, SetBkMode, TextOutA,
;               Rectangle, Ellipse, MoveToEx, LineTo, DeleteObject
;    msvcrt   : puts   (cdecl - caller cleans the stack)
; =====================================================================
BITS 32
%include "imports.inc"              ; generated: IMAGEBASE + IMP_* thunk VAs
ORG IMAGEBASE + 0x1000

; ---- Win32 constants ----
%define WS_OVERLAPPEDWINDOW 0x00CF0000
%define WS_CHILD            0x40000000
%define WS_VISIBLE          0x10000000
%define SW_SHOW             5
%define WM_DESTROY          0x0002
%define WM_PAINT            0x000F
%define WM_CHAR             0x0102
%define WM_LBUTTONDOWN      0x0201
%define HKEY_LOCAL_MACHINE  0x80000002
%define HKEY_CURRENT_USER   0x80000001
%define KEY_READ            0x00020019
%define TRANSPARENT         1

; =====================================================================
;  Entry point (called by the loader as stdcall, no arguments)
; =====================================================================
_start:
    push  ebp
    mov   ebp, esp

    ; ---- puts("...") : prove console output works ----
    push  szBanner
    call  [IMP_puts]
    add   esp, 4                      ; cdecl

    ; ---- hInstance = GetModuleHandleA(NULL) ----
    push  dword 0
    call  [IMP_GetModuleHandleA]
    mov   [hInst], eax

    ; ---- read HKLM\...\CurrentVersion\ProductName from the registry ----
    push  hKey
    push  dword KEY_READ
    push  dword 0
    push  szRegPath
    push  dword HKEY_LOCAL_MACHINE
    call  [IMP_RegOpenKeyExA]
    test  eax, eax
    jnz   .no_registry
    mov   dword [cbProduct], 63
    push  cbProduct
    push  bufProduct
    push  dword 0
    push  dword 0
    push  szProductName
    push  dword [hKey]
    call  [IMP_RegQueryValueExA]
    push  dword [hKey]
    call  [IMP_RegCloseKey]
.no_registry:

    ; ---- USERNAME from the simulated environment ----
    push  dword 63
    push  bufUser
    push  szEnvUser
    call  [IMP_GetEnvironmentVariableA]

    ; ---- COMPUTERNAME ----
    mov   dword [cbComputer], 63
    push  cbComputer
    push  bufComputer
    call  [IMP_GetComputerNameA]

    ; ---- SYSTEM / WINDOWS directories ----
    push  dword 63
    push  bufSysDir
    call  [IMP_GetSystemDirectoryA]
    push  dword 63
    push  bufWinDir
    call  [IMP_GetWindowsDirectoryA]

    ; ---- TICK COUNT ----
    call  [IMP_GetTickCount]
    mov   edi, bufTick
    call  itoa_edi

    ; ---- HKCU Shell Folders\Desktop ----
    push  hKey2
    push  dword KEY_READ
    push  dword 0
    push  szRegPath2
    push  dword HKEY_CURRENT_USER
    call  [IMP_RegOpenKeyExA]
    test  eax, eax
    jnz   .no_registry2
    mov   dword [cbDesktop], 63
    push  cbDesktop
    push  bufDesktop
    push  dword 0
    push  dword 0
    push  szDesktop
    push  dword [hKey2]
    call  [IMP_RegQueryValueExA]
    push  dword [hKey2]
    call  [IMP_RegCloseKey]
.no_registry2:

    ; ---- background brush ----
    push  dword 0x00F7F3F0            ; COLORREF 0x00BBGGRR -> light grey-blue
    call  [IMP_CreateSolidBrush]
    mov   [hbrBack], eax

    ; ---- WNDCLASSA ----
    mov   dword [wc + 0],  0x0003     ; CS_HREDRAW | CS_VREDRAW
    mov   dword [wc + 4],  WndProc
    mov   dword [wc + 8],  0
    mov   dword [wc + 12], 0
    mov   eax, [hInst]
    mov   [wc + 16], eax
    mov   dword [wc + 20], 0          ; hIcon
    mov   dword [wc + 24], 0          ; hCursor
    mov   eax, [hbrBack]
    mov   [wc + 28], eax              ; hbrBackground
    mov   dword [wc + 32], 0          ; lpszMenuName
    mov   dword [wc + 36], szClass    ; lpszClassName

    push  wc
    call  [IMP_RegisterClassA]
    test  eax, eax
    jz    .fail

    ; ---- CreateWindowExA(0, szClass, szTitle, WS_OVERLAPPEDWINDOW,
    ;                      40, 40, 460, 380, NULL, NULL, hInst, NULL) ----
    push  dword 0                     ; lpParam
    push  dword [hInst]               ; hInstance
    push  dword 0                     ; hMenu
    push  dword 0                     ; hWndParent
    push  dword 380                   ; nHeight
    push  dword 460                   ; nWidth
    push  dword 40                    ; y
    push  dword 40                    ; x
    push  dword WS_OVERLAPPEDWINDOW   ; dwStyle
    push  szTitle                     ; lpWindowName
    push  szClass                     ; lpClassName
    push  dword 0                     ; dwExStyle
    call  [IMP_CreateWindowExA]
    mov   [hWnd], eax
    test  eax, eax
    jz    .fail

    ; ---- child BUTTON control ----
    push  dword 0
    push  dword [hInst]
    push  dword 100                   ; control id
    push  dword [hWnd]                ; parent
    push  dword 28                    ; height
    push  dword 110                   ; width
    push  dword 332                   ; y
    push  dword 320                   ; x
    push  dword (WS_CHILD | WS_VISIBLE)
    push  szBtnText
    push  szBtnClass
    push  dword 0
    call  [IMP_CreateWindowExA]

    ; ---- ShowWindow + UpdateWindow ----
    push  dword SW_SHOW
    push  dword [hWnd]
    call  [IMP_ShowWindow]

    push  dword [hWnd]
    call  [IMP_UpdateWindow]

    push  szReady
    call  [IMP_puts]
    add   esp, 4

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
    sub   esp, 8
    push  ebx
    push  esi
    push  edi

    mov   eax, [ebp + 12]             ; msg

    cmp   eax, WM_PAINT
    je    .paint
    cmp   eax, WM_DESTROY
    je    .destroy
    cmp   eax, WM_LBUTTONDOWN
    je    .lbutton
    cmp   eax, WM_CHAR
    je    .charmsg
    jmp   .defproc

; ---------------------------------------------------------------------
.lbutton:
    inc   dword [nClicks]
    xor   eax, eax
    jmp   .ret

.charmsg:
    mov   eax, [ebp + 16]             ; wParam = character
    mov   [lastChar], al
    inc   dword [nKeys]
    xor   eax, eax
    jmp   .ret

.destroy:
    push  dword 0
    call  [IMP_PostQuitMessage]
    xor   eax, eax
    jmp   .ret

; ---------------------------------------------------------------------
.paint:
    inc   dword [nPaints]

    push  ps
    push  dword [ebp + 8]             ; hwnd
    call  [IMP_BeginPaint]
    mov   ebx, eax                    ; ebx = hdc

    ; ---- background ----
    mov   dword [rc + 0],  0
    mov   dword [rc + 4],  0
    mov   dword [rc + 8],  460
    mov   dword [rc + 12], 380
    push  dword [hbrBack]
    push  rc
    push  ebx
    call  [IMP_FillRect]

    ; ---- title bar strip ----
    push  dword 0x00A05A28            ; BGR -> steel blue
    call  [IMP_CreateSolidBrush]
    mov   esi, eax
    mov   dword [rc + 0],  0
    mov   dword [rc + 4],  0
    mov   dword [rc + 8],  460
    mov   dword [rc + 12], 34
    push  esi
    push  rc
    push  ebx
    call  [IMP_FillRect]
    push  esi
    call  [IMP_DeleteObject]

    ; ---- transparent text mode ----
    push  dword TRANSPARENT
    push  ebx
    call  [IMP_SetBkMode]

    ; heading (white on the blue strip)
    push  dword 0x00FFFFFF
    push  ebx
    call  [IMP_SetTextColor]
    push  dword -1
    push  szHeading
    push  dword 9
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]

    ; body text (dark)
    push  dword 0x00201810
    push  ebx
    call  [IMP_SetTextColor]

    push  dword -1
    push  szLine1
    push  dword 50
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  bufProduct
    push  dword 70
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  szLine2
    push  dword 94
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  bufUser
    push  dword 114
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  szLine3
    push  dword 138
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  bufComputer
    push  dword 158
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]

    ; ---- right column: directories / tick count ----
    push  dword -1
    push  szLine4
    push  dword 50
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  bufSysDir
    push  dword 70
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  szLine5
    push  dword 94
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  bufWinDir
    push  dword 114
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  szLine6
    push  dword 138
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  bufDesktop
    push  dword 158
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  szLine7
    push  dword 182
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    push  dword -1
    push  bufTick
    push  dword 202
    push  dword 240
    push  ebx
    call  [IMP_TextOutA]

    ; ---- GDI shapes ----
    push  dword 0x0030B0F0            ; orange-ish pen/brush (BGR)
    call  [IMP_CreateSolidBrush]
    mov   edi, eax
    push  edi
    push  ebx
    call  [IMP_SelectObject]

    push  dword 260                   ; bottom
    push  dword 288                   ; right
    push  dword 230                   ; top
    push  dword 240                   ; left
    push  ebx
    call  [IMP_Rectangle]

    push  dword 260
    push  dword 400
    push  dword 230
    push  dword 320
    push  ebx
    call  [IMP_Ellipse]

    push  edi
    call  [IMP_DeleteObject]

    ; separator line
    push  dword 0
    push  dword 282
    push  dword 16
    push  ebx
    call  [IMP_MoveToEx]
    push  dword 282
    push  dword 444
    push  ebx
    call  [IMP_LineTo]

    ; ---- interaction counters ----
    push  dword 0x00703010
    push  ebx
    call  [IMP_SetTextColor]

    ; "paints:" + number
    mov   eax, [nPaints]
    mov   edi, numBuf1
    call  itoa_edi
    push  dword -1
    push  szPaints
    push  dword 300
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]
    push  dword -1
    push  numBuf1
    push  dword 300
    push  dword 96
    push  ebx
    call  [IMP_TextOutA]

    ; "clicks:" + number
    mov   eax, [nClicks]
    mov   edi, numBuf2
    call  itoa_edi
    push  dword -1
    push  szClicks
    push  dword 318
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]
    push  dword -1
    push  numBuf2
    push  dword 318
    push  dword 96
    push  ebx
    call  [IMP_TextOutA]

    ; "keys:" + number
    mov   eax, [nKeys]
    mov   edi, numBuf3
    call  itoa_edi
    push  dword -1
    push  szKeys
    push  dword 336
    push  dword 16
    push  ebx
    call  [IMP_TextOutA]
    push  dword -1
    push  numBuf3
    push  dword 336
    push  dword 96
    push  ebx
    call  [IMP_TextOutA]

    push  ps
    push  dword [ebp + 8]
    call  [IMP_EndPaint]

    xor   eax, eax
    jmp   .ret

; ---------------------------------------------------------------------
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
;  Writes a NUL terminated decimal string.  Clobbers eax,ecx,edx,esi.
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
;  Data (the section is mapped read/write/execute)
; =====================================================================
align 4
hInst        dd 0
hWnd         dd 0
hbrBack      dd 0
hKey         dd 0
hKey2        dd 0
cbProduct    dd 0
cbComputer   dd 0
cbDesktop    dd 0
nPaints      dd 0
nClicks      dd 0
nKeys        dd 0
lastChar     db 0
             db 0, 0, 0

align 4
wc           times 40 db 0            ; WNDCLASSA
msg          times 28 db 0            ; MSG
ps           times 64 db 0            ; PAINTSTRUCT
rc           times 16 db 0            ; RECT

bufProduct   times 64 db 0
bufUser      times 64 db 0
bufComputer  times 64 db 0
bufSysDir    times 64 db 0
bufWinDir    times 64 db 0
bufDesktop   times 64 db 0
bufTick      times 16 db 0
numBuf1      times 16 db 0
numBuf2      times 16 db 0
numBuf3      times 16 db 0

szClass      db 'NexOSWin32Demo', 0
szTitle      db 'NexOS Win32 Demo - hello32.exe', 0
szBtnClass   db 'BUTTON', 0
szBtnText    db 'Click Me', 0
szHeading    db 'Native PE32 running on NexOS', 0
szLine1      db 'Registry ProductName:', 0
szLine2      db 'Environment USERNAME:', 0
szLine3      db 'Computer name:', 0
szLine4      db 'System directory:', 0
szLine5      db 'Windows directory:', 0
szLine6      db 'Desktop folder:', 0
szLine7      db 'Tick count:', 0
szPaints     db 'WM_PAINT  :', 0
szClicks     db 'WM_LBUTTON:', 0
szKeys       db 'WM_CHAR   :', 0
szRegPath    db 'SOFTWARE\Microsoft\Windows NT\CurrentVersion', 0
szRegPath2   db 'Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders', 0
szProductName db 'ProductName', 0
szDesktop    db 'Desktop', 0
szEnvUser    db 'USERNAME', 0
szBanner     db '[hello32] PE32 entry reached - Win32 subsystem alive.', 0
szReady      db '[hello32] window created, entering message loop.', 0
szFail       db '[hello32] ERROR: RegisterClassA/CreateWindowExA failed.', 0

TEXT_END:
