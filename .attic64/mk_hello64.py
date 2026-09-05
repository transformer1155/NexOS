#!/usr/bin/env python3
"""
Build hello64.exe - a minimal x86-64 PE32+ (machine 0x8664, magic 0x20B)
used to verify NexOS's Win64 loader.

It imports OutputDebugStringA + ExitProcess from KERNEL32.DLL, calls
OutputDebugStringA("Hello from Win64 PE!\\n") (which the loader's shim
echoes to the serial log as "[app] ..."), then ExitProcess(0).

Layout (single .text section at RVA 0x1000):
  code -> msg -> IAT(8B x2 + NUL) -> ILT(8B x2 + NUL) -> names
       -> import desc -> reloc_probe -> .reloc
A single DIR64 base-relocation exercises the loader's 64-bit reloc path.

HISTORY / gotchas (both produced a PE the loader could parse but not run):
  * The ILT/IAT thunks must NOT have bit 63 (IMAGE_ORDINAL_FLAG64) set when
    importing by name - it means "by ordinal" and the name is ignored, so
    every import came back unresolved.
  * Thunk arrays are NUL-terminated. Without the terminating zero qword the
    loader walks straight into the adjacent name strings (we saw it find
    "11" bogus imports) and then writes resolved pointers past the IAT.
"""
import struct, sys, os

IMAGEBASE = 0x140000000
VBASE     = 0x1000          # section VirtualAddress (== entry RVA)
FBASE     = 0x400           # section file offset
EFN       = 0x200           # e_lfanew

text = bytearray()
va = VBASE
labels = {}
rels = []
def emit(b):
    global va
    text.extend(b); va += len(b)
def align(n):
    """pad with zeros until the current RVA is n-byte aligned"""
    while va % n:
        emit(b"\x00")
def L(name):
    labels[name] = va
def emit_rip_rel(prefix, target):
    """emit `prefix` + 4-byte RIP-relative disp to *target* (RVA).
    The disp is patched after all labels exist."""
    emit(prefix)
    off = len(text)
    emit(b"\x00\x00\x00\x00")
    rels.append((off, target, va))      # va now == instruction end (RIP base)

# ---------------- code ----------------
# Microsoft x64 ABI: the caller owns a 32-byte "home space" above the return
# address, and rsp must be 16-byte aligned at every call site.  On entry
# rsp % 16 == 8, so `sub rsp,40` gives 32B home space *and* realigns.
# Without it the callee's register spill lands on our own return address.
L("_start")
emit(b"\x48\x83\xEC\x28")                # sub rsp, 40
emit_rip_rel(b"\x48\x8D\x0D", "msg")     # lea rcx, [rip+disp]  ; arg0 = msg
emit_rip_rel(b"\xFF\x15", "iat_ods")     # call [rip+disp]       ; OutputDebugStringA
emit(b"\x31\xC9")                        # xor ecx, ecx          ; ExitProcess(0)
emit_rip_rel(b"\xFF\x15", "iat_exit")    # call [rip+disp]       ; ExitProcess

# Self-check the DIR64 fixup: the stored qword must equal reloc_probe's
# runtime address.  eax becomes 0 iff the loader relocated it correctly,
# and that lands in the loader's "entry returned rc=" line.
emit_rip_rel(b"\x48\x8D\x05", "reloc_probe")  # lea rax,[rip+d]  ; actual VA
emit_rip_rel(b"\x48\x8B\x0D", "reloc_probe")  # mov rcx,[rip+d]  ; relocated value
emit(b"\x48\x29\xC8")                    # sub rax, rcx          ; 0 == reloc OK

emit(b"\x48\x83\xC4\x28")                # add rsp, 40
emit(b"\xC3")                            # ret  -> back into the loader
#   (K_ExitProcess returns normally in NexOS - a `hlt` here would hang
#    the kernel because the loader would never regain control.)

# ---------------- data ----------------
L("msg")
emit(b"Hello from Win64 PE!\n\x00")

# FirstThunk array (the IAT the code calls through).  Thunk arrays are
# arrays of ULONGLONG and MUST be 8-byte aligned *and* NUL-terminated -
# the loader walks them until it reads a zero entry.
align(8)
L("iat_ods");  emit(b"\x00" * 8)         # filled by loader
L("iat_exit"); emit(b"\x00" * 8)
emit(b"\x00" * 8)                        # <-- array terminator

# OriginalFirstThunk array (the import lookup table)
align(8)
L("ilt_ods");  emit(b"\x00" * 8)         # patched with the name RVA below
L("ilt_exit"); emit(b"\x00" * 8)
emit(b"\x00" * 8)                        # <-- array terminator

# IMAGE_IMPORT_BY_NAME: WORD Hint + ASCII name, 2-byte aligned per the spec
align(2)
L("name_ods"); emit(b"\x00\x00"); emit(b"OutputDebugStringA\x00")
align(2)
L("name_exit");emit(b"\x00\x00"); emit(b"ExitProcess\x00")
L("dllname");  emit(b"KERNEL32.DLL\x00")

# import descriptor (20B) + zero terminator (20B)
align(4)
L("imp_desc")
emit(struct.pack("<IIIII", 0, 0, 0, 0, 0))   # patched below
emit(b"\x00" * 20)

# reloc_probe: a DIR64 target whose link-time value = IMAGEBASE + its rva
align(8)
L("reloc_probe")
emit(struct.pack("<Q", 0))                    # patched below

# ---------------- .reloc ----------------
L("reloc")
page_rva  = labels["reloc_probe"] & ~0xFFF
entry_off = labels["reloc_probe"] &  0xFFF
entry = ((10 << 12) | (entry_off & 0xFFF)).to_bytes(2, "little")  # type 10 = DIR64
reloc_block = struct.pack("<II", page_rva, 12) + entry + b"\x00\x00"  # padded to 4
emit(reloc_block)

# ---------------- patches ----------------
# `text` holds the section content; offset 0 corresponds to RVA VBASE.
def AT(name):
    return labels[name] - VBASE
# RIP-relative displacements (labels now all known)
for off, target, instr_end in rels:
    struct.pack_into("<i", text, off, labels[target] - instr_end)
struct.pack_into("<Q", text, AT("reloc_probe"), IMAGEBASE + labels["reloc_probe"])
# Import-by-NAME thunks hold the plain RVA of an IMAGE_IMPORT_BY_NAME.
# Bit 63 (IMAGE_ORDINAL_FLAG64) must stay CLEAR - setting it means
# "import by ordinal" and makes every loader ignore the name entirely.
struct.pack_into("<Q", text, AT("ilt_ods"),  labels["name_ods"])
struct.pack_into("<Q", text, AT("ilt_exit"), labels["name_exit"])
# Real linkers seed the FirstThunk array with the same lookup values; the
# loader overwrites them with the resolved addresses.
struct.pack_into("<Q", text, AT("iat_ods"),  labels["name_ods"])
struct.pack_into("<Q", text, AT("iat_exit"), labels["name_exit"])
od = struct.pack("<IIIII",
                 labels["ilt_ods"],   # OriginalFirstThunk
                 0, 0,
                 labels["dllname"],   # Name RVA
                 labels["iat_ods"])   # FirstThunk
text[AT("imp_desc"):AT("imp_desc")+20] = od

# ---------------- headers ----------------
sec_vsize = len(text)
sec_rsize = (sec_vsize + 0x1FF) & ~0x1FF
size_of_image = VBASE + ((sec_vsize + 0xFFF) & ~0xFFF)

# DOS stub (0x200)
dos = bytearray(0x200)
dos[0:2] = b"MZ"
struct.pack_into("<I", dos, 0x3C, EFN)
msg_dos = b"This program cannot be run in DOS mode.\r\n$"
dos[0x40:0x40+len(msg_dos)] = msg_dos

# COFF header (20B): Machine, NumberOfSections, TimeDateStamp,
# PointerToSymbolTable, NumberOfSymbols, SizeOfOptionalHeader, Characteristics
coff = struct.pack("<HHIIIHH",
                   0x8664,            # Machine
                   1,                 # NumberOfSections
                   0,                 # TimeDateStamp
                   0,                 # PointerToSymbolTable
                   0,                 # NumberOfSymbols
                   0xF0,              # SizeOfOptionalHeader (240)
                   0x0022)            # Characteristics (executable, large-address-aware)

# Optional header PE32+ (240B)
opt = bytearray(240)
struct.pack_into("<H",  opt, 0,  0x20B)              # Magic
opt[2] = 14; opt[3] = 0                                # linker ver
struct.pack_into("<I", opt, 4,  sec_vsize)            # SizeOfCode
struct.pack_into("<I", opt, 16, labels["_start"])     # AddressOfEntryPoint
struct.pack_into("<I", opt, 20, VBASE)               # BaseOfCode
struct.pack_into("<Q", opt, 24, IMAGEBASE)           # ImageBase (8B)
struct.pack_into("<I", opt, 32, 0x1000)              # SectionAlignment
struct.pack_into("<I", opt, 36, 0x200)               # FileAlignment
struct.pack_into("<H", opt, 40, 4)                   # OS major
struct.pack_into("<H", opt, 48, 4)                   # Subsystem major
struct.pack_into("<I", opt, 56, size_of_image)       # SizeOfImage
struct.pack_into("<I", opt, 60, 0x400)               # SizeOfHeaders
struct.pack_into("<H", opt, 68, 3)                   # Subsystem = CUI
struct.pack_into("<Q", opt, 72, 0x100000)            # SizeOfStackReserve
struct.pack_into("<Q", opt, 80, 0x1000)              # SizeOfStackCommit
struct.pack_into("<Q", opt, 88, 0x100000)            # SizeOfHeapReserve
struct.pack_into("<Q", opt, 96, 0x1000)              # SizeOfHeapCommit
struct.pack_into("<I", opt, 108, 16)                 # NumberOfRvaAndSizes
dd = bytearray(16 * 8)
struct.pack_into("<II", dd, 1*8,  labels["imp_desc"], 40)      # Import
struct.pack_into("<II", dd, 5*8,  labels["reloc"],    len(reloc_block))  # Base Reloc
# CLR (index 14) left 0
opt[112:112+128] = dd

# Section header (40B)
sechdr = bytearray(40)
sechdr[0:8] = b".text\x00\x00\x00"
struct.pack_into("<I", sechdr, 8,  sec_vsize)        # VirtualSize
struct.pack_into("<I", sechdr, 12, VBASE)            # VirtualAddress
struct.pack_into("<I", sechdr, 16, sec_rsize)        # SizeOfRawData
struct.pack_into("<I", sechdr, 20, FBASE)            # PointerToRawData
struct.pack_into("<I", sechdr, 36, 0x60000020)       # CODE|EXECUTE|READ

hdr = dos + b"PE\x00\x00" + coff + bytes(opt) + bytes(sechdr)
hdr = hdr + b"\x00" * (FBASE - len(hdr))              # pad to 0x400
out = hdr + bytes(text)

out_path = sys.argv[1] if len(sys.argv) > 1 else "sfs_files/hello64.exe"
os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, "wb") as f:
    f.write(out)
print(f"wrote {out_path} ({len(out)} bytes)")
print(f"  entry RVA = 0x{labels['_start']:x}, SizeOfImage = 0x{size_of_image:x}")
print(f"  import desc RVA = 0x{labels['imp_desc']:x}, reloc RVA = 0x{labels['reloc']:x}")
print(f"  reloc_probe RVA = 0x{labels['reloc_probe']:x} (DIR64 target)")
