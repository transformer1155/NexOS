p = r"d:\MyOS\bootloader\Makefile"
s = open(p, encoding="utf-8").read()
old = "$(BUILD)/mforms64.o $(BUILD)/smp_bringup.o"
new = "$(BUILD)/mforms64.o $(BUILD)/smp_bringup.o $(BUILD)/ap_trampoline.o"
if old in s:
    s = s.replace(old, new)
    open(p, "w", encoding="utf-8").write(s)
    print("patched Makefile: added ap_trampoline.o to link")
else:
    print("OLD STRING NOT FOUND; current occurrences of smp_bringup.o:")
    import re
    for m in re.finditer(r".*smp_bringup\.o.*", s):
        print("  ", m.group(0)[:120])
