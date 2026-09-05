#!/usr/bin/env python3
# =====================================================================
#  make_test_exe.py  -  build a genuine PE32 (i386) executable from a
#                       flat NASM source, without any mingw toolchain.
# ---------------------------------------------------------------------
#  Why this exists
#  ---------------
#  The NexOS Win32 subsystem (win32.cpp) is a real PE32 loader: it maps
#  sections, applies base relocations and binds the import table before
#  jumping to the entry point.  To test it we need a real .exe, but the
#  WSL build environment has no i686-w64-mingw32 cross compiler.  So we
#  assemble a flat binary with NASM and wrap it in hand-built PE headers.
#
#  The interesting part: base relocations
#  --------------------------------------
#  `nasm -f bin` emits no relocation information, yet the loader refuses
#  to run an image that has to be rebased and has no .reloc section (and
#  it always has to be rebased - kmalloc never returns 0x00400000).
#
#  Trick: assemble the *same* source twice with two different IMAGEBASE
#  values that differ by DELTA.  Every 4-byte slot whose little-endian
#  value differs by exactly DELTA is, by construction, an absolute
#  address that needs a type-3 fixup.  Nothing else in the image can
#  change, because the only variable is IMAGEBASE.
#
#  Adjacent absolute dwords cannot produce false positives: with
#  DELTA = 0x00100000 only byte index 2 of a dword changes, so a
#  mis-aligned window differs by 0x10, 0x1000 or 0x10000000 - never by
#  DELTA itself.
#
#  Usage:  make_test_exe.py <source.asm> <output.exe>
# =====================================================================
import os
import re
import struct
import subprocess
import sys

# ---------------------------------------------------------------------
#  Emulated DLL exports (must stay in sync with the tables in win32.cpp)
# ---------------------------------------------------------------------
DLL_EXPORTS = {
    "KERNEL32.dll": [
        "GetLastError", "SetLastError", "GetStdHandle", "WriteConsoleA",
        "WriteFile", "ExitProcess", "OutputDebugStringA", "GetModuleHandleA",
        "LoadLibraryA", "FreeLibrary", "GetProcAddress", "GetModuleFileNameA",
        "GetProcessHeap", "HeapAlloc", "HeapFree", "VirtualAlloc",
        "VirtualFree", "LocalAlloc", "LocalFree", "GlobalAlloc", "GlobalFree",
        "GetCommandLineA", "GetEnvironmentVariableA", "SetEnvironmentVariableA",
        "ExpandEnvironmentStringsA", "GetComputerNameA", "GetComputerNameExA",
        "GetSystemDirectoryA", "GetWindowsDirectoryA", "GetTempPathA",
        "GetFullPathNameA", "GetFileAttributesA", "GetDriveTypeA",
        "GetLogicalDrives", "FindFirstFileA", "FindNextFileA", "FindClose",
        "SetFilePointer", "GetTickCount",
        "Sleep", "GetLocalTime", "GetSystemTime", "GetSystemInfo",
        "GetVersion", "GetVersionExA", "GetACP", "GetOEMCP",
        "GetCurrentProcessId", "GetCurrentProcess", "GetCurrentThreadId",
        "IsProcessorFeaturePresent", "InitializeCriticalSection",
        "EnterCriticalSection", "LeaveCriticalSection", "DeleteCriticalSection",
        "SetUnhandledExceptionFilter", "lstrlenA", "lstrcpyA", "lstrcatA",
        "lstrcmpA", "lstrcmpiA", "CreateFileA", "ReadFile", "GetFileSize",
        "CloseHandle", "MultiByteToWideChar", "WideCharToMultiByte",
    ],
    "USER32.dll": [
        "RegisterClassA", "RegisterClassExA", "UnregisterClassA",
        "CreateWindowExA", "DestroyWindow", "ShowWindow", "UpdateWindow",
        "InvalidateRect", "GetClientRect", "GetWindowRect", "SetWindowTextA",
        "IsWindow", "IsWindowVisible", "GetDesktopWindow", "GetForegroundWindow",
        "GetWindowTextA", "GetClassNameA", "SetFocus", "GetFocus",
        "GetCursorPos", "SetCursorPos", "ClientToScreen", "ScreenToClient",
        "BeginPaint", "EndPaint", "GetDC", "ReleaseDC", "FillRect",
        "DrawTextA", "MessageBoxA", "MessageBeep", "LoadIconA", "LoadCursorA",
        "GetMessageA", "PeekMessageA", "TranslateMessage", "DispatchMessageA",
        "PostQuitMessage", "DefWindowProcA", "PostMessageA", "SendMessageA",
        "GetSystemMetrics", "wsprintfA",
        "CreatePopupMenu", "AppendMenuA", "InsertMenuA", "DeleteMenu",
        "RemoveMenu", "DestroyMenu", "GetMenuItemCount", "GetMenuStringA",
        "EnableMenuItem", "CheckMenuItem", "TrackPopupMenu",
    ],
    "GDI32.dll": [
        "CreateSolidBrush", "CreatePen", "DeleteObject", "GetStockObject",
        "SelectObject", "SetTextColor", "SetBkColor", "SetBkMode", "TextOutA",
        "Rectangle", "Ellipse", "MoveToEx", "LineTo", "CreateFontA",
        "GetTextMetricsA", "GetTextExtentPoint32A", "SetPixel",
    ],
    "ADVAPI32.dll": [
        "RegOpenKeyExA", "RegOpenKeyA", "RegCreateKeyExA", "RegCloseKey",
        "RegQueryValueExA", "RegSetValueExA", "RegDeleteValueA",
        "RegDeleteKeyA", "RegEnumKeyExA", "RegEnumValueA", "RegQueryInfoKeyA",
        "GetUserNameA",
    ],
    "msvcrt.dll": [
        "puts", "putchar", "printf", "malloc", "free", "exit",
        "strlen", "strcpy", "strcmp", "memset", "memcpy",
    ],
}

IMAGEBASE_A = 0x00400000
DELTA = 0x00100000
IMAGEBASE_B = IMAGEBASE_A + DELTA

SECT_ALIGN = 0x1000
FILE_ALIGN = 0x200
HDR_SIZE = 0x400
TEXT_RVA = 0x1000


def align_up(v, a):
    return (v + a - 1) & ~(a - 1)


def which_dll(fn):
    for dll, names in DLL_EXPORTS.items():
        if fn in names:
            return dll
    return None


# ---------------------------------------------------------------------
#  Import directory builder
# ---------------------------------------------------------------------
class ImportDir:
    """Builds a PE import directory and reports the RVA of every IAT slot."""

    def __init__(self, groups, base_rva):
        # groups: ordered list of (dll_name, [func, ...])
        self.base = base_rva
        self.groups = groups

        ndesc = len(groups) + 1
        off = ndesc * 20                       # import descriptors

        int_off = {}
        for dll, fns in groups:
            int_off[dll] = off
            off += (len(fns) + 1) * 4
        iat_off = {}
        for dll, fns in groups:
            iat_off[dll] = off
            off += (len(fns) + 1) * 4
        self.iat_start = iat_off[groups[0][0]]

        # hint/name entries (WORD aligned)
        name_off = {}
        for dll, fns in groups:
            for fn in fns:
                name_off[(dll, fn)] = off
                off += 2 + len(fn) + 1
                off = align_up(off, 2)
        dll_off = {}
        for dll, _ in groups:
            dll_off[dll] = off
            off += len(dll) + 1
            off = align_up(off, 2)

        self.size = align_up(off, 4)
        blob = bytearray(self.size)

        for i, (dll, fns) in enumerate(groups):
            struct.pack_into("<IIIII", blob, i * 20,
                             base_rva + int_off[dll],   # OriginalFirstThunk
                             0,                          # TimeDateStamp
                             0,                          # ForwarderChain
                             base_rva + dll_off[dll],    # Name
                             base_rva + iat_off[dll])    # FirstThunk
            for k, fn in enumerate(fns):
                thunk = base_rva + name_off[(dll, fn)]
                struct.pack_into("<I", blob, int_off[dll] + k * 4, thunk)
                struct.pack_into("<I", blob, iat_off[dll] + k * 4, thunk)
            struct.pack_into("<I", blob, int_off[dll] + len(fns) * 4, 0)
            struct.pack_into("<I", blob, iat_off[dll] + len(fns) * 4, 0)

        for (dll, fn), o in name_off.items():
            struct.pack_into("<H", blob, o, 0)
            blob[o + 2:o + 2 + len(fn)] = fn.encode("ascii")
        for dll, o in dll_off.items():
            blob[o:o + len(dll)] = dll.encode("ascii")

        self.blob = bytes(blob)
        self.iat_rva = {}
        for dll, fns in groups:
            for k, fn in enumerate(fns):
                self.iat_rva[fn] = base_rva + iat_off[dll] + k * 4
        self.iat_size = sum((len(f) + 1) * 4 for _, f in groups)


# ---------------------------------------------------------------------
#  NASM driver
# ---------------------------------------------------------------------
def write_imports_inc(path, imagebase, imp):
    lines = [
        "; ------------------------------------------------------------",
        "; imports.inc - GENERATED by tools/make_test_exe.py, do not edit",
        "; ------------------------------------------------------------",
        "%%define IMAGEBASE 0x%08X" % imagebase,
        "",
    ]
    for fn in sorted(imp.iat_rva):
        # NOTE: the space before '(' is mandatory - without it NASM would
        # read the line as a *function-like* macro definition.
        name = ("IMP_" + fn).ljust(32)
        lines.append("%%define %s (IMAGEBASE + 0x%08X)" % (name, imp.iat_rva[fn]))
    lines.append("")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines))


def assemble(src, incdir, out):
    r = subprocess.run(["nasm", "-f", "bin", "-I", incdir + os.sep, src, "-o", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("nasm failed on %s" % src)
    return open(out, "rb").read()


# ---------------------------------------------------------------------
#  Relocation discovery (dual-base diff)
# ---------------------------------------------------------------------
def find_relocs(a, b, delta):
    if len(a) != len(b):
        raise SystemExit("the two assembler passes produced different sizes "
                         "(%d vs %d) - the source must not depend on "
                         "IMAGEBASE in a size-changing way" % (len(a), len(b)))
    sites = []
    for i in range(len(a) - 3):
        va = int.from_bytes(a[i:i + 4], "little")
        vb = int.from_bytes(b[i:i + 4], "little")
        if ((vb - va) & 0xFFFFFFFF) == delta:
            sites.append(i)
    return sites


def build_reloc_section(sites, text_rva):
    pages = {}
    for s in sites:
        rva = text_rva + s
        pages.setdefault(rva & ~0xFFF, []).append(rva & 0xFFF)
    out = bytearray()
    for page in sorted(pages):
        entries = sorted(pages[page])
        if len(entries) % 2:
            entries = entries + [None]          # pad with a type-0 entry
        size = 8 + len(entries) * 2
        out += struct.pack("<II", page, size)
        for e in entries:
            out += struct.pack("<H", 0 if e is None else ((3 << 12) | e))
    return bytes(out)


# ---------------------------------------------------------------------
#  PE writer
# ---------------------------------------------------------------------
DOS_STUB = (
    b"\x0E\x1F\xBA\x0E\x00\xB4\x09\xCD\x21\xB8\x01\x4C\xCD\x21"
    b"This program cannot be run in DOS mode.\r\r\n$"
)


def build_pe(text, idata, reloc, idata_rva, reloc_rva, entry_rva):
    sections = [
        (b".text",  TEXT_RVA,   text,   0xE0000060),
        (b".idata", idata_rva,  idata,  0xC0000040),
        (b".reloc", reloc_rva,  reloc,  0x42000040),
    ]

    # file offsets
    raw = {}
    off = HDR_SIZE
    for name, rva, data, _ in sections:
        raw[name] = off
        off += align_up(len(data), FILE_ALIGN)
    total_file = off

    size_of_image = align_up(sections[-1][1] + len(sections[-1][2]), SECT_ALIGN)

    img = bytearray(total_file)

    # ---- DOS header ----
    img[0:2] = b"MZ"
    struct.pack_into("<H", img, 0x02, 0x90)          # bytes on last page
    struct.pack_into("<H", img, 0x04, 0x03)          # pages
    struct.pack_into("<H", img, 0x08, 0x04)          # header paragraphs
    struct.pack_into("<H", img, 0x0A, 0xFFFF)
    struct.pack_into("<H", img, 0x0C, 0xFFFF)
    struct.pack_into("<H", img, 0x10, 0xB8)
    struct.pack_into("<H", img, 0x18, 0x40)          # reloc table offset
    struct.pack_into("<I", img, 0x3C, 0x80)          # e_lfanew
    img[0x40:0x40 + len(DOS_STUB)] = DOS_STUB

    nt = 0x80
    img[nt:nt + 4] = b"PE\0\0"

    # ---- COFF header ----
    struct.pack_into("<HHIIIHH", img, nt + 4,
                     0x014C,                 # Machine i386
                     len(sections),          # NumberOfSections
                     0x66B00000,             # TimeDateStamp
                     0, 0,                   # symbol table
                     0xE0,                   # SizeOfOptionalHeader
                     0x0102)                 # EXECUTABLE_IMAGE | 32BIT_MACHINE

    opt = nt + 24
    size_code = align_up(len(text), FILE_ALIGN)
    size_data = align_up(len(idata), FILE_ALIGN) + align_up(len(reloc), FILE_ALIGN)

    struct.pack_into("<HBBIIIIII", img, opt,
                     0x010B, 14, 0,
                     size_code, size_data, 0,
                     entry_rva, TEXT_RVA, idata_rva)
    struct.pack_into("<IIIHHHHHHIIIIHH", img, opt + 28,
                     IMAGEBASE_A, SECT_ALIGN, FILE_ALIGN,
                     4, 0,        # OS version
                     1, 0,        # image version
                     4, 0,        # subsystem version
                     0,           # Win32VersionValue
                     size_of_image, HDR_SIZE,
                     0,           # CheckSum
                     2,           # Subsystem = WINDOWS_GUI
                     0)           # DllCharacteristics
    struct.pack_into("<IIIIII", img, opt + 72,
                     0x00100000, 0x1000,     # stack reserve / commit
                     0x00100000, 0x1000,     # heap  reserve / commit
                     0,                      # LoaderFlags
                     16)                     # NumberOfRvaAndSizes

    dd = opt + 96
    struct.pack_into("<II", img, dd + 1 * 8, idata_rva, len(idata))     # import
    struct.pack_into("<II", img, dd + 5 * 8, reloc_rva, len(reloc))     # basereloc

    # ---- section table ----
    sh = opt + 0xE0
    for i, (name, rva, data, flags) in enumerate(sections):
        e = sh + i * 40
        img[e:e + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIII", img, e + 8,
                         len(data), rva,
                         align_up(len(data), FILE_ALIGN), raw[name])
        struct.pack_into("<I", img, e + 36, flags)

    # ---- section payloads ----
    for name, rva, data, _ in sections:
        img[raw[name]:raw[name] + len(data)] = data

    return bytes(img)


# ---------------------------------------------------------------------
def main():
    if len(sys.argv) < 3:
        print("Usage: %s <source.asm> <output.exe>" % sys.argv[0])
        return 1
    src = os.path.abspath(sys.argv[1])
    out = os.path.abspath(sys.argv[2])
    here = os.path.dirname(src)
    inc = os.path.join(here, "imports.inc")

    # ---- which emulated APIs does the program import? ----
    text_src = open(src, "r", errors="replace").read()
    wanted = sorted({w[4:] for w in re.findall(r"IMP_[A-Za-z0-9_]+", text_src)})
    groups, unknown = [], []
    for dll in DLL_EXPORTS:
        fns = [f for f in wanted if which_dll(f) == dll]
        if fns:
            groups.append((dll, fns))
    for f in wanted:
        if which_dll(f) is None:
            unknown.append(f)
    if unknown:
        print("ERROR: these imports are not emulated by win32.cpp: %s"
              % ", ".join(unknown))
        return 1

    print("Imports required by %s:" % os.path.basename(src))
    for dll, fns in groups:
        print("  %-14s %d  (%s)" % (dll, len(fns), ", ".join(fns)))

    tmp_a = out + ".pass_a.bin"
    tmp_b = out + ".pass_b.bin"

    # ---- pass 1: provisional .idata placement, learn the code size ----
    idata_rva = 0x8000
    imp = ImportDir(groups, idata_rva)
    write_imports_inc(inc, IMAGEBASE_A, imp)
    text_a = assemble(src, here, tmp_a)

    # ---- pass 2: put .idata right behind the real code ----
    idata_rva = align_up(TEXT_RVA + len(text_a), SECT_ALIGN)
    imp = ImportDir(groups, idata_rva)
    write_imports_inc(inc, IMAGEBASE_A, imp)
    text_a = assemble(src, here, tmp_a)
    if align_up(TEXT_RVA + len(text_a), SECT_ALIGN) != idata_rva:
        return print("ERROR: code size did not converge") or 1

    # ---- pass 3: same layout, shifted image base -> relocation sites ----
    write_imports_inc(inc, IMAGEBASE_B, imp)
    text_b = assemble(src, here, tmp_b)
    sites = find_relocs(text_a, text_b, DELTA)
    print("Code size      : %d bytes" % len(text_a))
    print("Absolute refs  : %d (type-3 fixups)" % len(sites))
    if not sites:
        print("ERROR: no relocations found - the .reloc section would be empty "
              "and the loader would refuse to rebase the image")
        return 1

    # restore imports.inc to the real base so the file on disk is the truth
    write_imports_inc(inc, IMAGEBASE_A, imp)

    reloc_rva = align_up(idata_rva + imp.size, SECT_ALIGN)
    reloc = build_reloc_section(sites, TEXT_RVA)

    img = build_pe(text_a, imp.blob, reloc, idata_rva, reloc_rva, TEXT_RVA)
    with open(out, "wb") as f:
        f.write(img)

    for t in (tmp_a, tmp_b):
        try:
            os.remove(t)
        except OSError:
            pass

    print("Layout         : .text 0x%04X (%d)  .idata 0x%04X (%d)  "
          ".reloc 0x%04X (%d)" % (TEXT_RVA, len(text_a), idata_rva,
                                  imp.size, reloc_rva, len(reloc)))
    print("PE32 written   : %s (%d bytes)" % (out, len(img)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
