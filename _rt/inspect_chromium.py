import collections
import os
import zipfile

Z = "/mnt/e/\u8fc5\u96f7\u4e0b\u8f7d/chromium-main.zip"

print("path:", Z)
print("exists:", os.path.exists(Z))
if not os.path.exists(Z):
    # try to locate a similarly named file
    for root in ("/mnt/e", "/mnt/d", "/mnt/c"):
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            if dirpath.count(os.sep) > 4:
                dirnames[:] = []
                continue
            for fn in filenames:
                if "chromium" in fn.lower() and fn.lower().endswith(".zip"):
                    print("  candidate:", os.path.join(dirpath, fn))
    raise SystemExit(0)

print("size: %.1f MB" % (os.path.getsize(Z) / 1048576.0))

z = zipfile.ZipFile(Z)
names = z.namelist()
print("entries:", len(names))

tops = collections.Counter(n.split("/")[0] for n in names)
print("top-level (top 25):")
for name, cnt in tops.most_common(25):
    print("   %-40s %d" % (name, cnt))

print("first 30 entries:")
for n in names[:30]:
    print("   ", n)

try:
    info = z.getinfo(names[0])
    print("first entry size:", info.file_size)
except Exception as exc:  # noqa: BLE001
    print("getinfo failed:", exc)

total = sum(i.file_size for i in z.infolist())
print("uncompressed total: %.1f MB" % (total / 1048576.0))
