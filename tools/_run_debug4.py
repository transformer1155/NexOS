#!/usr/bin/env python3
import subprocess, sys
result = subprocess.run(
    ['wsl', '-e', 'bash', '-lc', 'cd /mnt/d/MyOS/bootloader && python3 tools/_debug_boot4.py'],
    capture_output=True, text=True, timeout=120,
    cwd=r'D:\MyOS\bootloader'
)
print(result.stdout)
if result.stderr:
    print("STDERR:", result.stderr)
