import struct

# UART init machine code (16-bit real mode), 28 bytes.
# Asserts MCR OUT2/DTR/RTS + FIFO + 8N1, DLAB=0 so 0x3F8 = RBR/THR.
uart = bytes.fromhex(
    "BAF903" "B000" "EE"          # mov dx,0x3F9; mov al,0; out dx,al   (IER=0)
    "BAFB03" "B080" "EE"          # mov dx,0x3FB; mov al,0x80; out dx,al (LCR DLAB=1)
    "BAF803" "B001" "EE"          # mov dx,0x3F8; mov al,1; out dx,al   (DLL=1, 115200)
    "BAF903" "B000" "EE"          # mov dx,0x3F9; mov al,0; out dx,al   (DLM=0)
    "BAFB03" "B003" "EE"          # mov dx,0x3FB; mov al,3; out dx,al   (LCR 8N1 DLAB=0)
    "BAFA03" "B0C7" "EE"          # mov dx,0x3FA; mov al,0xC7; out dx,al (FCR FIFO)
    "BAFC03" "B00B" "EE"          # mov dx,0x3FC; mov al,0x0B; out dx,al (MCR OUT2/DTR/RTS)
)
assert len(uart) == 42, len(uart)

stage2 = bytearray(open("build/stage2_good.bin", "rb").read())
assert len(stage2) == 16384, len(stage2)
# Sanity: last 64 bytes should be padding (zeros)
assert stage2[-64:] == b"\x00" * 64, "stage2 tail not padding; shift unsafe"

new = uart + stage2[42:]   # prepend UART init, drop 42 trailing padding bytes
assert len(new) == 16384, len(new)
open("build/stage2_uart.bin", "wb").write(new)
print("built build/stage2_uart.bin (%d bytes)" % len(new))

# Rebuild image: boot.bin + stage2_uart + kernel_good
boot = open("build/boot.bin", "rb").read()
kern = open("build/kernel_good.bin", "rb").read()
img = boot + new + kern
open("build/os_v2_patched2.img", "wb").write(img)
print("built build/os_v2_patched2.img (%d bytes)" % len(img))
