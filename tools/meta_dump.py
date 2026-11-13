#!/usr/bin/env python3
"""Dump the metadata tables of a managed assembly.

Used to inspect how Roslyn encodes a construct (generics, interfaces,
byref parameters) so tools/mex_pack.py can be taught to resolve it.
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mex_pack as mp


def dump(path, tables=None):
    pe = mp.PEFile(open(path, 'rb').read())
    md = mp.Metadata(pe)
    for name in (tables or ['TypeDef', 'TypeRef', 'TypeSpec', 'MemberRef',
                            'MethodSpec', 'GenericParam', 'InterfaceImpl',
                            'Field', 'MethodDef']):
        rows = md.table(name)
        if not rows:
            continue
        print('==== %s (%d rows) ====' % (name, len(rows)))
        for i, r in enumerate(rows):
            out = {}
            for k, v in r.items():
                if k in ('Signature', 'Value'):
                    out[k] = mp.decompress_uint(md.get_blob(v), 0) if False else v
                elif isinstance(v, int) and name in ('MemberRef', 'TypeSpec',
                                                      'MethodSpec',
                                                      'InterfaceImpl'):
                    out[k] = v
                else:
                    out[k] = v
            print('  [%d] %s' % (i + 1, out))


def dump_il(path, typefilter=None):
    """Print the IL of every method, with resolved names where possible."""
    pe = mp.PEFile(open(path, 'rb').read())
    md = mp.Metadata(pe)
    tdefs = md.table('TypeDef')
    methods = md.table('MethodDef')
    tnames = []
    for td in tdefs:
        ns = md.get_string(td['Namespace'])
        nm = md.get_string(td['Name'])
        tnames.append((ns + '.' + nm) if ns else nm)
    for i, m in enumerate(methods):
        name = md.get_string(m['Name'])
        tidx = 0
        for j in range(len(tdefs)):
            fstart = tdefs[j]['FieldList']
            mstart = tdefs[j]['MethodList']
            mend = tdefs[j + 1]['MethodList'] if j + 1 < len(tdefs) else len(methods) + 1
            if mstart <= i + 1 < mend:
                tidx = j
                break
        fq = tnames[tidx] + '::' + name
        if typefilter and typefilter not in fq:
            continue
        if m['RVA'] == 0:
            print('%s  <no body>' % fq)
            continue
        b0 = pe.d[pe.rva2off(m['RVA'])]
        if (b0 & 3) == 2:
            size = b0 >> 2
            il = pe.d[pe.rva2off(m['RVA']) + 1:][:size]
            print('%s  IL(%d): %s' % (fq, size, il.hex()))
        else:
            print('%s  <fat header>' % fq)


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print('usage: meta_dump.py <dll> [--il [filter]]')
        sys.exit(1)
    path = a[0]
    if len(a) >= 2 and a[1] == '--il':
        filt = a[2] if len(a) > 2 else None
        dump_il(path, filt)
    else:
        dump(path)
