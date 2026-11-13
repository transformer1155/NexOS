# =====================================================================
#  split_app_sources.py -- turn the multi-app source files into one
#                           file per application.
# ---------------------------------------------------------------------
#  WHY
#  ----
#  csharp/apps/Shell/Apps.cs holds SEVEN managed applications in one
#  file (calculator, about, task manager, file explorer, control panel,
#  memory optimiser, notepad).  MediaPlayers.cs holds two more plus the
#  shared "Media" header-probing helper.
#
#  When every standalone .mex compiled the whole file, all seven
#  applications shipped inside EVERY image: seven copies of the same
#  100 KB of code, ~750 KB of dead weight in the SFS volume, which only
#  has ~430 KB to spare.  Each application must therefore carry ONLY
#  its own class.
#
#  WHAT
#  ---
#  Reads the listed sources, splits them at class boundaries (brace
#  matched, so nested types stay with their parent) and writes
#
#      csharp/apps/standalone/parts/<ClassName>.cs   -- one app class
#      csharp/apps/standalone/parts/_Shared_Media.cs -- helper classes
#
#  The shared helpers are emitted once and referenced by every app that
#  needs them.  The generated files are checked in (the generator writes
#  them), so a plain `make` never has to run Python for the split.
#
#  Run:  python tools/split_app_sources.py
# =====================================================================
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "csharp", "apps", "standalone", "parts")

# file -> list of top-level type names that are "shared" (helpers) rather
# than applications.  A shared type is emitted into _Shared_<file>.cs and
# referenced by every application built from that file.
SHARED = {
    "csharp/apps/Shell/MediaPlayers.cs": ["Media"],
}

# file -> the application classes it contains, in order.
FILES = [
    ("csharp/apps/Shell/Apps.cs", [
        "CalculatorApp", "AboutApp", "TaskManagerApp", "FileExplorerApp",
        "ControlPanelApp", "MemOptimizerApp", "NotepadApp",
    ]),
    ("csharp/apps/Shell/MediaPlayers.cs", [
        "VideoPlayerApp", "AudioPlayerApp",
    ]),
]


def read(path):
    with open(os.path.join(ROOT, path), "r", encoding="utf-8") as f:
        return f.read()


def find_class_span(src, name):
    """Return (start, end) of the top-level class `name` in src.

    `start` is the index of the 'class' keyword's line start; the
    preceding comment block is included by the caller.  `end` is the
    index just past the closing brace of the class body.
    """
    m = re.search(r"^[ \t]*(?:public|internal|sealed|abstract|partial|static)*"
                  r"[ \t]*class[ \t]+" + re.escape(name) + r"\b", src, re.M)
    if not m:
        return None
    # Find the opening brace of the class body.
    i = src.find("{", m.end())
    if i < 0:
        return None
    depth = 0
    j = i
    n = len(src)
    in_str = False
    in_chr = False
    in_line = False
    in_block = False
    while j < n:
        c = src[j]
        nxt = src[j + 1] if j + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
        elif in_block:
            if c == "*" and nxt == "/":
                in_block = False
                j += 1
        elif in_str:
            if c == "\\":
                j += 1
            elif c == '"':
                in_str = False
        elif in_chr:
            if c == "\\":
                j += 1
            elif c == "'":
                in_chr = False
        else:
            if c == "/" and nxt == "/":
                in_line = True
                j += 1
            elif c == "/" and nxt == "*":
                in_block = True
                j += 1
            elif c == '"':
                in_str = True
            elif c == "'":
                in_chr = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return (m.start(), j + 1)
        j += 1
    return None


def leading_comment_start(src, idx):
    """Walk back over the comment block that introduces a type."""
    lines = src[:idx].split("\n")
    # Drop the declaration line itself.
    k = len(lines) - 1
    out = []
    while k > 0:
        prev = lines[k - 1].strip()
        if prev.startswith("//"):
            out.insert(0, lines[k - 1])
            k -= 1
            continue
        break
    if not out:
        return idx
    start = idx - sum(len(l) + 1 for l in out)
    return max(0, start)


def namespace_open(src):
    """Index of the '{' that opens the file-level namespace."""
    m = re.search(r"^namespace[ \t]+[A-Za-z0-9_.]+[ \t]*$", src, re.M)
    if not m:
        raise SystemExit("no namespace declaration found")
    return src.find("{", m.end())


def file_header(src):
    """Everything before the namespace block: usings + file comment.

    The trailing "namespace X" line is KEPT, because wrap() emits
    "namespace X\n{" itself -- including it here would duplicate it.
    """
    i = namespace_open(src)
    return src[:i]


def wrap(header, body):
    """Put `body` inside the namespace block of `header`."""
    head = header.rstrip()
    # Drop a trailing "namespace X" line: wrap() supplies its own.
    lines = head.split("\n")
    while lines and (not lines[-1].strip() or
                     lines[-1].strip().startswith("namespace ")):
        lines.pop()
    head = "\n".join(lines)
    return head + "\n\nnamespace NexOS.Forms\n{\n" + body.rstrip() + "\n}\n"


def main():
    os.makedirs(OUT, exist_ok=True)
    written = []

    for rel, classes in FILES:
        src = read(rel)
        header = file_header(src)
        shared_names = SHARED.get(rel, [])

        shared_body = []
        app_body = {}

        for name in shared_names + classes:
            span = find_class_span(src, name)
            if not span:
                raise SystemExit("class %s not found in %s" % (name, rel))
            start, end = span
            start = leading_comment_start(src, start)
            block = src[start:end]
            if name in shared_names:
                shared_body.append(block)
            else:
                app_body[name] = block

        if shared_body:
            fname = "_Shared_" + os.path.basename(rel).replace(".cs", "") + ".cs"
            path = os.path.join(OUT, fname)
            with open(path, "w", encoding="utf-8") as f:
                f.write("// GENERATED by tools/split_app_sources.py -- do not edit.\n")
                f.write("// Shared helper types from %s.\n" % rel)
                f.write(wrap(header, "\n\n".join(shared_body)))
            written.append(fname)

        for name, block in app_body.items():
            fname = name + ".cs"
            path = os.path.join(OUT, fname)
            with open(path, "w", encoding="utf-8") as f:
                f.write("// GENERATED by tools/split_app_sources.py -- do not edit.\n")
                f.write("// Extracted from %s so that every standalone .mex carries\n" % rel)
                f.write("// only its own application code.\n")
                f.write(wrap(header, block))
            written.append(fname)

    for n in sorted(written):
        p = os.path.join(OUT, n)
        print("%8d  %s" % (os.path.getsize(p), n))
    print("%d files in %s" % (len(written), OUT))


if __name__ == "__main__":
    main()
