#!/usr/bin/env python3
"""Confirm both .COM programs are packed into the shipped SFS volume."""
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
data = open(os.path.join(root, "build", "sfs.img"), "rb").read()

checks = [b"hello.com", b"fileio.com", b"Hello DOS", b"ramfs-overlay-works"]
print(f"sfs.img = {len(data)} bytes\n")
for c in checks:
    i = data.find(c)
    print(f"  {c.decode():24} {'offset %d' % i if i >= 0 else 'NOT FOUND'}")