#!/usr/bin/env python3
"""Run _verify_boot.py via WSL"""
import subprocess, sys, os

result = subprocess.run(
    ['wsl', '-e', 'bash', '-lc', 'cd /mnt/d/MyOS/bootloader && python3 tools/_verify_boot.py'],
    capture_output=True, text=True, timeout=120,
    cwd=r'D:\MyOS\bootloader'
)
print("=== STDOUT ===")
print(result.stdout)
print("=== STDERR ===")
print(result.stderr)
print(f"=== Return code: {result.returncode} ===")
