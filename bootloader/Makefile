# =====================================================================
#  Makefile  -  BIOS + UEFI bootloader, C++ kernel, MKFS + SFS file systems
# =====================================================================

AS      := nasm
CC      := g++
LD      := ld
OBJCOPY := objcopy
PYTHON  := python3

BUILD   := build
IMG     := $(BUILD)/os.img
UEFI_IMG := $(BUILD)/os_uefi.img
SFS_IMG := $(BUILD)/sfs.img
SFS_DIR := sfs_files

# ----- Flags -----
ASFLAGS_BIN := -f bin
ASFLAGS_ELF := -f elf32

CXXFLAGS := -m32 -ffreestanding -fno-exceptions -fno-rtti \
            -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none \
            -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra

LDFLAGS  := -m elf_i386 -nostdlib -T linker.ld -z noexecstack

# ----- 64-bit kernel flags -----
CXXFLAGS64 := -m64 -ffreestanding -fno-exceptions -fno-rtti \
              -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none \
              -fno-asynchronous-unwind-tables -nostdlib -O2 -Wall -Wextra \
              -mno-red-zone -mcmodel=large
LDFLAGS64  := -m elf_x86_64 -nostdlib -T linker64.ld -z noexecstack

# ----- UEFI build variables -----
EFI_INC   := /usr/include/efi
EFI_LIB   := /usr/lib
EFI_CRT0  := $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS   := $(EFI_LIB)/elf_x86_64_efi.lds
OVMF_CODE := /usr/share/OVMF/OVMF_CODE.fd
OVMF_VARS := /usr/share/OVMF/OVMF_VARS.fd

EFI_CFLAGS  := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -DEFI_FUNCTION_WRAPPER \
               -DGNU_EFI_USE_MS_ABI -fno-stack-protector -fshort-wchar \
               -Wall -Wextra
EFI_LDFLAGS := -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic \
               -L$(EFI_LIB) $(EFI_CRT0)

# SFS is written to disk at LBA 3328 (after kernel.bin 33-1181 + kernel64 2048-3301)
SFS_LBA      := 3328
SFS_BYTE_OFF := $(shell echo $$(( $(SFS_LBA) * 512 )))

# 64-bit kernel is written to disk at LBA 2048 (kernel.bin may be up to 1 MiB)
KERNEL64_LBA := 2048

ISO        := $(BUILD)/os.iso
ISO_ROOT   := $(BUILD)/iso_root
HYBRID_IMG := $(BUILD)/os_hybrid.img

.PHONY: all clean run test test-ai test-net disasm uefi uefi-run uefi-test sfs \
        kernel64 switch64 \
        bios-run bios-test \
        iso iso-run iso-run-uefi iso-test iso-test-uefi \
        hybrid hybrid-run hybrid-run-uefi hybrid-test hybrid-test-uefi \
        test-all

all: $(UEFI_IMG)

# =====================================================================
#  BIOS boot path
# =====================================================================

# ----- Stage 1: 512-byte boot sector -----
$(BUILD)/boot.bin: boot.asm | $(BUILD)
	$(AS) $(ASFLAGS_BIN) boot.asm -o $@

# ----- Stage 2: padded to 16 KiB (32 sectors) inside the source -----
$(BUILD)/stage2.bin: stage2.asm | $(BUILD)
	$(AS) $(ASFLAGS_BIN) stage2.asm -o $@

# ----- Kernel objects -----
$(BUILD)/entry.o: entry.asm | $(BUILD)
	$(AS) $(ASFLAGS_ELF) entry.asm -o $@

$(BUILD)/kernel.o: kernel.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c kernel.cpp -o $@

$(BUILD)/ai_engine.o: ai_engine.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c ai_engine.cpp -o $@

$(BUILD)/net.o: net.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c net.cpp -o $@

$(BUILD)/gui.o: gui.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c gui.cpp -o $@

$(BUILD)/winloader.o: winloader.cpp | $(BUILD)
	$(CC) $(CXXFLAGS) -c winloader.cpp -o $@

# ----- 32-to-64 switcher (linked into 32-bit kernel) -----
$(BUILD)/switch32to64.o: switch32to64.asm | $(BUILD)
	$(AS) $(ASFLAGS_ELF) switch32to64.asm -o $@

# ----- Link kernel ELF (entry.o first => _start at image offset 0) -----
$(BUILD)/kernel.elf: $(BUILD)/entry.o $(BUILD)/kernel.o $(BUILD)/ai_engine.o $(BUILD)/net.o $(BUILD)/gui.o $(BUILD)/winloader.o $(BUILD)/switch32to64.o linker.ld | $(BUILD)
	$(LD) $(LDFLAGS) -o $@ $(BUILD)/entry.o $(BUILD)/kernel.o $(BUILD)/ai_engine.o $(BUILD)/net.o $(BUILD)/gui.o $(BUILD)/winloader.o $(BUILD)/switch32to64.o

# ----- Extract flat kernel binary -----
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf | $(BUILD)
	$(OBJCOPY) -O binary $< $@

# =====================================================================
#  64-bit kernel build
#  Compiled with -m64, linked at 0x100000 (1MB)
#  Contains: entry64.asm + kernel64.cpp + switch64to32.asm
# =====================================================================

# ----- 64-bit kernel objects -----
$(BUILD)/entry64.o: entry64.asm | $(BUILD)
	$(AS) -f elf64 entry64.asm -o $@

$(BUILD)/kernel64.o: kernel64.cpp | $(BUILD)
	$(CC) $(CXXFLAGS64) -c kernel64.cpp -o $@

$(BUILD)/ai_engine64.o: ai_engine.cpp | $(BUILD)
	$(CC) $(CXXFLAGS64) -c ai_engine.cpp -o $@

$(BUILD)/net64.o: net.cpp | $(BUILD)
	$(CC) $(CXXFLAGS64) -c net.cpp -o $@

$(BUILD)/switch64to32.o: switch64to32.asm | $(BUILD)
	$(AS) -f elf64 switch64to32.asm -o $@

$(BUILD)/gui64.o: gui.cpp | $(BUILD)
	$(CC) $(CXXFLAGS64) -c gui.cpp -o $@

$(BUILD)/winloader64.o: winloader.cpp | $(BUILD)
	$(CC) $(CXXFLAGS64) -c winloader.cpp -o $@

# ----- Link 64-bit kernel ELF (entry64.o first => _start64 at offset 0) -----
$(BUILD)/kernel64.elf: $(BUILD)/entry64.o $(BUILD)/kernel64.o $(BUILD)/ai_engine64.o $(BUILD)/net64.o $(BUILD)/gui64.o $(BUILD)/winloader64.o $(BUILD)/switch64to32.o linker64.ld | $(BUILD)
	$(LD) $(LDFLAGS64) -o $@ $(BUILD)/entry64.o $(BUILD)/kernel64.o $(BUILD)/ai_engine64.o $(BUILD)/net64.o $(BUILD)/gui64.o $(BUILD)/winloader64.o $(BUILD)/switch64to32.o

# ----- Extract flat 64-bit kernel binary -----
$(BUILD)/kernel64.bin: $(BUILD)/kernel64.elf | $(BUILD)
	$(OBJCOPY) -O binary $< $@

kernel64: $(BUILD)/kernel64.bin
	@echo "==> 64-bit kernel built: $(BUILD)/kernel64.bin ($$(stat -c%s $(BUILD)/kernel64.bin) bytes)"

# =====================================================================
#  SFS (Simple File System) image generation
#  Packs files from sfs_files/ into a read-only FS image.
# =====================================================================
$(SFS_IMG): $(wildcard $(SFS_DIR)/*) tools/sfs_gen.py | $(BUILD)
	$(PYTHON) tools/sfs_gen.py $(SFS_DIR) $@

sfs: $(SFS_IMG)

# =====================================================================
#  Disk image assembly
#  boot + stage2 + kernel + kernel64 at LBA 400 + SFS at LBA 800, padded to 2 MiB
# =====================================================================
$(IMG): $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin $(BUILD)/kernel64.bin $(SFS_IMG) | $(BUILD)
	cat $(BUILD)/boot.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin > $@
	truncate -s 2M $@
	# Write kernel64.bin at LBA 400
	dd if=$(BUILD)/kernel64.bin of=$@ bs=512 seek=$(KERNEL64_LBA) conv=notrunc 2>/dev/null
	# Write SFS image at LBA 800
	dd if=$(SFS_IMG) of=$@ bs=512 seek=$(SFS_LBA) conv=notrunc 2>/dev/null
	@echo "==> BIOS Image built: $(IMG) ($$(stat -c%s $@) bytes)"
	@echo "    Kernel32: $$(stat -c%s $(BUILD)/kernel.bin) bytes"
	@echo "    Kernel64: $$(stat -c%s $(BUILD)/kernel64.bin) bytes at LBA $(KERNEL64_LBA)"
	@echo "    SFS:      $$(stat -c%s $(SFS_IMG)) bytes at LBA $(SFS_LBA)"

# =====================================================================
#  UEFI boot path
# =====================================================================

# ----- Compile UEFI bootloader C -----
$(BUILD)/bootuefi.o: uefi/bootuefi.c | $(BUILD)
	gcc $(EFI_CFLAGS) -I. -c $< -o $@

# ----- Assemble UEFI mode-transition stub -----
$(BUILD)/enter_kernel_uefi.o: uefi/enter_kernel.S | $(BUILD)
	gcc -c $< -o $@

# ----- Assemble RIP-relative embedded kernel accessor (avoids PE32+ relocation issues) -----
$(BUILD)/get_embedded_uefi.o: uefi/get_embedded.S | $(BUILD)
	gcc -c $< -o $@

# ----- Generate relocatable object with kernel.bin embedded (avoids OVMF FAT12 bug) -----
$(BUILD)/kernel_blob.o: $(BUILD)/kernel.bin | $(BUILD)
	cp $< $(BUILD)/kernel.blob
	$(LD) -r -b binary -o $@ $(BUILD)/kernel.blob

# ----- Link UEFI shared object (includes embedded kernel blob) -----
$(BUILD)/bootx64.so: $(BUILD)/bootuefi.o $(BUILD)/enter_kernel_uefi.o $(BUILD)/get_embedded_uefi.o $(BUILD)/kernel_blob.o | $(BUILD)
	$(LD) $(EFI_LDFLAGS) $^ -o $@ -lefi -lgnuefi

# ----- Convert to PE32+ EFI application -----
# NOTE: objcopy's --target=efi-app-x86-64 does NOT properly convert ELF
# .rela relocations to PE32+ .reloc base relocations (tested with
# binutils 2.38).  We use gen_reloc.py to manually generate the .reloc
# section from the ELF .rela entries and patch it into the PE32+ binary.
$(BUILD)/BOOTX64.EFI: $(BUILD)/bootx64.so tools/gen_reloc.py | $(BUILD)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .dynamic \
	           -j .dynsym -j .dynstr \
	           --target=efi-app-x86_64 $< $@
	$(PYTHON) tools/gen_reloc.py $(BUILD)/bootx64.so $@

# ----- Build ESP (EFI System Partition) image with bootloader + kernels -----
# Uses FAT16 format (16 MiB) for El Torito CD boot (removable media = FAT12/16 OK).
# boot-load-size = 32768 (16MB / 512) fits in 16-bit El Torito field.
# VirtualBox UEFI: FAT16 + non-zero boot-load-size + ISO9660 fallback = compatible.
$(BUILD)/esp.img: $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(BUILD)/kernel64.bin $(SFS_IMG) | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=16 2>/dev/null
	mformat -i $@ ::
	mmd     -i $@ ::/EFI
	mmd     -i $@ ::/EFI/BOOT
	mcopy   -i $@ $(BUILD)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy   -i $@ $(BUILD)/kernel.bin   ::/kernel.bin
	mcopy   -i $@ $(BUILD)/kernel64.bin ::/kernel64.bin
	@echo "==> ESP image built: $(BUILD)/esp.img ($$(stat -c%s $@) bytes, FAT16)"

# ----- Combined UEFI disk image (GPT + ESP + kernel64 at LBA 400 + SFS) -----
# Uses GPT partition table so OVMF/VirtualBox can find the ESP.
# Layout:
#   LBA 0:      Protective MBR
#   LBA 1:      GPT header
#   LBA 2-33:   GPT partition entry array
#   LBA 34-399: Unused (gap)
#   LBA 400:    kernel64.bin (raw disk offset)
#   LBA 800:    SFS image (raw disk offset)
#   LBA 1024+:  ESP partition (FAT16, contains BOOTX64.EFI at /EFI/BOOT/BOOTX64.EFI)
$(UEFI_IMG): $(BUILD)/esp.img $(BUILD)/kernel64.bin $(SFS_IMG) tools/make_gpt_uefi.py | $(BUILD)
	$(PYTHON) tools/make_gpt_uefi.py $(BUILD)/esp.img $@ $(BUILD)/kernel64.bin $(SFS_IMG)
	@echo "==> UEFI Image built: $(UEFI_IMG) ($$(stat -c%s $@) bytes)"

# ----- Top-level UEFI build target -----
uefi: $(UEFI_IMG)

# =====================================================================
#  ISO build (BIOS no-emulation + UEFI for CD/DVD and USB)
#  - BIOS:  El Torito no-emulation (boot_cd.asm reads kernel from CD)
#  - UEFI:  El Torito no-emulation (esp.img with BOOTX64.EFI)
#  - GPT:   ESP appended as partition 2 for UEFI USB boot
# =====================================================================

# ----- CD boot sector (no-emulation, 2048-byte CD sectors) -----
$(BUILD)/boot_cd.bin: boot_cd.asm | $(BUILD)
	$(AS) $(ASFLAGS_BIN) boot_cd.asm -o $@

# ----- CD boot image (boot_cd.bin + kernel, 2048-byte aligned) -----
$(BUILD)/cd_boot.img: $(BUILD)/boot_cd.bin $(BUILD)/kernel.bin tools/make_cd_boot.py | $(BUILD)
	$(PYTHON) tools/make_cd_boot.py $(BUILD)/boot_cd.bin $(BUILD)/kernel.bin $@

# ----- Prepare ISO root directory -----
# Includes /EFI/BOOT/BOOTX64.EFI in ISO9660 filesystem as a fallback for
# VirtualBox UEFI (Bug #19910: ISO9660 driver interferes with El Torito boot).
# Also includes kernel binaries for direct UEFI loading.
$(ISO_ROOT)/boot/cd_boot.img: $(BUILD)/cd_boot.img | $(ISO_ROOT)
	cp $< $@

$(ISO_ROOT)/boot/esp.img: $(BUILD)/esp.img | $(ISO_ROOT)
	cp $< $@

$(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI: $(BUILD)/BOOTX64.EFI | $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/EFI/BOOT
	cp $< $@

$(ISO_ROOT)/kernel.bin: $(BUILD)/kernel.bin | $(ISO_ROOT)
	cp $< $@

$(ISO_ROOT)/kernel64.bin: $(BUILD)/kernel64.bin | $(ISO_ROOT)
	cp $< $@

$(ISO_ROOT):
	mkdir -p $(ISO_ROOT)/boot

# ----- Build hybrid ISO (BIOS + UEFI, VirtualBox-compatible) -----
# UEFI boot path 1: El Torito no-emulation (esp.img as FAT16 boot image)
# UEFI boot path 2: ISO9660 fallback (/EFI/BOOT/BOOTX64.EFI in filesystem)
# boot-load-size = 32768 (16MB ESP / 512 bytes per sector)
$(ISO): $(ISO_ROOT)/boot/cd_boot.img $(ISO_ROOT)/boot/esp.img \
        $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI \
        $(ISO_ROOT)/kernel.bin $(ISO_ROOT)/kernel64.bin | $(BUILD)
	xorriso -as mkisofs \
	  -R -J -V MINIOS \
	  -c boot/boot.cat \
	  -b boot/cd_boot.img \
	  -no-emul-boot \
	  -boot-load-size 4 \
	  -boot-info-table \
	  -eltorito-alt-boot \
	  -e boot/esp.img \
	  -no-emul-boot \
	  -boot-load-size 32768 \
	  -append_partition 2 C12A7328-F81F-11D2-BA4B-00A0C93EC93B $(BUILD)/esp.img \
	  -output $(ISO) \
	  $(ISO_ROOT)
	@echo "==> ISO built: $(ISO) ($$(stat -c%s $(ISO)) bytes)"
	@echo "    BIOS:  no-emulation CD boot (boot_cd.asm + kernel)"
	@echo "    UEFI:  El Torito (esp.img) + ISO9660 fallback (/EFI/BOOT/BOOTX64.EFI)"
	@echo "    Burn to CD/DVD or dd to USB for real hardware"

iso: $(ISO)

# =====================================================================
#  Hybrid USB image (BIOS + UEFI bootable, 512-byte sectors)
#  Layout:
#    LBA 0-32:    boot.asm + stage2 (BIOS boot)
#    LBA 33+:     kernel.bin (32-bit kernel, BIOS path)
#    LBA 400:     kernel64.bin (64-bit kernel)
#    LBA 800:     SFS file system
#    LBA 1024+:   ESP partition (UEFI boot, FAT32)
#  MBR partition entry 1: ESP at LBA 1024, type 0xEF
# =====================================================================
$(HYBRID_IMG): $(IMG) $(BUILD)/esp.img tools/patch_mbr.py | $(BUILD)
	cp $(IMG) $@
	# Extend image to fit ESP at LBA 1024 (16MB ESP)
	truncate -s $$(( (1024 + 32768 + 64) * 512 )) $@
	# Write ESP at LBA 1024
	dd if=$(BUILD)/esp.img of=$@ bs=512 seek=1024 conv=notrunc 2>/dev/null
	# Patch MBR: partition 1 = ESP at LBA 1024, type 0xEF (EFI)
	$(PYTHON) -c "\
import struct,sys; \
d=bytearray(open('$@','rb').read()); \
p=bytearray(16); \
p[0]=0x80; p[1:4]=b'\x00\x21\x00'; p[4]=0xEF; p[5:8]=b'\xfe\xff\xff'; \
struct.pack_into('<I',p,8,1024); struct.pack_into('<I',p,12,32768); \
d[446:462]=p; open('$@','wb').write(d)"
	@echo "==> Hybrid USB image built: $(HYBRID_IMG) ($$(stat -c%s $@) bytes)"
	@echo "    BIOS:  boot.asm -> stage2 -> kernel (512-byte sectors)"
	@echo "    UEFI:  ESP at LBA 1024 (type 0xEF, BOOTX64.EFI)"
	@echo "    dd to USB: dd if=$(HYBRID_IMG) of=/dev/sdX bs=4M"

hybrid: $(HYBRID_IMG)

# =====================================================================
#  QEMU run / test targets
# =====================================================================

$(BUILD):
	mkdir -p $(BUILD)

# ----- Build 64-bit kernel + rebuild disk image with it -----
switch64: $(IMG)
	@echo "==> Disk image with 64-bit kernel ready: $(IMG)"
	@echo "    Run 'make run' and type 'switch' in the shell to switch to 64-bit"

# ----- UEFI: run in a QEMU window (default target) -----
run: $(UEFI_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-drive format=raw,file=$(UEFI_IMG) -m 64M

# ----- BIOS: run in a QEMU window (legacy) -----
bios-run: $(IMG)
	qemu-system-x86_64 -drive format=raw,file=$(IMG) -m 64M \
		-net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080

# ----- UEFI: automated headless test (default) -----
test: $(UEFI_IMG)
	./test_uefi.sh $(UEFI_IMG)

# ----- BIOS: automated headless test (legacy) -----
bios-test: $(IMG)
	./test.sh $(IMG)

# ----- AI engine: automated headless test -----
test-ai: $(IMG)
	./test_ai.sh $(IMG)

# ----- Network: automated HTTP server test -----
test-net: $(IMG)
	./test_net.sh $(IMG)

# ----- UEFI: run in a QEMU window with OVMF -----
uefi-run: $(UEFI_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-drive format=raw,file=$(UEFI_IMG) -m 64M

# ----- UEFI: automated headless test -----
uefi-test: $(UEFI_IMG)
	./test_uefi.sh $(UEFI_IMG)

# ----- ISO: run in QEMU (BIOS mode, x86_64 for switch support) -----
iso-run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -boot d -m 2G

# ----- User data disk (secondary ATA VHD). Files persist across reboots. -----
data-vhd: build/data.vhd
build/data.vhd: tools/make_data_vhd.py
	python3 tools/make_data_vhd.py 8

# ----- ISO + data VHD: run with a persistent user disk (recommended) -----
iso-run-data: $(ISO) build/data.vhd
	qemu-system-x86_64 -cdrom $(ISO) -boot d -m 2G \
		-drive file=build/data.vhd,format=raw,if=ide,index=1
# ----- ISO: run in QEMU (UEFI mode) -----
iso-run-uefi: $(ISO)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-cdrom $(ISO) -m 64M

# ----- ISO: automated headless test (BIOS) -----
iso-test: $(ISO)
	./test_iso.sh $(ISO)

# ----- ISO: automated headless test (UEFI) -----
iso-test-uefi: $(ISO)
	./test_uefi.sh $(ISO) iso

# ----- Hybrid USB: run in QEMU (BIOS mode, x86_64 for switch support) -----
hybrid-run: $(HYBRID_IMG)
	qemu-system-x86_64 -drive format=raw,file=$(HYBRID_IMG) -m 64M \
		-net nic,model=ne2k_isa -net user,hostfwd=tcp::8080-:8080

# ----- Hybrid USB: run in QEMU (UEFI mode) -----
hybrid-run-uefi: $(HYBRID_IMG)
	cp $(OVMF_VARS) $(BUILD)/ovmf_vars.fd
	qemu-system-x86_64 -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/ovmf_vars.fd \
		-drive format=raw,file=$(HYBRID_IMG) -m 64M

# ----- Hybrid USB: automated headless test (BIOS) -----
hybrid-test: $(HYBRID_IMG)
	./test.sh $(HYBRID_IMG)

# ----- Hybrid USB: automated headless test (UEFI) -----
hybrid-test-uefi: $(HYBRID_IMG)
	./test_uefi.sh $(HYBRID_IMG)

# ----- Run all tests -----
test-all: test test-ai test-net uefi-test iso-test iso-test-uefi hybrid-test hybrid-test-uefi
	@echo ""
	@echo "============================================"
	@echo "  ALL TESTS COMPLETE"
	@echo "  BIOS disk:      test"
	@echo "  AI engine:      test-ai"
	@echo "  Network/HTTP:   test-net"
	@echo "  UEFI disk:      uefi-test"
	@echo "  ISO BIOS:       iso-test"
	@echo "  ISO UEFI:       iso-test-uefi"
	@echo "  Hybrid BIOS:    hybrid-test"
	@echo "  Hybrid UEFI:    hybrid-test-uefi"
	@echo "============================================"

# =====================================================================
#  Misc
# =====================================================================

# ----- Inspect kernel symbols / disassembly -----
disasm: $(BUILD)/kernel.elf
	@echo "== entry point =="; nm $(BUILD)/kernel.elf | grep -E ' _start| kmain'
	@echo "== disassembly (head) =="; objdump -d -M intel $(BUILD)/kernel.elf | head -40

clean:
	rm -rf $(BUILD)
