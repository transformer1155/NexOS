p = r"d:\MyOS\bootloader\Makefile"
lines = open(p, encoding="utf-8").read().split("\n")
L = lines[1251]  # line 1252 (0-indexed)
# Remove every occurrence of the ap_trampoline.o token, then add exactly one
# at the very end of the object list.
L = L.replace("$(BUILD)/ap_trampoline.o", "")
# collapse any double spaces created
import re
L = re.sub(r" +", " ", L).strip()
L = L + " $(BUILD)/ap_trampoline.o"
lines[1251] = L
open(p, "w", encoding="utf-8").write("\n".join(lines))
print("FIXED 1252; ap_trampoline.o count =", L.count("ap_trampoline.o"))
print("TAIL:", L[-90:])
