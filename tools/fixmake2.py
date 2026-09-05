p = "Makefile"
t = open(p, encoding="utf-8").read()
t = t.replace(" $(BUILD)/ap_trampoline.o", "")
open(p, "w", encoding="utf-8").write(t)
print("remaining ap_trampoline in 1251/1252:", t.count("ap_trampoline.o"))
