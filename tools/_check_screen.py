#!/usr/bin/env python3
"""Analyze screendump PPM to see what's on screen"""
import subprocess, sys

# Convert PPM to PNG for viewing, and extract text
result = subprocess.run(
    ['wsl', '-e', 'bash', '-lc', 
     'cd /mnt/d/MyOS/bootloader && '
     'convert /tmp/NexOS_screen4.ppm /tmp/NexOS_screen4.png 2>&1 || '
     'python3 -c "'
     'from PIL import Image; '
     'img = Image.open(\"/tmp/NexOS_screen4.ppm\"); '
     'img.save(\"/tmp/NexOS_screen4.png\")" 2>&1 || '
     'echo "no PIL, trying raw analysis"'],
    capture_output=True, text=True, timeout=60,
    cwd=r'D:\MyOS\bootloader'
)
print("Convert:", result.stdout[:500])

# Read PPM header to understand dimensions
result2 = subprocess.run(
    ['wsl', '-e', 'bash', '-lc',
     'head -5 /tmp/NexOS_screen4.ppm'],
    capture_output=True, text=True, timeout=30,
    cwd=r'D:\MyOS\bootloader'
)
print("\nPPM header:", result2.stdout)

# Try raw analysis of the PPM data
result3 = subprocess.run(
    ['wsl', '-e', 'bash', '-lc',
     'python3 -c "'
     'data = open(\"/tmp/NexOS_screen4.ppm\", \"rb\").read(); '
     'print(f\"Size: {len(data)} bytes\"); '
     '# Find header end (after P6\\nW H\\nMAX\\n)'
     'header_end = data.find(b\"\\n\", data.find(b\"\\n\", data.find(b\"\\n\")+1)+1)+1; '
     'print(f\"Header end: {header_end}\"); '
     'pixels = data[header_end:]; '
     'print(f\"Pixel data: {len(pixels)} bytes\"); '
     '# Check if non-zero pixels'
     'nonzero = sum(1 for b in pixels if b != 0); '
     'print(f\"Non-zero bytes: {nonzero} ({nonzero*100//len(pixels)}%)\"); '
     '"'],
    capture_output=True, text=True, timeout=30,
    cwd=r'D:\MyOS\bootloader'
)
print(result3.stdout)
