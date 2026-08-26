BITS 32
global _start
extern kmain

section .text
_start:
  cli
  ; DEBUG breadcrumb: 'S' = kernel _start reached in protected mode.
  mov dx, 0x3FB
  mov al, 0x03
  out dx, al
  mov dx, 0x3FD
.w: in al, dx
  test al, 0x20
  jz .w
  mov al, 'S'
  mov dx, 0x3F8
  out dx, al

  ; We were entered from 16-bit real mode via a far jump with CS=0x08 (flat
  ; 4 GiB code descriptor). DS/ES/SS are still real-mode values (=0), which is
  ; the NULL descriptor in protected mode -> any data/stack access would GPF.
  ; Load the flat data selector (0x10) into every data segment BEFORE touching
  ; the stack.
  mov ax, 0x10
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov esp, stack_top
  mov ebp, esp
  call kmain
.halt:
  hlt
  jmp .halt

section .bss
align 16
stack_bottom:
  resb 16384
stack_top:
