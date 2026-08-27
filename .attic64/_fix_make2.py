p = r"d:\MyOS\bootloader\Makefile"
s = open(p, encoding="utf-8").read()
# remove any duplicate ap_trampoline.o tokens (keep exactly one)
while "ap_trampoline.o ap_trampoline.o" in s:
    s = s.replace("ap_trampoline.o ap_trampoline.o", "ap_trampoline.o")
open(p, "w", encoding="utf-8").write(s)
print("dedup done; count now:", s.count("ap_trampoline.o"))
