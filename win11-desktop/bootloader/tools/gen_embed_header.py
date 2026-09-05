#!/usr/bin/env python3
"""Generate a C header file containing a binary file as an embedded array."""
import sys

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_binary> <output_header>")
        sys.exit(1)

    in_path = sys.argv[1]
    out_path = sys.argv[2]

    with open(in_path, 'rb') as f:
        data = f.read()

    with open(out_path, 'w') as out:
        out.write('#ifndef KERNEL_EMBEDDED_H\n#define KERNEL_EMBEDDED_H\n\n')
        out.write(f'#define KERNEL_EMBEDDED_SIZE {len(data)}U\n\n')
        out.write('static const unsigned char kernel_embedded_data[KERNEL_EMBEDDED_SIZE] = {\n')
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
            out.write(f'  {hex_str},\n')
        out.write('};\n\n#endif\n')

    print(f'Generated {out_path}: {len(data)} bytes from {in_path}')

if __name__ == '__main__':
    main()