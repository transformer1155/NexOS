s = open("stage2.asm", "r", encoding="latin-1").read()
uart = (
    "    ; --- UART init (COM1 @ 0x3F8): 115200 8N1, FIFO, DTR/RTS asserted ---\n"
    "    mov dx, 0x3F9\n"
    "    mov al, 0x00\n"
    "    out dx, al          ; IER = 0\n"
    "    mov dx, 0x3FB\n"
    "    mov al, 0x80\n"
    "    out dx, al          ; LCR: DLAB=1\n"
    "    mov dx, 0x3F8\n"
    "    mov al, 0x01\n"
    "    out dx, al          ; DLL = 1 (115200 baud)\n"
    "    mov dx, 0x3F9\n"
    "    mov al, 0x00\n"
    "    out dx, al          ; DLM = 0\n"
    "    mov dx, 0x3FB\n"
    "    mov al, 0x03\n"
    "    out dx, al          ; LCR: 8N1, DLAB=0 (0x3F8 now = RBR/THR)\n"
    "    mov dx, 0x3FA\n"
    "    mov al, 0xC7\n"
    "    out dx, al          ; FCR: FIFO enable\n"
    "    mov dx, 0x3FC\n"
    "    mov al, 0x0B\n"
    "    out dx, al          ; MCR: DTR/RTS/OUT2 (assert line for QEMU tcp)\n"
)
anchor = "    mov si, msg_hello\n"
assert anchor in s, "anchor not found"
s = s.replace(anchor, uart + anchor, 1)
open("_stage2.asm", "w", encoding="latin-1").write(s)
print("patched stage2")
