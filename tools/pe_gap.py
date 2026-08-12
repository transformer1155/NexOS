#!/usr/bin/env python3
"""
pe_gap.py -- quantify the gap between a real Windows PE and the NexOS
             win64 loader.

Parses a PE32+ image's import directory (plus the delay-import directory,
which Chromium relies on heavily) and compares the required imports against
the export tables the NexOS loader actually implements in win32.cpp.

Usage:  python3 tools/pe_gap.py <file.exe> [more.exe ...]

This is deliberately dependency-free: it walks the headers by hand rather
than pulling in pefile, so it runs the same way inside WSL and on Windows.
"""
import re
import struct
import sys
import os

WIN32_CPP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "win32.cpp")

# The loader's hard ceiling, from win32.cpp:1996 / :2202.
MAXPE = 192 * 1024


def loader_exports():
    """Scrape the API names the NexOS loader can actually resolve."""
    try:
        src = open(WIN32_CPP, encoding="utf-8", errors="replace").read()
    except OSError:
        return {}
    tables = {}
    for tab, dll in [
        ("EX_KERNEL32", "kernel32.dll"),
        ("EX_USER32", "user32.dll"),
        ("EX_GDI32", "gdi32.dll"),
        ("EX_ADVAPI32", "advapi32.dll"),
        ("EX_MSVCRT", "msvcrt.dll"),
    ]:
        m = re.search(r"static const W32Export " + tab + r"\[\] = \{(.*?)\n\};", src, re.S)
        if m:
            tables[dll] = set(re.findall(r'EXP\("([^"]+)"', m.group(1)))
    return tables


class PE:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        self.ok = False
        self._parse()

    def _u16(self, o):
        return struct.unpack_from("<H", self.data, o)[0]

    def _u32(self, o):
        return struct.unpack_from("<I", self.data, o)[0]

    def _parse(self):
        d = self.data
        if d[:2] != b"MZ":
            return
        pe = self._u32(0x3C)
        if d[pe:pe + 4] != b"PE\0\0":
            return
        self.machine = self._u16(pe + 4)
        self.nsec = self._u16(pe + 6)
        opt = pe + 24
        self.magic = self._u16(opt)
        self.plus = self.magic == 0x20B
        self.nrva = self._u32(opt + (108 if self.plus else 92))
        dd = opt + (112 if self.plus else 96)
        self.dirs = [(self._u32(dd + i * 8), self._u32(dd + i * 8 + 4))
                     for i in range(min(self.nrva, 16))]
        sec = opt + self._u16(pe + 20)
        self.sections = []
        for i in range(self.nsec):
            o = sec + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsz, va, rsz, raw = struct.unpack_from("<IIII", d, o + 8)
            self.sections.append((name, va, vsz, raw, rsz))
        self.ok = True

    def off(self, rva):
        for _, va, vsz, raw, rsz in self.sections:
            if va <= rva < va + max(vsz, rsz):
                x = raw + (rva - va)
                return x if x < len(self.data) else None
        return None

    def cstr(self, off):
        if off is None:
            return None
        e = self.data.find(b"\0", off)
        return self.data[off:e].decode("latin1", "replace")

    def _walk_descriptors(self, rva, size, name_off, ilt_off, stride, delay=False):
        """Shared walker for the import and delay-import descriptor arrays."""
        out = {}
        if not rva:
            return out
        base = self.off(rva)
        if base is None:
            return out
        i = 0
        while True:
            o = base + i * stride
            if o + stride > len(self.data):
                break
            chunk = self.data[o:o + stride]
            if chunk == b"\0" * stride:
                break
            dll_rva = struct.unpack_from("<I", self.data, o + name_off)[0]
            if not dll_rva:
                break
            # Delay-import descriptors in modern toolchains still use RVAs
            # (the "V2" attribute); nothing here needs the old VA form.
            dll = self.cstr(self.off(dll_rva))
            thunk = struct.unpack_from("<I", self.data, o + ilt_off)[0]
            names = self._read_thunks(thunk)
            if dll:
                out.setdefault(dll.lower(), set()).update(names)
            i += 1
            if i > 4096:
                break
        return out

    def _read_thunks(self, rva):
        names = set()
        if not rva:
            return names
        o = self.off(rva)
        if o is None:
            return names
        w = 8 if self.plus else 4
        fmt = "<Q" if self.plus else "<I"
        ordflag = (1 << 63) if self.plus else (1 << 31)
        k = 0
        while o + w <= len(self.data):
            v = struct.unpack_from(fmt, self.data, o)[0]
            if v == 0:
                break
            if v & ordflag:
                names.add("#%d" % (v & 0xFFFF))
            else:
                n = self.cstr(self.off((v & 0x7FFFFFFF) + 2))
                if n:
                    names.add(n)
            o += w
            k += 1
            if k > 65536:
                break
        return names

    def imports(self):
        rva, size = self.dirs[1] if len(self.dirs) > 1 else (0, 0)
        return self._walk_descriptors(rva, size, name_off=12, ilt_off=0, stride=20)

    def delay_imports(self):
        rva, size = self.dirs[13] if len(self.dirs) > 13 else (0, 0)
        return self._walk_descriptors(rva, size, name_off=4, ilt_off=16, stride=32,
                                      delay=True)

    def exports(self):
        """Names in the export directory (dir 0).

        Only named exports are returned; ordinal-only entries are counted
        separately by the caller through NumberOfFunctions.  That split
        matters because ntdll exports a large block of Zw* aliases that share
        addresses with their Nt* twins.
        """
        rva, _ = self.dirs[0] if self.dirs else (0, 0)
        out = []
        if not rva:
            return out, 0
        o = self.off(rva)
        if o is None:
            return out, 0
        nfunc, nname = struct.unpack_from("<II", self.data, o + 20)
        name_rva = struct.unpack_from("<I", self.data, o + 32)[0]
        nt = self.off(name_rva)
        if nt is None:
            return out, nfunc
        for i in range(min(nname, 65536)):
            if nt + i * 4 + 4 > len(self.data):
                break
            s = self.cstr(self.off(struct.unpack_from("<I", self.data, nt + i * 4)[0]))
            if s:
                out.append(s)
        return out, nfunc


def report(path, have):
    pe = PE(path)
    print("=" * 74)
    print("FILE  %s" % path)
    if not pe.ok:
        print("  not a PE image")
        return
    size = len(pe.data)
    arch = {0x8664: "x86-64", 0x14C: "i386", 0xAA64: "arm64"}.get(pe.machine, hex(pe.machine))
    print("  size      %s bytes  (%.2f MiB)" % (f"{size:,}", size / 1048576))
    print("  machine   %s   magic %s" % (arch, "PE32+" if pe.plus else "PE32"))
    print("  loader    MAXPE = %s bytes  ->  %s"
          % (f"{MAXPE:,}",
             "FITS" if size <= MAXPE else
             "REJECTED (-6), image is %.1fx over the limit" % (size / MAXPE)))

    # ---- loader blind spots -------------------------------------------
    # win64_run understands exactly three directories: imports (1),
    # base relocations (5) and the CLR header (14, as a refusal).  Anything
    # else in the file is mapped but never acted on, so a PE that *needs*
    # one of them will fault at a point far away from the real cause.
    DIRNAME = {7: "arch", 9: "TLS callbacks", 10: "load config",
               11: "bound import", 13: "delay imports", 14: "CLR/.NET"}
    notes = []
    rel = pe.dirs[5] if len(pe.dirs) > 5 else (0, 0)
    notes.append(("reloc", "present" if rel[0] else
                  "ABSENT - loader refuses (-3) unless it lands on ImageBase"))
    for idx, label in DIRNAME.items():
        if len(pe.dirs) > idx and pe.dirs[idx][0]:
            if idx == 14:
                notes.append((label, "present - loader refuses (-3)"))
            elif idx == 13:
                notes.append((label, "present - loader IGNORES dir 13; "
                                     "first call through a delay stub faults"))
            elif idx == 9:
                notes.append((label, "present - loader never runs them; "
                                     "app sees uninitialised TLS"))
            else:
                notes.append((label, "present - ignored by loader"))
    print("  loader-fit")
    for k, v in notes:
        print("      %-14s %s" % (k, v))

    imp = pe.imports()
    dly = pe.delay_imports()
    merged = {}
    for src in (imp, dly):
        for k, v in src.items():
            merged.setdefault(k, set()).update(v)

    total = sum(len(v) for v in merged.values())
    print("  imports   %d DLL(s), %d function(s)   [%d static + %d delay-loaded]"
          % (len(merged), total,
             sum(len(v) for v in imp.values()), sum(len(v) for v in dly.values())))
    print()
    print("  %-24s %6s %6s %6s   %s" % ("DLL", "needs", "have", "MISS", "status"))
    print("  " + "-" * 66)
    grand_miss = 0
    for dll in sorted(merged, key=lambda d: -len(merged[d])):
        need = merged[dll]
        got = have.get(dll, set())
        miss = need - got
        grand_miss += len(miss)
        status = "not emulated at all" if dll not in have else \
                 ("complete" if not miss else "partial")
        print("  %-24s %6d %6d %6d   %s"
              % (dll, len(need), len(need) - len(miss), len(miss), status))
    print("  " + "-" * 66)
    print("  %-24s %6d %6d %6d" % ("TOTAL", total, total - grand_miss, grand_miss))
    cov = (total - grand_miss) / total * 100 if total else 0
    print()
    print("  API coverage: %.1f%%   (%d of %d resolvable)" % (cov, total - grand_miss, total))
    return grand_miss, total, size


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    have = loader_exports()
    n = sum(len(v) for v in have.values())
    print("NexOS win64 loader implements %d exports across %d DLLs: %s"
          % (n, len(have), ", ".join(sorted(have))))
    print()
    for p in sys.argv[1:]:
        report(p, have)
    return 0


if __name__ == "__main__":
    sys.exit(main())
