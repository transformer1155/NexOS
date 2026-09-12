#!/usr/bin/env python3
"""gen_gb2312_list.py - Emit the exact Unicode codepoint set of GB2312.

GB2312 is a 94x94 double-byte set.  We enumerate the full code space and keep
every byte pair Python can decode, which yields the authoritative 7445-char set
(6763 hanzi + ~682 symbols/punctuation/kana) in ascending Unicode order.

Output: build/gb2312_uni.txt  (one decimal codepoint per line, ascending)
"""
import os, sys

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join('build', 'gb2312_uni.txt')
    seen = set()
    for hi in range(0xA1, 0xF8):          # GB2312 high byte  A1..F7
        for lo in range(0xA1, 0xFF):       # GB2312 low  byte  A1..FE
            try:
                ch = bytes([hi, lo]).decode('gb2312')
            except Exception:
                continue
            if len(ch) != 1:
                continue
            seen.add(ord(ch))
    cps = sorted(seen)
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'w', encoding='ascii') as f:
        for cp in cps:
            f.write('%d\n' % cp)
    print("GB2312: %d unique codepoints -> %s" % (len(cps), out))

if __name__ == '__main__':
    main()
