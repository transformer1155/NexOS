#!/usr/bin/env python3
"""从上游 Makefile.am 里抽出权威源文件清单。

为什么不用"能编过的都要"：那样得到的是一个由编译器口味决定的集合，
会同时漏掉需要额外宏才能编的文件、混进 Windows/wasm 的同名替身。
这里改成解析 automake 变量，按目标平台把条件分支求值一遍，
得到的就是上游在 Linux/x86 + SGen 配置下真正会编的那批 .c。

用法:  python3 tools/gen_srclist.py <metadata|sgen|utils>
"""
import re
import sys
import os

VENDOR = os.path.join(os.path.dirname(__file__), "..", "..", "vendor", "mono")

# 目标配置：Unix 主机、x86、SGen GC、开 ILGen、开 icall 表、
# 关 socket / 关 attach / 非 wasm / 非 MSVC。
COND = {
    "ENABLE_MSVC_ONLY": False,
    "HOST_WIN32": False,
    "HOST_WASM": False,
    "CROSS_COMPILING": True,
    "BITCODE": False,
    "WASM": False,
    "SUPPORT_SGEN": True,
    "SUPPORT_BOEHM": False,
    "DISABLE_ICALL_TABLES": False,
    "ENABLE_ILGEN": True,
    "HAVE_STATIC_ZLIB": False,
    "HAVE_SYS_ZLIB": False,
    "DISABLE_PERFCOUNTERS": False,
    "ENABLE_NETCORE": False,
    # utils/Makefile.am 用的是 CROSS_COMPILE（没有尾巴 ING），别写混。
    # 我们编的是 x86 目标、宿主也是 x86 语义，所以 !CROSS_COMPILE 为真，
    # arch_sources 走 mono-hwcap-x86.c 而不是 mono-hwcap-cross.c。
    "CROSS_COMPILE": False,
    "X86": True,
    "AMD64": False,
    "ARM": False,
    "ARM64": False,
    "MIPS": False,
    "POWERPC": False,
    "POWERPC64": False,
    "SPARC": False,
    "SPARC64": False,
    "S390X": False,
    "RISCV": False,
    "ENABLE_LLVM": False,
    "INTERNAL_LLVM": False,
    "ENABLE_DTRACE": False,
    "ENABLE_INTERPRETER": True,
    "JIT_SUPPORTED": True,
}


def parse(path):
    """把 Makefile.am 求值成 {变量名: [token,...]}。只处理 = / += / if-else-endif。"""
    variables = {}
    stack = []          # [(active, seen_true)]
    with open(path, encoding="utf-8", errors="replace") as fh:
        raw = fh.read()
    # 续行合并
    raw = raw.replace("\\\n", " ")
    for line in raw.splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        m = re.match(r"^if\s+(!?)([A-Za-z0-9_]+)", s)
        if m:
            neg, name = m.group(1) == "!", m.group(2)
            val = COND.get(name, False)
            if neg:
                val = not val
            stack.append([val, val])
            continue
        if s == "else":
            if stack:
                stack[-1][0] = not stack[-1][1]
            continue
        if s.startswith("endif"):
            if stack:
                stack.pop()
            continue
        if any(not a for a, _ in stack):
            continue
        m = re.match(r"^([A-Za-z0-9_]+)\s*(\+?=)\s*(.*)$", s)
        if m:
            name, op, rest = m.group(1), m.group(2), m.group(3)
            toks = rest.split()
            if op == "=":
                variables[name] = toks
            else:
                variables.setdefault(name, []).extend(toks)
    return variables


def expand(variables, name, seen=None):
    seen = seen or set()
    if name in seen:
        return []
    seen.add(name)
    out = []
    for tok in variables.get(name, []):
        m = re.match(r"^\$\(([A-Za-z0-9_]+)\)$", tok)
        if m:
            out.extend(expand(variables, m.group(1), seen))
        else:
            out.append(tok)
    return out


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "metadata"
    if what == "metadata":
        path = os.path.join(VENDOR, "metadata", "Makefile.am")
        target = "libmonoruntimesgen_la_SOURCES"
    elif what == "sgen":
        path = os.path.join(VENDOR, "sgen", "Makefile.am")
        target = None
    elif what == "utils":
        path = os.path.join(VENDOR, "utils", "Makefile.am")
        target = "libmonoutils_la_SOURCES"
    else:
        print("unknown: " + what, file=sys.stderr)
        return 1

    variables = parse(path)
    if target is None:
        # sgen/Makefile.am 只有一个 libmonosgen_la_SOURCES 之类的变量
        cands = [k for k in variables if k.endswith("_SOURCES")]
        target = cands[0] if cands else None
        if target is None:
            print("no *_SOURCES in " + path, file=sys.stderr)
            return 1

    files = [f for f in expand(variables, target) if f.endswith(".c")]
    # 去重保序
    seen, ordered = set(), []
    for f in files:
        if f not in seen:
            seen.add(f)
            ordered.append(f)
    for f in ordered:
        print(f)
    print("# total: %d" % len(ordered), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
