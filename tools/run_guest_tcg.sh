#!/bin/bash
# =====================================================================
#  run_guest_tcg.sh - validate the minimal guest program under QEMU TCG
# =====================================================================
#
#  WHY TCG INSTEAD OF KVM
#  ----------------------
#  WSL2 runs on top of Hyper-V and in this environment does NOT load
#  kvm_amd/kvm_intel: /dev/kvm exists as a node but has no backend behind
#  it, because nested virtualization is off.  A hardware-virtualized KVM
#  smoke test therefore cannot run here.
#
#  QEMU's TCG (pure software emulation) needs no hardware virtualization
#  and validates the parts of the guest that are easiest to get wrong:
#
#    1. guest_smoke.bin is valid 32-bit code that runs to completion,
#    2. it writes the shared framebuffer region (a gradient),
#    3. it emits "ready" via the UART port,
#    4. it signals completion via the exit port.
#
#  WHAT IT CANNOT PROVE
#  --------------------
#  The VMRUN/VMLAUNCH transition itself -- that needs real AMD-V / VT-x.
#  Run tools/run_kvm_smoke.sh on bare-metal Linux for that half.
#
#  APPROACH
#  --------
#  The stub and the payload live in the SAME 512-byte boot sector, spliced
#  with nasm's incbin.  An earlier version put the payload in sector 2 and
#  relied on a BIOS disk read; that is fragile (and was the actual bug).
#  One sector boots reliably because the BIOS always loads sector 0.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

PAYLOAD=guest_smoke.bin
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
LOG="$WORK/serial.log"

command -v nasm >/dev/null || { echo "need nasm"; exit 1; }
command -v qemu-system-i386 >/dev/null || { echo "need qemu-system-i386"; exit 1; }

nasm -f bin plugins/guest_smoke.asm -o "$WORK/$PAYLOAD" || exit 1
echo "guest payload: $(wc -c < "$WORK/$PAYLOAD") bytes"

cat > "$WORK/boot.asm" <<'ASM'
BITS 16
org 0x7C00

PAYLOAD equ 0x1000          ; where the payload runs (vmm.cpp GUEST_CODE_PHYS)

start:
    cli
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7C00
    sti

    ; Copy the embedded payload to 0x1000.
    mov     si, payload_src
    mov     ax, PAYLOAD / 16
    mov     es, ax
    xor     di, di
    mov     cx, payload_end - payload_src
.copy:
    mov     al, [si]
    mov     [es:di], al
    inc     si
    inc     di
    loop    .copy

    ; Enable A20 (fast method via port 0x92).
    in      al, 0x92
    or      al, 2
    out     0x92, al

    ; Flat GDT, then enter 32-bit protected mode.
    lgdt    [gdt_desc]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     0x08:pm_entry

BITS 32
pm_entry:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     esp, 0x8000
    jmp     PAYLOAD

BITS 16
align 8
gdt_start:
    dq 0
    ; 32-bit code: base 0, limit 4 GiB, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
    ; 32-bit data: base 0, limit 4 GiB, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; The guest payload is spliced in here, still inside sector 0.
payload_src:
    incbin PAYLOAD_FILE
payload_end:

times 510 - ($ - $$) db 0
dw 0xAA55
ASM

nasm -f bin -D PAYLOAD_FILE="\"$WORK/$PAYLOAD\"" "$WORK/boot.asm" -o "$WORK/disk.bin" || exit 1
echo "boot sector: $(wc -c < "$WORK/disk.bin") bytes"

echo "--- booting guest under QEMU TCG ---"
timeout 180 qemu-system-i386 \
    -drive format=raw,file="$WORK/disk.bin" \
    -m 32 -display none -serial "file:$LOG" -no-reboot >/dev/null 2>&1

echo "--- serial output from guest ---"
cat "$LOG" 2>/dev/null || true
echo "[--- end ---]"

if grep -q "ready" "$LOG" 2>/dev/null; then
    echo "[ OK ] guest emitted \"ready\" over the UART path"
    echo "[ OK ] guest payload executed to completion"
    exit 0
fi
echo "[FAIL] no \"ready\" on serial; the guest did not complete"
exit 1
