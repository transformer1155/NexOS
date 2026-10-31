#!/usr/bin/env python3
"""Read and decode the in-kernel AMD-V bring-up record.

The 64-bit kernel writes one sector at SVM_LOG_LBA (2047) via the
range-guarded blk_write_sector(), so the record sits flush against
kernel64.bin (LBA 2048) in the free gap between the 32-bit kernel image
and the SFS.  This tool is the read side of that contract.

Usage:
    python3 tools/read_svm_log.py [image] [--lba N]

Defaults to build/os_v2.img and LBA 2047.
"""
import struct
import sys
import os

SVM_LOG_LBA_DEFAULT = 2047
SECTOR = 512
MAGIC = 0x4B4D5653          # 'SVMK' little-endian

RC_NAMES = {
    0:  "OK                (guest ran and halted)",
    -1: "ERR_NOMEM         (out of physical frames / misaligned)",
    -2: "ERR_NOAMD         (not AuthenticAMD, or SVM bit / VM_CR disabled)",
    -3: "ERR_NOSVME        (EFER.SVME did not stick)",
    -4: "ERR_VMFAIL        (VMCB rejected: #VMFAILVALID)",
    -5: "ERR_BUSY          (vmexit budget exhausted before HLT)",
}


def main():
    args = [a for a in sys.argv[1:]]
    lba = SVM_LOG_LBA_DEFAULT
    if "--lba" in args:
        i = args.index("--lba")
        lba = int(args[i + 1])
        del args[i:i + 2]
    img = args[0] if args else "build/os_v2.img"

    if not os.path.exists(img):
        print(f"error: image not found: {img}", file=sys.stderr)
        print("  (run 'make' first, or pass the image path)", file=sys.stderr)
        return 2

    with open(img, "rb") as f:
        f.seek(lba * SECTOR)
        raw = f.read(SECTOR)

    if len(raw) < SECTOR:
        print(f"error: short read at LBA {lba} (image ends first)", file=sys.stderr)
        return 2

    fields = struct.unpack_from("<9I", raw, 0)
    (magic, rc, exits, halted, rip, vmcb_pa, hsave_pa, guest_pa, uart_n) = fields

    print(f"image      : {img}  ({os.path.getsize(img)} bytes)")
    print(f"sector     : LBA {lba}  ({lba * SECTOR} .. {lba * SECTOR + SECTOR - 1})")
    print()

    if magic != MAGIC:
        print(f"magic      : 0x{magic:08X}  != expected 0x{MAGIC:08X}")
        print()
        print("This sector does not hold an SVM record.")
        if magic == 0:
            print("It is all zero: the kernel never ran svm_k64_bringup(),")
            print("or the boot did not reach the framebuffer-console stage.")
        else:
            print("It holds something else; check the LBA before drawing")
            print("conclusions -- the writer refuses to touch a non-gap sector.")
        return 1

    signed_rc = rc - (1 << 32) if rc >= (1 << 31) else rc
    name = RC_NAMES.get(signed_rc, "unknown")
    print(f"magic      : 0x{magic:08X}  ('SVMK' - record is valid)")
    print(f"rc         : {signed_rc}  ({name})")
    print()
    print(f"exits      : {exits}       (#VMEXITs dispatched)")
    print(f"halted     : {halted}       (1 = guest executed HLT)")
    print(f"rip        : 0x{rip:08X}")
    print(f"vmcb_pa    : 0x{vmcb_pa:08X}   aligned={vmcb_pa % 4096 == 0}")
    print(f"hsave_pa   : 0x{hsave_pa:08X}   aligned={hsave_pa % 4096 == 0}")
    print(f"guest_pa   : 0x{guest_pa:08X}   aligned={guest_pa % 4096 == 0}")
    print()

    # vUART bytes the guest emitted through OUT (the 'S','V' of the stub).
    if 0 < uart_n <= 64:
        payload = raw[36:36 + uart_n]
        printable = "".join(chr(b) if 32 <= b < 127 else "." for b in payload)
        print(f"vUART      : {uart_n} byte(s) captured")
        print(f"  hex      : {payload.hex(' ')}")
        print(f"  ascii    : {printable!r}")
        if payload == b"SV":
            print()
            print("  ^ 'SV' is the guest stub's own output: mov al,'S'; out 0xE9;")
            print("    mov al,'V'; out 0xE9; hlt.  Seeing both bytes here plus")
            print("    halted=1 proves the guest really executed in the VMCB and")
            print("    that BOTH dispatch paths (IOIO_PROT then HLT) worked.")
    else:
        print(f"vUART      : none captured (uart_n={uart_n}); the guest never")
        print("             reached its OUT instructions.")
    print()

    if signed_rc == 0 and halted == 1 and uart_n == 2:
        print("VERDICT    : AMD-V works on this machine (full pass).")
    elif signed_rc == -4:
        print("VERDICT    : SVM is present but the VMCB was rejected.  The")
        print("             serial log carries svmk_dump_vmcb(), which is the")
        print("             field-by-field picture of what was offered.")
    elif signed_rc == -2:
        print("VERDICT    : no usable AMD-V on this CPU/VM (see serial log for")
        print("             which check failed: vendor, SVM bit, or VM_CR).")
    else:
        print("VERDICT    : not a full pass; see serial log for the detail.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
