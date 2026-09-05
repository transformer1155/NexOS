import sys
p = sys.argv[1]
t = open(p, encoding="latin-1", errors="replace").read()
print("LEN", len(t))
for l in t.splitlines():
    if any(k in l for k in ["login", "K64", "switch", "RESULT", "distnet", "Welcome", "root", "incorrect"]):
        print(repr(l))
