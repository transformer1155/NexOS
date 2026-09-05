p = r"d:\MyOS\bootloader\.attic64\smp_bringup.cpp"
s = open(p, encoding="utf-8").read()
old = '''extern "C" void ap_main(int idx){
    outb(0x3F8, (uint8_t)('A'));
    if (idx >= 0 && idx < MAX_CPUS) g_cpu[idx].present = 1;
    serial_puts("CPU");'''
new = '''extern "C" void ap_main(int idx){
    outb(0x3F8, (uint8_t)('A'));
    outb(0x3F8, (uint8_t)('B'));
    if (idx >= 0 && idx < MAX_CPUS) g_cpu[idx].present = 1;
    outb(0x3F8, (uint8_t)('C'));
    serial_puts("CPU");'''
if old not in s:
    print("PATTERN NOT FOUND")
else:
    s = s.replace(old, new, 1)
    open(p, "w", encoding="utf-8").write(s)
    print("patched ap_main with B/C markers")
