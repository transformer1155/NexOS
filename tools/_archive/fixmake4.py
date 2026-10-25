p = "Makefile"
lines = open(p, encoding="utf-8").read().split("\n")
out = []
i = 0
while i < len(lines):
    l = lines[i]
    if l.strip().startswith("$(CC64) $(CXX64FLAGS) -c .attic64/smp_bringup.cpp"):
        # skip this line and the following comment lines until blank
        i += 1
        while i < len(lines) and (lines[i].strip().startswith("#") or lines[i].strip() == ""):
            i += 1
        continue
    out.append(l)
    i += 1
open(p, "w", encoding="utf-8").write("\n".join(out))
t = "\n".join(out)
print("smp_bringup/ap_trampoline remaining:", t.count("smp_bringup") + t.count("ap_trampoline"))
