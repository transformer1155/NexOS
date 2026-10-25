p = "Makefile"
lines = open(p, encoding="utf-8").read().split("\n")
out = []
for l in lines:
    if "ap_trampoline.o" in l or "smp_bringup.o" in l:
        continue
    out.append(l)
open(p, "w", encoding="utf-8").write("\n".join(out))
t = "\n".join(out)
print("ap_trampoline remaining:", t.count("ap_trampoline"))
print("smp_bringup remaining:", t.count("smp_bringup"))
