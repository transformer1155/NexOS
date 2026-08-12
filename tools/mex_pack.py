#!/usr/bin/env python3
# =====================================================================
#  mex_pack.py  -  Roslyn assembly  ->  NexOS .mex flat image
# ---------------------------------------------------------------------
#  The NexOS kernel is a 32-bit freestanding C++ image with no room for
#  a real ECMA-335 metadata engine.  So all of the hard work happens
#  here, offline:
#
#    * parse PE32 -> CLI header -> metadata root -> #~ table stream
#    * read TypeDef / Field / MethodDef / MemberRef / TypeRef / #US
#    * compute instance field byte offsets and static slot indices
#    * walk every method body and REWRITE each metadata token in the IL
#      into a flat index the kernel can use directly:
#          ldstr   -> string index
#          call    -> method index
#          ldfld   -> field BYTE OFFSET
#          ldsfld  -> static slot index
#          newarr  -> type index
#
#  The kernel therefore only needs a loader for a handful of flat arrays
#  plus a stack interpreter.  See clr.cpp.
# =====================================================================
import struct, sys, os

# ---------------------------------------------------------------------
#  small binary reader helpers
# ---------------------------------------------------------------------
def u8(b, o):  return b[o]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]

def decompress_uint(b, o):
    """ECMA-335 II.23.2 compressed unsigned integer -> (value, new_offset)"""
    x = b[o]
    if x < 0x80:
        return x, o + 1
    if (x & 0xC0) == 0x80:
        return ((x & 0x3F) << 8) | b[o + 1], o + 2
    return (((x & 0x1F) << 24) | (b[o+1] << 16) | (b[o+2] << 8) | b[o+3]), o + 4


# ---------------------------------------------------------------------
#  PE32 container
# ---------------------------------------------------------------------
class PEFile:
    def __init__(self, data):
        self.d = data
        if data[0:2] != b'MZ':
            raise ValueError('not a PE image (no MZ)')
        pe = u32(data, 0x3C)
        if data[pe:pe+4] != b'PE\0\0':
            raise ValueError('not a PE image (no PE signature)')
        coff = pe + 4
        self.nsections = u16(data, coff + 2)
        opt_size = u16(data, coff + 16)
        opt = coff + 20
        magic = u16(data, opt)
        # data directories start after the standard+windows specific fields
        dd = opt + (96 if magic == 0x10B else 112)
        self.cli_rva = u32(data, dd + 14 * 8)
        self.cli_size = u32(data, dd + 14 * 8 + 4)
        sect = opt + opt_size
        self.sections = []
        for i in range(self.nsections):
            s = sect + i * 40
            self.sections.append({
                'name': data[s:s+8].rstrip(b'\0').decode('ascii', 'replace'),
                'vsize': u32(data, s + 8),
                'vaddr': u32(data, s + 12),
                'rawsize': u32(data, s + 16),
                'rawptr': u32(data, s + 20),
            })

    def rva2off(self, rva):
        for s in self.sections:
            if s['vaddr'] <= rva < s['vaddr'] + max(s['vsize'], s['rawsize']):
                return rva - s['vaddr'] + s['rawptr']
        raise ValueError('RVA 0x%X not mapped by any section' % rva)


# ---------------------------------------------------------------------
#  metadata table schema (ECMA-335 II.22)
#  Every table must be described even if unused, otherwise row sizes
#  cannot be computed and the stream walk desynchronises.
# ---------------------------------------------------------------------
CODED = {
    'TypeDefOrRef':        (2, ['TypeDef', 'TypeRef', 'TypeSpec']),
    'HasConstant':         (2, ['Field', 'Param', 'Property']),
    'HasCustomAttribute':  (5, ['MethodDef','Field','TypeRef','TypeDef','Param',
                                'InterfaceImpl','MemberRef','Module','DeclSecurity',
                                'Property','Event','StandAloneSig','ModuleRef',
                                'TypeSpec','Assembly','AssemblyRef','File',
                                'ExportedType','ManifestResource','GenericParam',
                                'GenericParamConstraint','MethodSpec']),
    'HasFieldMarshall':    (1, ['Field', 'Param']),
    'HasDeclSecurity':     (2, ['TypeDef', 'MethodDef', 'Assembly']),
    'MemberRefParent':     (3, ['TypeDef','TypeRef','ModuleRef','MethodDef','TypeSpec']),
    'HasSemantics':        (1, ['Event', 'Property']),
    'MethodDefOrRef':      (1, ['MethodDef', 'MemberRef']),
    'MemberForwarded':     (1, ['Field', 'MethodDef']),
    'Implementation':      (2, ['File', 'AssemblyRef', 'ExportedType']),
    'CustomAttributeType': (3, [None, None, 'MethodDef', 'MemberRef', None]),
    'ResolutionScope':     (2, ['Module', 'ModuleRef', 'AssemblyRef', 'TypeRef']),
    'TypeOrMethodDef':     (1, ['TypeDef', 'MethodDef']),
}

TABLE_NAMES = {
    0x00:'Module', 0x01:'TypeRef', 0x02:'TypeDef', 0x04:'Field', 0x06:'MethodDef',
    0x08:'Param', 0x09:'InterfaceImpl', 0x0A:'MemberRef', 0x0B:'Constant',
    0x0C:'CustomAttribute', 0x0D:'FieldMarshal', 0x0E:'DeclSecurity',
    0x0F:'ClassLayout', 0x10:'FieldLayout', 0x11:'StandAloneSig', 0x12:'EventMap',
    0x14:'Event', 0x15:'PropertyMap', 0x17:'Property', 0x18:'MethodSemantics',
    0x19:'MethodImpl', 0x1A:'ModuleRef', 0x1B:'TypeSpec', 0x1C:'ImplMap',
    0x1D:'FieldRVA', 0x20:'Assembly', 0x21:'AssemblyProcessor', 0x22:'AssemblyOS',
    0x23:'AssemblyRef', 0x24:'AssemblyRefProcessor', 0x25:'AssemblyRefOS',
    0x26:'File', 0x27:'ExportedType', 0x28:'ManifestResource', 0x29:'NestedClass',
    0x2A:'GenericParam', 0x2B:'MethodSpec', 0x2C:'GenericParamConstraint',
}

SCHEMA = {
    'Module':        [('Generation','u2'),('Name','str'),('Mvid','guid'),
                      ('EncId','guid'),('EncBaseId','guid')],
    'TypeRef':       [('ResolutionScope','c:ResolutionScope'),('Name','str'),
                      ('Namespace','str')],
    'TypeDef':       [('Flags','u4'),('Name','str'),('Namespace','str'),
                      ('Extends','c:TypeDefOrRef'),('FieldList','r:Field'),
                      ('MethodList','r:MethodDef')],
    'Field':         [('Flags','u2'),('Name','str'),('Signature','blob')],
    'MethodDef':     [('RVA','u4'),('ImplFlags','u2'),('Flags','u2'),('Name','str'),
                      ('Signature','blob'),('ParamList','r:Param')],
    'Param':         [('Flags','u2'),('Sequence','u2'),('Name','str')],
    'InterfaceImpl': [('Class','r:TypeDef'),('Interface','c:TypeDefOrRef')],
    'MemberRef':     [('Class','c:MemberRefParent'),('Name','str'),('Signature','blob')],
    'Constant':      [('Type','u2'),('Parent','c:HasConstant'),('Value','blob')],
    'CustomAttribute':[('Parent','c:HasCustomAttribute'),
                       ('Type','c:CustomAttributeType'),('Value','blob')],
    'FieldMarshal':  [('Parent','c:HasFieldMarshall'),('NativeType','blob')],
    'DeclSecurity':  [('Action','u2'),('Parent','c:HasDeclSecurity'),
                      ('PermissionSet','blob')],
    'ClassLayout':   [('PackingSize','u2'),('ClassSize','u4'),('Parent','r:TypeDef')],
    'FieldLayout':   [('Offset','u4'),('Field','r:Field')],
    'StandAloneSig': [('Signature','blob')],
    'EventMap':      [('Parent','r:TypeDef'),('EventList','r:Event')],
    'Event':         [('EventFlags','u2'),('Name','str'),('EventType','c:TypeDefOrRef')],
    'PropertyMap':   [('Parent','r:TypeDef'),('PropertyList','r:Property')],
    'Property':      [('Flags','u2'),('Name','str'),('Type','blob')],
    'MethodSemantics':[('Semantics','u2'),('Method','r:MethodDef'),
                       ('Association','c:HasSemantics')],
    'MethodImpl':    [('Class','r:TypeDef'),('MethodBody','c:MethodDefOrRef'),
                      ('MethodDeclaration','c:MethodDefOrRef')],
    'ModuleRef':     [('Name','str')],
    'TypeSpec':      [('Signature','blob')],
    'ImplMap':       [('MappingFlags','u2'),('MemberForwarded','c:MemberForwarded'),
                      ('ImportName','str'),('ImportScope','r:ModuleRef')],
    'FieldRVA':      [('RVA','u4'),('Field','r:Field')],
    'Assembly':      [('HashAlgId','u4'),('MajorVersion','u2'),('MinorVersion','u2'),
                      ('BuildNumber','u2'),('RevisionNumber','u2'),('Flags','u4'),
                      ('PublicKey','blob'),('Name','str'),('Culture','str')],
    'AssemblyProcessor':[('Processor','u4')],
    'AssemblyOS':    [('OSPlatformID','u4'),('OSMajorVersion','u4'),('OSMinorVersion','u4')],
    'AssemblyRef':   [('MajorVersion','u2'),('MinorVersion','u2'),('BuildNumber','u2'),
                      ('RevisionNumber','u2'),('Flags','u4'),('PublicKeyOrToken','blob'),
                      ('Name','str'),('Culture','str'),('HashValue','blob')],
    'AssemblyRefProcessor':[('Processor','u4'),('AssemblyRef','r:AssemblyRef')],
    'AssemblyRefOS': [('OSPlatformID','u4'),('OSMajorVersion','u4'),
                      ('OSMinorVersion','u4'),('AssemblyRef','r:AssemblyRef')],
    'File':          [('Flags','u4'),('Name','str'),('HashValue','blob')],
    'ExportedType':  [('Flags','u4'),('TypeDefId','u4'),('TypeName','str'),
                      ('TypeNamespace','str'),('Implementation','c:Implementation')],
    'ManifestResource':[('Offset','u4'),('Flags','u4'),('Name','str'),
                        ('Implementation','c:Implementation')],
    'NestedClass':   [('NestedClass','r:TypeDef'),('EnclosingClass','r:TypeDef')],
    'GenericParam':  [('Number','u2'),('Flags','u2'),('Owner','c:TypeOrMethodDef'),
                      ('Name','str')],
    'MethodSpec':    [('Method','c:MethodDefOrRef'),('Instantiation','blob')],
    'GenericParamConstraint':[('Owner','r:GenericParam'),('Constraint','c:TypeDefOrRef')],
}


class Metadata:
    def __init__(self, pe):
        d = pe.d
        cli = pe.rva2off(pe.cli_rva)
        self.entry_token = u32(d, cli + 20)
        md_rva = u32(d, cli + 8)
        md = pe.rva2off(md_rva)
        if d[md:md+4] != b'BSJB':
            raise ValueError('bad metadata signature')
        vlen = u32(d, md + 12)
        p = md + 16 + vlen
        p += 2                       # flags
        nstreams = u16(d, p); p += 2
        self.streams = {}
        for _ in range(nstreams):
            off = u32(d, p); size = u32(d, p + 4); p += 8
            e = d.index(b'\0', p)
            name = d[p:e].decode('ascii')
            p = e + 1
            p = (p + 3) & ~3
            self.streams[name] = (md + off, size)

        self.strings = self.streams.get('#Strings')
        self.blob    = self.streams.get('#Blob')
        self.us      = self.streams.get('#US')
        self.d = d
        self._parse_tables()

    # ---- heap accessors ------------------------------------------
    def get_string(self, idx):
        base = self.strings[0]
        e = self.d.index(b'\0', base + idx)
        return self.d[base + idx:e].decode('utf-8', 'replace')

    def get_blob(self, idx):
        base = self.blob[0]
        ln, o = decompress_uint(self.d, base + idx)
        return self.d[o:o + ln]

    def get_us(self, off):
        """#US entry -> python str"""
        base = self.us[0]
        ln, o = decompress_uint(self.d, base + off)
        if ln == 0:
            return ''
        raw = self.d[o:o + ln - 1]        # last byte is the terminal flag
        return raw.decode('utf-16-le', 'replace')

    # ---- #~ table stream -----------------------------------------
    def _parse_tables(self):
        d = self.d
        base = self.streams['#~'][0]
        heapsizes = u8(d, base + 6)
        self.wide_str  = bool(heapsizes & 0x01)
        self.wide_guid = bool(heapsizes & 0x02)
        self.wide_blob = bool(heapsizes & 0x04)
        valid = u64(d, base + 8)
        p = base + 24
        self.rows = {}
        for t in range(64):
            if valid & (1 << t):
                n = u32(d, p); p += 4
                self.rows[TABLE_NAMES.get(t, 't%02X' % t)] = n
        self.tables = {}
        for t in range(64):
            name = TABLE_NAMES.get(t)
            if not (valid & (1 << t)):
                continue
            if name is None or name not in SCHEMA:
                raise ValueError('unhandled metadata table 0x%02X' % t)
            cols = SCHEMA[name]
            widths = [self._colw(c[1]) for c in cols]
            rowsz = sum(widths)
            n = self.rows[name]
            rowsdata = []
            for i in range(n):
                off = p + i * rowsz
                rec = {}
                co = off
                for (cname, ctype), w in zip(cols, widths):
                    rec[cname] = u16(d, co) if w == 2 else u32(d, co)
                    co += w
                rowsdata.append(rec)
            self.tables[name] = rowsdata
            p += rowsz * n

    def _colw(self, ctype):
        if ctype == 'u1':   return 1
        if ctype == 'u2':   return 2
        if ctype == 'u4':   return 4
        if ctype == 'u8':   return 8
        if ctype == 'str':  return 4 if self.wide_str else 2
        if ctype == 'guid': return 4 if self.wide_guid else 2
        if ctype == 'blob': return 4 if self.wide_blob else 2
        if ctype.startswith('r:'):
            return 4 if self.rows.get(ctype[2:], 0) >= 65536 else 2
        if ctype.startswith('c:'):
            bits, tabs = CODED[ctype[2:]]
            limit = 1 << (16 - bits)
            for tn in tabs:
                if tn and self.rows.get(tn, 0) >= limit:
                    return 4
            return 2
        raise ValueError('bad column type ' + ctype)

    def table(self, name):
        return self.tables.get(name, [])


# ---------------------------------------------------------------------
#  IL opcode operand sizes -- everything not listed takes no operand
# ---------------------------------------------------------------------
OP1 = {  # single byte opcodes with operands: opcode -> operand byte count
    0x0E:1, 0x0F:1, 0x10:1, 0x11:1, 0x12:1, 0x13:1, 0x1F:1,
    0x20:4, 0x21:8, 0x22:4, 0x23:8,
    0x27:4, 0x28:4, 0x29:4,
    0x2B:1, 0x2C:1, 0x2D:1, 0x2E:1, 0x2F:1, 0x30:1, 0x31:1, 0x32:1,
    0x33:1, 0x34:1, 0x35:1, 0x36:1, 0x37:1,
    0x38:4, 0x39:4, 0x3A:4, 0x3B:4, 0x3C:4, 0x3D:4, 0x3E:4, 0x3F:4,
    0x40:4, 0x41:4, 0x42:4, 0x43:4, 0x44:4,
    0x45:-1,                                    # switch
    0x6F:4, 0x70:4, 0x71:4, 0x72:4, 0x73:4, 0x74:4, 0x75:4, 0x79:4,
    0x7B:4, 0x7C:4, 0x7D:4, 0x7E:4, 0x7F:4, 0x80:4, 0x81:4,
    0x8C:4, 0x8D:4, 0x8F:4,
    0xA3:4, 0xA4:4, 0xA5:4,
    0xC2:4, 0xC6:4, 0xD0:4, 0xDD:4, 0xDE:1,
}
OP2 = {  # 0xFE-prefixed opcodes with operands
    0x06:4, 0x07:4, 0x09:2, 0x0A:2, 0x0B:2, 0x0C:2, 0x0D:2, 0x0E:2,
    0x12:1, 0x15:4, 0x16:4, 0x19:1, 0x1C:4,
}

# opcodes whose 4-byte operand is a metadata token, grouped by what the
# kernel wants to see there instead
TOK_METHOD = {0x27, 0x28, 0x6F, 0x73}          # jmp call callvirt newobj
TOK_STRING = {0x72}                            # ldstr
TOK_FIELD  = {0x7B, 0x7C, 0x7D}                # ldfld ldflda stfld
TOK_SFIELD = {0x7E, 0x7F, 0x80}                # ldsfld ldsflda stsfld
TOK_TYPE   = {0x70, 0x71, 0x74, 0x75, 0x79, 0x81, 0x8C, 0x8D,
              0x8F, 0xA3, 0xA4, 0xA5, 0xD0}


# ---------------------------------------------------------------------
#  signature helpers
# ---------------------------------------------------------------------
ELEMENT_SIZE_8 = {0x0A, 0x0B, 0x0D}   # i8, u8, r8  -> 8 byte slots

def parse_method_sig(sig):
    """-> (has_this, param_count, has_ret)"""
    o = 0
    cc = sig[o]; o += 1
    has_this = bool(cc & 0x20)
    if cc & 0x10:                      # generic
        _, o = decompress_uint(sig, o)
    n, o = decompress_uint(sig, o)
    # RetType follows; skip any custom modifiers then test for VOID
    while o < len(sig) and sig[o] in (0x1F, 0x20):
        o += 1
        _, o = decompress_uint(sig, o)
    has_ret = not (o < len(sig) and sig[o] == 0x01)   # 0x01 = ELEMENT_TYPE_VOID
    return has_this, n, has_ret

def field_slot_size(sig):
    """Field signature -> byte size (only 4 and 8 are produced)."""
    # sig[0] == 0x06 (FIELD), then the type
    o = 1
    while o < len(sig) and sig[o] in (0x1F, 0x20):   # cmod_reqd / cmod_opt
        o += 1
        _, o = decompress_uint(sig, o)
    if o < len(sig) and sig[o] in ELEMENT_SIZE_8:
        return 8
    return 4

def parse_local_sig(sig):
    """LocalVarSig -> local slot count"""
    if not sig or sig[0] != 0x07:
        return 0
    n, _ = decompress_uint(sig, 1)
    return n


# =====================================================================
#  Packer
# =====================================================================
class Packer:
    def __init__(self, path, verbose=False):
        self.verbose = verbose
        with open(path, 'rb') as f:
            data = f.read()
        self.pe = PEFile(data)
        self.md = Metadata(self.pe)
        self.strdata = bytearray()
        self.strmap = {}
        self.us_list = []        # list of utf-8 bytes
        self.us_map = {}         # #US offset -> index
        self.icall_names = []
        self.icall_map = {}

    # ---- string blob (names, for debugging + icall binding) -------
    def intern(self, s):
        if s in self.strmap:
            return self.strmap[s]
        off = len(self.strdata)
        self.strdata += s.encode('utf-8') + b'\0'
        self.strmap[s] = off
        return off

    # ---- pass 1: types + field layout -----------------------------
    def build_types(self):
        md = self.md
        tdefs = md.table('TypeDef')
        fields = md.table('Field')
        methods = md.table('MethodDef')
        n_td = len(tdefs)

        self.type_info = []
        self.field_offset = {}     # field row (1-based) -> byte offset
        self.static_slot = {}      # field row (1-based) -> slot index

        # ---- pass A: record the shape of every type ----------------
        for i, td in enumerate(tdefs):
            fstart = td['FieldList']
            fend = tdefs[i+1]['FieldList'] if i + 1 < n_td else len(fields) + 1
            mstart = td['MethodList']
            mend = tdefs[i+1]['MethodList'] if i + 1 < n_td else len(methods) + 1
            name = md.get_string(td['Name'])
            ns = md.get_string(td['Namespace'])
            full = (ns + '.' + name) if ns else name

            self.type_info.append({
                'name': full,
                'inst_size': 0,
                'extends': td['Extends'],
                'base': None,
                'fstart': fstart, 'fend': fend,
                'mstart': mstart, 'mend': mend,
                'flags': 0,
            })

        # ---- resolve Extends (coded TypeDefOrRef) to a type index ---
        #  tag 0 = TypeDef (this assembly), 1 = TypeRef, 2 = TypeSpec.
        #  We compile /nostdlib, so every base class that matters is a
        #  TypeDef; anything else is external and the chain stops there.
        for t in self.type_info:
            ext = t['extends']
            if ext:
                tag, row = ext & 3, ext >> 2
                if tag == 0 and 1 <= row <= n_td:
                    t['base'] = row - 1

        # ---- pass B: lay out fields, base class first ---------------
        #  A derived class must place its own fields *after* everything
        #  its base declared.  Laying every type out from offset 4 makes
        #  child fields alias parent fields -- writing Button.Text would
        #  clobber Control.X.  Recursing into the base first guarantees
        #  the inherited size is known before the child is placed.
        n_static = [0]
        done = [False] * n_td

        def layout(i, chain):
            if done[i]:
                return
            t = self.type_info[i]
            if i in chain:                      # inheritance cycle: bail
                t['inst_size'] = 4
                done[i] = True
                return
            inst = 4                            # object header: type id
            b = t['base']
            if b is not None:
                layout(b, chain | {i})
                inst = self.type_info[b]['inst_size']
            for fr in range(t['fstart'], t['fend']):
                f = fields[fr - 1]
                flags = f['Flags']
                sig = md.get_blob(f['Signature'])
                sz = field_slot_size(sig)
                if flags & 0x10:                # STATIC
                    self.static_slot[fr] = n_static[0]
                    n_static[0] += 2 if sz == 8 else 1
                else:
                    if sz == 8 and (inst & 7):
                        inst += 4
                    self.field_offset[fr] = inst
                    inst += sz
            t['inst_size'] = inst
            done[i] = True

        for i in range(n_td):
            layout(i, frozenset())
        self.n_static = n_static[0]

        # map method row -> declaring type index
        self.method_type = {}
        for ti, t in enumerate(self.type_info):
            for mr in range(t['mstart'], t['mend']):
                self.method_type[mr] = ti

        # tag the well-known types the interpreter special-cases
        for ti, t in enumerate(self.type_info):
            if t['name'] == 'System.String':
                t['flags'] |= 2
                self.string_type = ti
            elif t['name'] == 'System.Object':
                self.object_type = ti

    # ---- pass 2: methods ------------------------------------------
    def build_methods(self):
        md = self.md
        methods = md.table('MethodDef')
        self.method_info = []
        self.code = bytearray()

        for mi, m in enumerate(methods):
            row = mi + 1
            name = md.get_string(m['Name'])
            sig = md.get_blob(m['Signature'])
            has_this, npar, has_ret = parse_method_sig(sig)
            n_args = npar + (1 if has_this else 0)
            impl = m['ImplFlags']
            flags = 0
            if not has_this:
                flags |= 1
            if name == '.ctor' or name == '.cctor':
                flags |= 4
            if has_ret:
                flags |= 8
            if m['Flags'] & 0x0040:             # MethodAttributes.Virtual
                flags |= 32

            tidx = self.method_type.get(row, 0)
            tname = self.type_info[tidx]['name'] if self.type_info else '?'
            fq = tname + '::' + name

            icall_id = 0xFFFFFFFF
            code_off = 0
            code_size = 0
            max_stack = 8
            n_locals = 0

            if impl & 0x1000:
                # MethodImplAttributes.InternalCall -> bound to a native
                # handler in clr.cpp by fully-qualified name.
                flags |= 2
                if fq not in self.icall_map:
                    self.icall_map[fq] = len(self.icall_names)
                    self.icall_names.append(fq)
                icall_id = self.icall_map[fq]
            elif m['RVA'] == 0:
                # Abstract or interface method: no body and no native
                # implementation either.  Never registered as an icall --
                # doing so produced phantom "unbound" entries for things
                # like System.IDisposable::Dispose.  Calling one is a
                # runtime fault, which the interpreter reports.
                flags |= 16
            else:
                body = self.read_body(m['RVA'])
                max_stack = body['max_stack']
                n_locals = body['n_locals']
                il = self.rewrite_il(bytearray(body['il']), fq)
                code_off = len(self.code)
                code_size = len(il)
                self.code += il

            self.method_info.append({
                'code_off': code_off, 'code_size': code_size,
                'max_stack': max_stack, 'n_locals': n_locals,
                'n_args': n_args, 'flags': flags,
                'icall_id': icall_id, 'decl_type': tidx,
                'name': fq, 'short': name,
            })

    def read_body(self, rva):
        d = self.pe.d
        o = self.pe.rva2off(rva)
        b0 = d[o]
        if (b0 & 3) == 2:                       # tiny header
            size = b0 >> 2
            return {'il': d[o+1:o+1+size], 'max_stack': 8, 'n_locals': 0}
        if (b0 & 3) == 3:                       # fat header
            flags_hdr = u16(d, o)
            hdr_words = flags_hdr >> 12
            max_stack = u16(d, o + 2)
            size = u32(d, o + 4)
            localtok = u32(d, o + 8)
            n_locals = 0
            if localtok:
                sigs = self.md.table('StandAloneSig')
                rowi = (localtok & 0xFFFFFF) - 1
                if 0 <= rowi < len(sigs):
                    n_locals = parse_local_sig(
                        self.md.get_blob(sigs[rowi]['Signature']))
            start = o + hdr_words * 4
            return {'il': d[start:start+size], 'max_stack': max_stack,
                    'n_locals': n_locals}
        raise ValueError('unknown method header byte 0x%02X' % b0)

    # ---- token resolution -----------------------------------------
    def resolve_method_token(self, tok):
        table = tok >> 24
        row = tok & 0xFFFFFF
        if table == 0x06:                      # MethodDef
            return row - 1
        if table == 0x0A:                      # MemberRef -> match locally
            mr = self.md.table('MemberRef')[row - 1]
            name = self.md.get_string(mr['Name'])
            sig = self.md.get_blob(mr['Signature'])
            _, npar, _ = parse_method_sig(sig)
            # MemberRefParent coded index: tag in low 3 bits
            tag = mr['Class'] & 7
            pidx = mr['Class'] >> 3
            tname = None
            if tag == 0:                       # TypeDef
                t = self.type_info[pidx - 1]
                tname = t['name']
            elif tag == 1:                     # TypeRef
                tr = self.md.table('TypeRef')[pidx - 1]
                ns = self.md.get_string(tr['Namespace'])
                nm = self.md.get_string(tr['Name'])
                tname = (ns + '.' + nm) if ns else nm
            if tname is not None:
                want = tname + '::' + name
                for i, mi in enumerate(self.method_info):
                    if mi['name'] == want and mi['n_args'] - \
                       (0 if mi['flags'] & 1 else 1) == npar:
                        return i
                for i, mi in enumerate(self.method_info):
                    if mi['name'] == want:
                        return i
            raise ValueError('unresolved MemberRef %s::%s' % (tname, name))
        raise ValueError('unsupported method token 0x%08X' % tok)

    def resolve_type_token(self, tok):
        table = tok >> 24
        row = tok & 0xFFFFFF
        if table == 0x02:
            return row - 1
        if table == 0x01:
            tr = self.md.table('TypeRef')[row - 1]
            ns = self.md.get_string(tr['Namespace'])
            nm = self.md.get_string(tr['Name'])
            full = (ns + '.' + nm) if ns else nm
            for i, t in enumerate(self.type_info):
                if t['name'] == full:
                    return i
            raise ValueError('unresolved TypeRef ' + full)
        if table == 0x04:                      # ldtoken of a field
            return 0
        raise ValueError('unsupported type token 0x%08X' % tok)

    def resolve_field(self, tok, static):
        table = tok >> 24
        row = tok & 0xFFFFFF
        if table == 0x04:
            if static:
                return self.static_slot[row]
            return self.field_offset[row]
        if table == 0x0A:
            mr = self.md.table('MemberRef')[row - 1]
            name = self.md.get_string(mr['Name'])
            raise ValueError('field MemberRef not supported: ' + name)
        raise ValueError('unsupported field token 0x%08X' % tok)

    def intern_us(self, tok):
        off = tok & 0xFFFFFF
        if off in self.us_map:
            return self.us_map[off]
        s = self.md.get_us(off)
        idx = len(self.us_list)
        self.us_list.append(s.encode('utf-8'))
        self.us_map[off] = idx
        return idx

    # ---- IL walk + token rewrite ----------------------------------
    def rewrite_il(self, il, owner):
        i = 0
        n = len(il)
        while i < n:
            op = il[i]
            if op == 0xFE:
                op2 = il[i+1]
                sz = OP2.get(op2, 0)
                i += 2 + sz
                continue
            sz = OP1.get(op, 0)
            if sz == -1:                       # switch
                cnt = u32(il, i + 1)
                i += 1 + 4 + 4 * cnt
                continue
            if sz == 4:
                tok = u32(il, i + 1)
                new = None
                if op in TOK_STRING:
                    new = self.intern_us(tok)
                elif op in TOK_METHOD:
                    new = self.resolve_method_token(tok)
                elif op in TOK_FIELD:
                    new = self.resolve_field(tok, False)
                elif op in TOK_SFIELD:
                    new = self.resolve_field(tok, True)
                elif op in TOK_TYPE:
                    new = self.resolve_type_token(tok)
                if new is not None:
                    struct.pack_into('<I', il, i + 1, new & 0xFFFFFFFF)
            i += 1 + sz
        return il

    # ---- entry point ----------------------------------------------
    def find_entry(self, want):
        for i, m in enumerate(self.method_info):
            if m['name'] == want:
                return i
        for i, m in enumerate(self.method_info):
            if m['name'].endswith('::Main'):
                return i
        raise ValueError('entry point not found: ' + want)

    # ---- emit ------------------------------------------------------
    def emit(self, entry_name):
        self.build_types()
        self.build_methods()
        entry = self.find_entry(entry_name)

        for t in self.type_info:
            t['name_off'] = self.intern(t['name'])
        for m in self.method_info:
            m['name_off'] = self.intern(m['name'])
            m['short_off'] = self.intern(m['short'])
        icall_offs = [self.intern(x) for x in self.icall_names]

        # ---- string literal table (utf-8, nul terminated) ---------
        litblob = bytearray()
        littab = []
        for s in self.us_list:
            littab.append((len(litblob), len(s)))
            litblob += s + b'\0'

        HDR = 72          # 16 u32 fields + off_names + pad
        MSZ, TSZ, SSZ = 32, 16, 8
        off_methods = HDR
        off_types   = off_methods + MSZ * len(self.method_info)
        off_strings = off_types + TSZ * len(self.type_info)
        off_icalls  = off_strings + SSZ * len(littab)
        off_litdata = off_icalls + 4 * len(icall_offs)
        off_names   = off_litdata + len(litblob)
        off_code    = off_names + len(self.strdata)
        off_code    = (off_code + 3) & ~3
        total       = off_code + len(self.code)

        out = bytearray(total)
        struct.pack_into('<4sIIIIIIIIIIIIIII', out, 0,
                         b'MEX1', 1, entry,
                         len(self.method_info), off_methods,
                         len(self.type_info), off_types,
                         self.n_static,
                         len(littab), off_strings, off_litdata, len(litblob),
                         off_code, len(self.code),
                         len(icall_offs), off_icalls)
        struct.pack_into('<I', out, 64, off_names)     # header slot 17

        p = off_methods
        for m in self.method_info:
            struct.pack_into('<IIHHHHIIII', out, p,
                             m['code_off'], m['code_size'],
                             m['max_stack'], m['n_locals'],
                             m['n_args'], m['flags'],
                             m['icall_id'] & 0xFFFFFFFF, m['decl_type'],
                             m['name_off'], m['short_off'])
            p += MSZ
        p = off_types
        for t in self.type_info:
            base = t['base'] if t['base'] is not None else 0xFFFFFFFF
            struct.pack_into('<IIII', out, p,
                             t['inst_size'], t['name_off'],
                             base, t['flags'])
            p += TSZ
        p = off_strings
        for (o, l) in littab:
            struct.pack_into('<II', out, p, o, l)
            p += SSZ
        p = off_icalls
        for o in icall_offs:
            struct.pack_into('<I', out, p, o)
            p += 4
        out[off_litdata:off_litdata+len(litblob)] = litblob
        out[off_names:off_names+len(self.strdata)] = self.strdata
        out[off_code:off_code+len(self.code)] = self.code
        return bytes(out)


def main():
    if len(sys.argv) < 3:
        print('usage: mex_pack.py <input.dll> <output.mex> [Entry::Method] [-v]')
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    entry = 'Program::Main'
    verbose = '-v' in sys.argv
    for a in sys.argv[3:]:
        if a != '-v':
            entry = a
    p = Packer(src, verbose)
    blob = p.emit(entry)
    with open(dst, 'wb') as f:
        f.write(blob)

    print('[mex] %s -> %s  (%d bytes)' % (os.path.basename(src),
                                          os.path.basename(dst), len(blob)))
    print('[mex] types=%d methods=%d statics=%d strings=%d code=%d icalls=%d'
          % (len(p.type_info), len(p.method_info), p.n_static,
             len(p.us_list), len(p.code), len(p.icall_names)))
    if verbose:
        print('[mex] entry = #%d %s' % (p.find_entry(entry),
                                        p.method_info[p.find_entry(entry)]['name']))
        for i, nm in enumerate(p.icall_names):
            print('       icall %2d  %s' % (i, nm))
        for i, m in enumerate(p.method_info):
            kind = 'icall' if m['flags'] & 2 else 'il   '
            print('       m%-3d %s %-40s args=%d loc=%d stk=%d size=%d'
                  % (i, kind, m['name'], m['n_args'], m['n_locals'],
                     m['max_stack'], m['code_size']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
