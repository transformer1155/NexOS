p = "Makefile"
lines = open(p, encoding="utf-8").read().split("\n")
for i, l in enumerate(lines):
    if l.startswith("$(BUILD)/kernel64.elf:"):
        # dependency line
        lines[i] = "$(BUILD)/kernel64.elf: $(BUILD)/entry64.o $(BUILD)/switch64to32.o $(BUILD)/kernel64.o $(BUILD)/ai_engine64.o $(BUILD)/ai_plugin64.o $(BUILD)/kb64.o $(BUILD)/gguf64.o $(BUILD)/gguf_infer64.o $(BUILD)/memory_adapter64.o $(BUILD)/file_adapter64.o $(BUILD)/gguf_loader64.o $(BUILD)/knowledge_base64.o $(BUILD)/net64.o $(BUILD)/distnet64.o $(BUILD)/gui64.o $(BUILD)/font_vec64.o $(BUILD)/addrman64.o $(BUILD)/winloader64.o $(BUILD)/win32_64.o $(BUILD)/gdt64.o $(BUILD)/proc64.o $(BUILD)/vfs64.o $(BUILD)/perm64.o $(BUILD)/clr64.o $(BUILD)/mforms64.o $(BUILD)/smp64.o .attic64/linker64.ld | $(BUILD)"
    elif l.strip().startswith("$(LD64) $(LDFLAGS64) -o $@"):
        lines[i] = "\t$(LD64) $(LDFLAGS64) -o $@ $(BUILD)/entry64.o $(BUILD)/switch64to32.o $(BUILD)/kernel64.o $(BUILD)/ai_engine64.o $(BUILD)/ai_plugin64.o $(BUILD)/kb64.o $(BUILD)/gguf64.o $(BUILD)/gguf_infer64.o $(BUILD)/memory_adapter64.o $(BUILD)/file_adapter64.o $(BUILD)/gguf_loader64.o $(BUILD)/knowledge_base64.o $(BUILD)/net64.o $(BUILD)/distnet64.o $(BUILD)/gui64.o $(BUILD)/font_vec64.o $(BUILD)/addrman64.o $(BUILD)/winloader64.o $(BUILD)/win32_64.o $(BUILD)/gdt64.o $(BUILD)/proc64.o $(BUILD)/vfs64.o $(BUILD)/perm64.o $(BUILD)/clr64.o $(BUILD)/mforms64.o $(BUILD)/smp64.o"
open(p, "w", encoding="utf-8").write("\n".join(lines))
# verify
t = open(p, encoding="utf-8").read()
print("smp_bringup in link:", t.count("smp_bringup.o"))
print("ap_trampoline in link:", t.count("ap_trampoline.o"))
print("smp64 in link:", t.count("smp64.o"))
