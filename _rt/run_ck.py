import pathlib

# check_k64_fit.sh derives the repo root from its own location
# (cd "$(dirname "$0")/.."), so the LF copy must live one level below the root.
src = pathlib.Path("tools/check_k64_fit.sh").read_bytes()
src = src.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
out = pathlib.Path("build/ck.sh")
out.write_bytes(src)
print("wrote %s (%d bytes)" % (out, len(src)))
