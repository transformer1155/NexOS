#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Insert AiDesktopKey routing into Desktop.Key (CRLF-safe)."""
import os, sys
ROOT = "/mnt/d/MyOS/bootloader"
os.chdir(ROOT)
DESK = "csharp/apps/Shell/Desktop.cs"
with open(DESK, "rb") as f:
    data = f.read()

def crlf(s):
    return s.replace("\n", "\r\n")

old = crlf("""            if (ch == -9) { if (CurrentDesktop == 0) SwitchDesktop(1); else SwitchDesktop(0); return; }
            if (renameIdx < 0) return;
""")
new = crlf("""            if (ch == -9) { if (CurrentDesktop == 0) SwitchDesktop(1); else SwitchDesktop(0); return; }
            if (CurrentDesktop == 1) { AiDesktopKey(ch); return; }
            if (renameIdx < 0) return;
""")
ob = old.encode("utf-8")
nb = new.encode("utf-8")
cnt = data.count(ob)
if cnt != 1:
    print("FAIL [key] matches=%d" % cnt); sys.exit(1)
data = data.replace(ob, nb)
with open(DESK, "wb") as f:
    f.write(data)
print("ok [key] routed desktop keys to AiDesktopKey when AI desktop active")
