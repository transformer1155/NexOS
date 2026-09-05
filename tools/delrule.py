p = "Makefile"
lines = open(p, encoding="utf-8").read().split("\n")
out = []
skip = False
for l in lines:
    if l.startswith("# AP trampoline:") or l.startswith("$(BUILD)/ap_trampoline.o:"):
        skip = True
        continue
    if skip:
        if l.strip() == "" or l.startswith("\t"):
            continue
        else:
            skip = False
    out.append(l)
open(p, "w", encoding="utf-8").write("\n".join(out))
t = "\n".join(out)
print("ap_trampoline remaining:", t.count("ap_trampoline"))
