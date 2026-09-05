#!/usr/bin/env python3
"""
whodefines.py - 给一批未定义符号找上游定义点。

link_audit.sh 告诉我们"还缺谁"，这个工具回答"上游把它定义在哪个 .c 里"。
有了它才能判断一个缺失符号是：
  (a) 我们少编了某个源文件      -> 加进 Makefile 的清单
  (b) 那个文件编了但整段被宏关掉 -> 补 -D
  (c) 上游也没有 .c 定义         -> 平台相关 / 需要写桩

用法:
    python3 tools/whodefines.py build/link_missing.txt
    python3 tools/whodefines.py mono_cpu_count mono_dl_open
"""
import os
import re
import sys
import collections

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "..", "vendor", "mono")
ROOT = os.path.normpath(ROOT)

SCAN_DIRS = ["utils", "metadata", "mini", "sgen", "eglib", "arch"]


def load_sources():
    """返回 [(相对路径, 全文)]。只看 .c/.h，.h 用来识别 inline 定义。"""
    out = []
    for d in SCAN_DIRS:
        base = os.path.join(ROOT, d)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [x for x in dirnames
                           if x not in ("tests", "unit-tests", ".git")]
            for fn in filenames:
                if not fn.endswith((".c", ".h")):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    with open(p, "r", encoding="utf-8", errors="replace") as f:
                        out.append((os.path.relpath(p, ROOT).replace("\\", "/"),
                                    f.read()))
                except OSError:
                    pass
    return out


_CACHE = {}


def make_matcher(sym):
    """
    分级匹配。关键洞察：Mono 的代码风格里，函数【定义】的函数名一定顶格
    在第 0 列（返回类型单独占上一行），而函数【调用】永远是缩进的。
    所以 `^SYM\\s*\\(` 是极强的定义信号，不会把调用点误判成定义点。
    分级是必要的——不分级就会按字母序把 mono_thread_info_resume 记到
    metadata/threads.c（那里只是调用），而不是 utils/mono-threads.c。

      tier 0  ^SYM(            顶格函数定义，Mono 风格，最可信
      tier 1  ^类型 ... SYM(   同行写返回类型的定义，且不以 ; 收尾
      tier 2  ^类型 SYM = / ;  顶格全局变量定义（排除 extern/typedef）
    """
    if sym in _CACHE:
        return _CACHE[sym]
    esc = re.escape(sym)
    m = (
        re.compile(r"^" + esc + r"[ \t]*\(", re.M),
        re.compile(r"^[A-Za-z_][\w \t\*]*[ \t\*]" + esc + r"[ \t]*\([^;]*$",
                   re.M),
        re.compile(r"^[A-Za-z_][\w \t\*]*[ \t\*]" + esc +
                   r"[ \t]*(\[[^\]]*\])?[ \t]*(=|;)", re.M),
    )
    _CACHE[sym] = m
    return m


def find(sym, sources):
    tiers = make_matcher(sym)
    hits = []
    for path, text in sources:
        if sym not in text:
            continue
        for t, rx in enumerate(tiers):
            ok = False
            for mo in rx.finditer(text):
                line = text[text.rfind("\n", 0, mo.start()) + 1: mo.end()]
                if "extern" in line or "typedef" in line:
                    continue
                ok = True
                break
            if ok:
                hits.append((t, path))
                break
    # 先按可信度，再 .c 优先，最后字母序
    hits.sort(key=lambda h: (h[0], not h[1].endswith(".c"), h[1]))
    return [(p, "t%d" % t) for t, p in hits]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    if len(args) == 1 and os.path.isfile(args[0]):
        syms = [l.strip() for l in open(args[0]) if l.strip()]
    else:
        syms = args

    sources = load_sources()
    sys.stderr.write("scanned %d files under %s\n" % (len(sources), ROOT))

    by_file = collections.defaultdict(list)
    nowhere = []
    for s in syms:
        hits = find(s, sources)
        cs = [h for h in hits if h[0].endswith(".c")]
        if cs:
            by_file[cs[0][0]].append(s)
        elif hits:
            by_file[hits[0][0] + " (header-only)"].append(s)
        else:
            nowhere.append(s)

    print("=== missing symbols grouped by upstream definition site ===")
    for f in sorted(by_file, key=lambda k: (-len(by_file[k]), k)):
        print("\n%-46s  %d" % (f, len(by_file[f])))
        for s in sorted(by_file[f]):
            print("    " + s)
    if nowhere:
        print("\n%-46s  %d" % ("<< no definition found in tree >>",
                               len(nowhere)))
        for s in sorted(nowhere):
            print("    " + s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
