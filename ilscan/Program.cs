using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text.RegularExpressions;

// Full IL opcode audit: enumerate every opcode used across all methods of a
// managed DLL and report any not implemented by the minimal MiniCLR (clr.cpp).
// Correctly skips variable-length switch operands, so the report is accurate.
class Program
{
    static Dictionary<short,string> Names = new Dictionary<short,string>();
    static Dictionary<short,System.Reflection.Emit.OpCode> EmitMap = new Dictionary<short,System.Reflection.Emit.OpCode>();

    static int OperandSize(System.Reflection.Emit.OperandType tp)
    {
        switch (tp)
        {
            case System.Reflection.Emit.OperandType.InlineNone: return 0;
            case System.Reflection.Emit.OperandType.ShortInlineBrTarget:
            case System.Reflection.Emit.OperandType.ShortInlineI:
            case System.Reflection.Emit.OperandType.ShortInlineVar: return 1;
            case System.Reflection.Emit.OperandType.InlineBrTarget:
            case System.Reflection.Emit.OperandType.InlineField:
            case System.Reflection.Emit.OperandType.InlineI:
            case System.Reflection.Emit.OperandType.InlineMethod:
            case System.Reflection.Emit.OperandType.InlineSig:
            case System.Reflection.Emit.OperandType.InlineString:
            case System.Reflection.Emit.OperandType.InlineTok:
            case System.Reflection.Emit.OperandType.InlineType:
            case System.Reflection.Emit.OperandType.ShortInlineR: return 4;
            case System.Reflection.Emit.OperandType.InlineR:
            case System.Reflection.Emit.OperandType.InlineI8: return 8;
            case System.Reflection.Emit.OperandType.InlineSwitch: return -1; // special
            default: return 0;
        }
    }

    static int SkipForSwitch(byte[] il, int pos)
    {
        if (pos + 4 > il.Length) return 4;
        int count = il[pos] | (il[pos+1]<<8) | (il[pos+2]<<16) | (il[pos+3]<<24);
        return 4 + count * 4;
    }

    static void Main(string[] args)
    {
        if (args.Length < 2) { Console.WriteLine("usage: ilscan <dll> <clr.cpp> | ilscan <dll> dump:<substr>"); return; }
        var t = typeof(System.Reflection.Emit.OpCodes);
        foreach (var f in t.GetFields(BindingFlags.Public|BindingFlags.Static))
        {
            if (f.FieldType == typeof(System.Reflection.Emit.OpCode))
            {
                var oc = (System.Reflection.Emit.OpCode)f.GetValue(null);
                short v = oc.Value;
                if (!Names.ContainsKey(v)) Names[v] = f.Name;
                if (!EmitMap.ContainsKey(v)) EmitMap[v] = oc;
            }
        }

        // DUMP MODE
        if (args[1].StartsWith("dump:"))
        {
            string sub = args[1].Substring(5);
            using (var fs2 = File.OpenRead(args[0]))
            using (var pe2 = new PEReader(fs2))
            {
                var mr2 = pe2.GetMetadataReader();
                foreach (var h in mr2.MethodDefinitions)
                {
                    var md = mr2.GetMethodDefinition(h);
                    string mt = mr2.GetString(md.Name);
                    string ct = "";
                    try { ct = mr2.GetString(mr2.GetTypeDefinition(md.GetDeclaringType()).Name); } catch {}
                    string full = (ct.Length>0?ct+".":"") + mt;
                    if (mt.IndexOf(sub, StringComparison.OrdinalIgnoreCase) < 0 &&
                        full.IndexOf(sub, StringComparison.OrdinalIgnoreCase) < 0) continue;
                    int rva = md.RelativeVirtualAddress;
                    if (rva == 0) { Console.WriteLine($"=== {full} (no body) ==="); continue; }
                    var body = pe2.GetMethodBody(rva);
                    var il = body.GetILBytes();
                    Console.WriteLine($"=== {full} (IL {il.Length} bytes) ===");
                    int i = 0, off = 0;
                    while (i < il.Length)
                    {
                        int opv = il[i]; System.Reflection.Emit.OpCode oc; string nm;
                        if (opv == 0xFE)
                        {
                            if (i+1 >= il.Length) break;
                            opv = 0xFE00 | il[i+1];
                            nm = EmitMap.TryGetValue((short)opv, out oc) ? oc.Name : ("0xFE"+il[i+1].ToString("X2"));
                            i += 2;
                        }
                        else { nm = EmitMap.TryGetValue((short)opv, out oc) ? oc.Name : ("0x"+opv.ToString("X2")); i += 1; }
                        Console.Write($"  {off,-5} {nm}");
                        int skip = (oc.Name == null) ? 0 : OperandSize(oc.OperandType);
                        if (skip < 0) skip = SkipForSwitch(il, i);
                        if (skip > 0 && i+skip <= il.Length)
                        {
                            var sb = new System.Text.StringBuilder();
                            for (int k=0;k<skip;k++) sb.Append(il[i+k].ToString("X2"));
                            Console.Write($"  [{sb}]");
                        }
                        Console.WriteLine();
                        i += skip; off++;
                    }
                }
            }
            return;
        }

        var Supported = new HashSet<int>();
        var re = new Regex(@"case\s+0x([0-9A-Fa-f]+)");
        foreach (var line in File.ReadAllLines(args[1]))
            foreach (Match m in re.Matches(line))
                try { Supported.Add(Convert.ToInt32(m.Groups[1].Value, 16)); } catch {}
        Console.WriteLine($"clr.cpp supported opcodes: {Supported.Count}");
        bool IsSupported(int v)
        {
            if (Supported.Contains(v)) return true;
            if ((v >> 8) == 0xFE) return Supported.Contains(v & 0xFF);
            return false;
        }

        using var fs = File.OpenRead(args[0]);
        using var pe = new PEReader(fs);
        var mr = pe.GetMetadataReader();
        var used = new HashSet<int>();
        var missSrc = new Dictionary<int, HashSet<string>>();

        foreach (var h in mr.MethodDefinitions)
        {
            var md = mr.GetMethodDefinition(h);
            int rva = md.RelativeVirtualAddress;
            if (rva == 0) continue;
            var body = pe.GetMethodBody(rva);
            var il = body.GetILBytes();
            if (il == null) continue;
            string mt = mr.GetString(md.Name);
            string ct = "";
            try { ct = mr.GetString(mr.GetTypeDefinition(md.GetDeclaringType()).Name); } catch {}
            string full = (ct.Length>0?ct+".":"") + mt;

            int i = 0;
            while (i < il.Length)
            {
                int opv = il[i];
                System.Reflection.Emit.OpCode oc;
                if (opv == 0xFE)
                {
                    if (i+1 >= il.Length) break;
                    opv = 0xFE00 | il[i+1];
                    if (!EmitMap.TryGetValue((short)opv, out oc)) oc = default;
                    i += 2;
                }
                else
                {
                    if (!EmitMap.TryGetValue((short)opv, out oc)) oc = default;
                    i += 1;
                }
                used.Add(opv);
                if (!IsSupported(opv))
                {
                    if (!missSrc.ContainsKey(opv)) missSrc[opv] = new HashSet<string>();
                    missSrc[opv].Add(full);
                }
                int skip = (oc.Name == null) ? 0 : OperandSize(oc.OperandType);
                if (skip < 0) skip = SkipForSwitch(il, i);
                i += skip;
            }
        }

        var missing = new List<int>(missSrc.Keys);
        missing.Sort();
        Console.WriteLine($"IL opcodes used: {used.Count}");
        if (missing.Count == 0)
        {
            Console.WriteLine("OK: all used opcodes are supported by MiniCLR.");
        }
        else
        {
            Console.WriteLine($"MISSING ({missing.Count}):");
            foreach (var m in missing)
            {
                string nm = Names.TryGetValue((short)m, out var n) ? n : (((m>>8)==0xFE) ? ("0xFE"+((m&0xFF).ToString("X2"))) : "?");
                var srcs = missSrc[m];
                int cnt = 0; var sb = new System.Text.StringBuilder();
                foreach (var s in srcs) { if (cnt++ >= 6) break; if (cnt>1) sb.Append(", "); sb.Append(s); }
                Console.WriteLine($"  0x{m:X2}  {nm,-12}  in: {sb}  ({srcs.Count} methods)");
            }
        }
    }
}
