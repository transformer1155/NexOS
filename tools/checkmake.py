p = "Makefile"
for i, l in enumerate(open(p, encoding="utf-8").read().split("\n")):
    if "kernel64.elf:" in l or (l.strip().startswith("$(LD64)") and "kernel64" in l):
        print(i, "smp_bringup" in l, "ap_trampoline" in l, "smp64.o" in l)
