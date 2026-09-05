p = r"d:\MyOS\bootloader\Makefile"
text = open(p, encoding="utf-8").read()
lines = text.split("\n")

# 1) Fix dependency (1251) and link (1252) lines.
objs = [
    "entry64.o","switch64to32.o","kernel64.o","ai_engine64.o","ai_plugin64.o",
    "kb64.o","gguf64.o","gguf_infer64.o","memory_adapter64.o","file_adapter64.o",
    "gguf_loader64.o","knowledge_base64.o","net64.o","distnet64.o","gui64.o",
    "font_vec64.o","addrman64.o","winloader64.o","win32_64.o","gdt64.o",
    "proc64.o","vfs64.o","perm64.o","clr64.o","mforms64.o","smp_bringup.o",
    "ap_trampoline.o",
]
dep = "$(BUILD)/kernel64.elf: " + " ".join("$(BUILD)/"+o for o in objs) + " .attic64/linker64.ld | $(BUILD)"
link = "\t$(LD64) $(LDFLAGS64) -o $@ " + " ".join("$(BUILD)/"+o for o in objs)
lines[1250] = dep
lines[1251] = link

# 2) Ensure build rules exist right after line 1252 (index 1251).
def ensure_rule(target_line, recipe):
    # target_line example: "$(BUILD)/smp_bringup.o: .attic64/smp_bringup.cpp .attic64/smp64.h | $(BUILD)"
    # recipe example: "\t$(CC64) $(CXX64FLAGS) -c .attic64/smp_bringup.cpp -o $@"
    for i, l in enumerate(lines):
        if l.strip().startswith(target_line.split(":")[0].strip()):
            return  # already present
    # insert after current 1252 (index 1251)
    insert_at = 1252
    lines[insert_at:insert_at] = [target_line, recipe, ""]

ensure_rule(
    "$(BUILD)/smp_bringup.o: .attic64/smp_bringup.cpp .attic64/smp64.h | $(BUILD)",
    "\t$(CC64) $(CXX64FLAGS) -c .attic64/smp_bringup.cpp -o $@",
)
ensure_rule(
    "$(BUILD)/ap_trampoline.o: .attic64/ap_trampoline.asm | $(BUILD)",
    "\t$(AS) -f bin -o $(BUILD)/ap_trampoline.bin .attic64/ap_trampoline.asm\n\t$(OBJCOPY64) -I binary -O elf64-x86-64 -B i386 $(BUILD)/ap_trampoline.bin $@",
)

open(p, "w", encoding="utf-8").write("\n".join(lines))
print("fixed. smp_bringup rule present:", any("smp_bringup.o:" in l for l in lines))
print("ap_trampoline rule present:", any("ap_trampoline.o:" in l for l in lines))
print("line1252 ap_trampoline count:", lines[1251].count("ap_trampoline.o"))
