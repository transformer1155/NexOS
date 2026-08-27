p = r"d:\MyOS\bootloader\Makefile"
lines = open(p, encoding="utf-8").read().split("\n")
line = lines[1251]  # 0-indexed -> line 1252
# collapse repeated 'build/ap_trampoline.o' tokens into a single one
import re
# remove all occurrences of the token, then add exactly one at the end of the
# object list (before any trailing backslash).
has_tramp = "build/ap_trampoline.o" in line
line = line.replace("build/ap_trampoline.o", "").replace("  ", " ").strip()
if has_tramp:
    # append before trailing backslash if present
    if line.endswith("\\"):
        line = line[:-1].rstrip() + " $(BUILD)/ap_trampoline.o \\"
    else:
        line = line + " $(BUILD)/ap_trampoline.o"
lines[1251] = line
open(p, "w", encoding="utf-8").write("\n".join(lines))
print("line 1252 now has ap_trampoline.o count:", line.count("ap_trampoline.o"))
print(line[-120:])
