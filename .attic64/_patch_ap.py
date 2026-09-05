p = r"d:\MyOS\bootloader\.attic64\smp64.cpp"
s = open(p, encoding="utf-8").read()
old = 'extern "C" void ap_main(int idx){\n    if (idx >= 0'
new = 'extern "C" void ap_main(int idx){\n    outb(0x3F8, (uint8_t)(\'A\'));\n    if (idx >= 0'
if old not in s:
    print("PATTERN NOT FOUND")
else:
    s = s.replace(old, new, 1)
    open(p, "w", encoding="utf-8").write(s)
    print("patched ap_main with 'A' marker")
