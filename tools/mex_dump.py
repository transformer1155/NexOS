#!/usr/bin/env python3
# =====================================================================
#  mex_dump.py  -  disassemble a .mex image produced by mex_pack.py
# ---------------------------------------------------------------------
#  Used to verify that metadata tokens were rewritten into flat indices
#  before blaming the kernel interpreter.
# =====================================================================
import struct, sys

OPS = {
    0x00:'nop', 0x01:'break', 0x02:'ldarg.0', 0x03:'ldarg.1', 0x04:'ldarg.2',
    0x05:'ldarg.3', 0x06:'ldloc.0', 0x07:'ldloc.1', 0x08:'ldloc.2',
    0x09:'ldloc.3', 0x0A:'stloc.0', 0x0B:'stloc.1', 0x0C:'stloc.2',
    0x0D:'stloc.3', 0x0E:'ldarg.s', 0x0F:'ldarga.s', 0x10:'starg.s',
    0x11:'ldloc.s', 0x12:'ldloca.s', 0x13:'stloc.s', 0x14:'ldnull',
    0x15:'ldc.i4.m1', 0x16:'ldc.i4.0', 0x17:'ldc.i4.1', 0x18:'ldc.i4.2',
    0x19:'ldc.i4.3', 0x1A:'ldc.i4.4', 0x1B:'ldc.i4.5', 0x1C:'ldc.i4.6',
    0x1D:'ldc.i4.7', 0x1E:'ldc.i4.8', 0x1F:'ldc.i4.s', 0x20:'ldc.i4',
    0x21:'ldc.i8', 0x22:'ldc.r4', 0x23:'ldc.r8', 0x25:'dup', 0x26:'pop',
    0x27:'jmp', 0x28:'call', 0x29:'calli', 0x2A:'ret', 0x2B:'br.s',
    0x2C:'brfalse.s', 0x2D:'brtrue.s', 0x2E:'beq.s', 0x2F:'bge.s',
    0x30:'bgt.s', 0x31:'ble.s', 0x32:'blt.s', 0x33:'bne.un.s',
    0x34:'bge.un.s', 0x35:'bgt.un.s', 0x36:'ble.un.s', 0x37:'blt.un.s',
    0x38:'br', 0x39:'brfalse', 0x3A:'brtrue', 0x3B:'beq', 0x3C:'bge',
    0x3D:'bgt', 0x3E:'ble', 0x3F:'blt', 0x40:'bne.un', 0x41:'bge.un',
    0x42:'bgt.un', 0x43:'ble.un', 0x44:'blt.un', 0x45:'switch',
    0x58:'add', 0x59:'sub', 0x5A:'mul', 0x5B:'div', 0x5C:'div.un',
    0x5D:'rem', 0x5E:'rem.un', 0x5F:'and', 0x60:'or', 0x61:'xor',
    0x62:'shl', 0x63:'shr', 0x64:'shr.un', 0x65:'neg', 0x66:'not',
    0x6F:'callvirt', 0x72:'ldstr', 0x73:'newobj', 0x74:'castclass',
    0x75:'isinst', 0x79:'unbox', 0x7B:'ldfld', 0x7C:'ldflda', 0x7D:'stfld',
    0x7E:'ldsfld', 0x7F:'ldsflda', 0x80:'stsfld', 0x8C:'box', 0x8D:'newarr',
    0x8E:'ldlen', 0x8F:'ldelema', 0x91:'ldelem.i1', 0x92:'ldelem.u1',
    0x93:'ldelem.i2', 0x94:'ldelem.u2', 0x95:'ldelem.i4', 0x96:'ldelem.u4',
    0x9A:'ldelem.ref', 0x9C:'stelem.i1', 0x9D:'stelem.i2', 0x9E:'stelem.i4',
    0xA2:'stelem.ref', 0xA3:'ldelem', 0xA4:'stelem', 0xA5:'unbox.any',
    0xD0:'ldtoken', 0xDD:'leave', 0xDE:'leave.s', 0x7A:'throw',
}
OPS2 = {0x01:'ceq', 0x02:'cgt', 0x03:'cgt.un', 0x04:'clt', 0x05:'clt.un',
        0x06:'ldftn', 0x09:'ldarg', 0x0C:'ldloc', 0x0E:'stloc',
        0x15:'initobj', 0x16:'constrained.', 0x1C:'sizeof'}

OP1 = {0x0E:1,0x0F:1,0x10:1,0x11:1,0x12:1,0x13:1,0x1F:1,0x20:4,0x21:8,
       0x22:4,0x23:8,0x27:4,0x28:4,0x29:4,0x2B:1,0x2C:1,0x2D:1,0x2E:1,
       0x2F:1,0x30:1,0x31:1,0x32:1,0x33:1,0x34:1,0x35:1,0x36:1,0x37:1,
       0x38:4,0x39:4,0x3A:4,0x3B:4,0x3C:4,0x3D:4,0x3E:4,0x3F:4,0x40:4,
       0x41:4,0x42:4,0x43:4,0x44:4,0x45:-1,0x6F:4,0x70:4,0x71:4,0x72:4,
       0x73:4,0x74:4,0x75:4,0x79:4,0x7B:4,0x7C:4,0x7D:4,0x7E:4,0x7F:4,
       0x80:4,0x81:4,0x8C:4,0x8D:4,0x8F:4,0xA3:4,0xA4:4,0xA5:4,0xC2:4,
       0xC6:4,0xD0:4,0xDD:4,0xDE:1}
OP2SZ = {0x06:4,0x07:4,0x09:2,0x0A:2,0x0B:2,0x0C:2,0x0D:2,0x0E:2,0x12:1,
         0x15:4,0x16:4,0x19:1,0x1C:4}


def main():
    blob = open(sys.argv[1], 'rb').read()
    (magic, ver, entry, nm, offm, nt, offt, nstat, ns, offs, offlit,
     litsz, offcode, codesz, nic, offic) = struct.unpack_from('<4sIIIIIIIIIIIIIII', blob, 0)
    offnames = struct.unpack_from('<I', blob, 64)[0]
    assert magic == b'MEX1', 'bad magic'

    def cstr(off):
        e = blob.index(b'\0', offnames + off)
        return blob[offnames + off:e].decode('utf-8', 'replace')

    print('MEX1 v%d  entry=#%d  methods=%d types=%d statics=%d strings=%d code=%d icalls=%d'
          % (ver, entry, nm, nt, nstat, ns, codesz, nic))

    lits = []
    for i in range(ns):
        o, l = struct.unpack_from('<II', blob, offs + i * 8)
        lits.append(blob[offlit + o:offlit + o + l].decode('utf-8', 'replace'))
    for i, s in enumerate(lits):
        print('  str%-2d %r' % (i, s))

    icalls = []
    for i in range(nic):
        o = struct.unpack_from('<I', blob, offic + i * 4)[0]
        icalls.append(cstr(o))

    methods = []
    for i in range(nm):
        (co, cs, ms, nl, na, fl, ic, dt, no, _r) = struct.unpack_from(
            '<IIHHHHIIII', blob, offm + i * 32)
        methods.append(dict(co=co, cs=cs, ms=ms, nl=nl, na=na, fl=fl,
                            ic=ic, dt=dt, name=cstr(no)))

    want = sys.argv[2] if len(sys.argv) > 2 else None
    for i, m in enumerate(methods):
        if want and want not in m['name']:
            continue
        kind = 'ICALL#%d' % m['ic'] if m['fl'] & 2 else 'IL'
        print('\nm%d  %s  [%s] args=%d locals=%d maxstack=%d size=%d'
              % (i, m['name'], kind, m['na'], m['nl'], m['ms'], m['cs']))
        if m['fl'] & 2:
            continue
        il = blob[offcode + m['co']:offcode + m['co'] + m['cs']]
        p = 0
        while p < len(il):
            op = il[p]
            if op == 0xFE:
                o2 = il[p+1]
                nmx = OPS2.get(o2, 'fe.%02X' % o2)
                sz = OP2SZ.get(o2, 0)
                arg = ''
                if sz:
                    v = int.from_bytes(il[p+2:p+2+sz], 'little')
                    arg = ' %d' % v
                print('    IL_%04X: %s%s' % (p, nmx, arg))
                p += 2 + sz
                continue
            nmx = OPS.get(op, 'op.%02X' % op)
            sz = OP1.get(op, 0)
            arg = ''
            if sz == -1:
                cnt = int.from_bytes(il[p+1:p+5], 'little')
                p2 = p + 5 + 4 * cnt
                print('    IL_%04X: switch (%d)' % (p, cnt))
                p = p2
                continue
            if sz:
                raw = il[p+1:p+1+sz]
                v = int.from_bytes(raw, 'little', signed=(sz == 1))
                if op == 0x72:
                    arg = ' str%d %r' % (v, lits[v] if v < len(lits) else '?')
                elif op in (0x28, 0x6F, 0x73, 0x27):
                    tgt = methods[v]['name'] if v < len(methods) else '?'
                    if v < len(methods) and methods[v]['fl'] & 2:
                        tgt += '  <icall %s>' % icalls[methods[v]['ic']]
                    arg = ' m%d %s' % (v, tgt)
                elif op in (0x7B, 0x7C, 0x7D):
                    arg = ' fldoff=%d' % v
                elif op in (0x7E, 0x7F, 0x80):
                    arg = ' slot=%d' % v
                elif op in (0x2B,0x2C,0x2D,0x2E,0x2F,0x30,0x31,0x32,0x33,
                            0x34,0x35,0x36,0x37,0xDE):
                    arg = ' -> IL_%04X' % (p + 1 + sz + v)
                elif op in (0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,0x40,
                            0x41,0x42,0x43,0x44,0xDD):
                    sv = int.from_bytes(raw, 'little', signed=True)
                    arg = ' -> IL_%04X' % (p + 1 + sz + sv)
                else:
                    arg = ' %d' % v
            print('    IL_%04X: %s%s' % (p, nmx, arg))
            p += 1 + sz


if __name__ == '__main__':
    main()
