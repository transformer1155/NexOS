import sys, re
s = open("kernel.cpp").read()
# strip any stray debug markers accidentally left in the source
s = re.sub(r'MARK\d+[a-zA-Z]?\n?', '', s)

init_fn = '''static void serial_init(void){
    outb(0x3F8 + 1, 0x00);   // IER: disable interrupts
    outb(0x3F8 + 3, 0x80);   // LCR: DLAB=1
    outb(0x3F8 + 0, 0x01);   // DLL: divisor low (115200 baud)
    outb(0x3F8 + 1, 0x00);   // DLM: divisor high
    outb(0x3F8 + 3, 0x03);   // LCR: 8N1, DLAB=0
    outb(0x3F8 + 2, 0xC7);   // FCR: enable FIFO, clear, 14-byte trig
    outb(0x3F8 + 4, 0x0B);   // MCR: DTR/RTS/OUT2
}
'''

a1 = 'static int serial_try_getc(void){\n    if (inb(0x3FD) & 0x01) return (int)(unsigned char)inb(0x3F8);\n    return -1;\n}\n'
assert a1 in s, "anchor1 not found"
s = s.replace(a1, a1 + "\n" + init_fn, 1)

a2 = '    pic_init();\n'
assert a2 in s, "anchor2 not found"
s = s.replace(a2, a2 + '    serial_init();\n    serial_puts("[K-s] serial init done\\n");\n', 1)

open("_kp.cpp", "w").write(s)
print("patched_ok")
