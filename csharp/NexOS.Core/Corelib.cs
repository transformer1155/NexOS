// =====================================================================
//  Corelib.cs  -  Minimal BCL for MiniCLR (compiled with /nostdlib)
// ---------------------------------------------------------------------
//  Roslyn requires a fixed set of "special types" to exist when the real
//  System.Private.CoreLib is not referenced.  This file provides exactly
//  those types and nothing more, so every NexOS C# app is a fully
//  self-contained assembly with no AssemblyRef rows -- which lets
//  tools/mex_pack.py resolve every metadata token locally.
// =====================================================================
namespace System
{
    public class Object
    {
        public Object() { }
        public virtual string ToString() { return "object"; }
        public virtual bool Equals(object o) { return this == o; }
        public virtual int GetHashCode() { return 0; }
    }

    public struct Void { }

    public abstract class ValueType : Object { }
    public abstract class Enum : ValueType { }

    public struct Boolean { }
    public struct Char { }
    public struct SByte { }
    public struct Byte { }
    public struct Int16 { }
    public struct UInt16 { }
    public struct Int32 { }
    public struct UInt32 { }
    public struct Int64 { }
    public struct UInt64 { }
    public struct Single { }
    public struct Double { }
    public struct IntPtr { }
    public struct UIntPtr { }

    public sealed class String : Object
    {
        // Layout must match MexString objects created by the interpreter:
        //   [0] = object header (type id), [4] = length, [8..] = UTF-8 bytes
        public readonly int Length;

        public override string ToString() { return this; }

        public static string Concat(string a, string b)
        {
            return NexOS.Sys.StrConcat(a, b);
        }

        public char this[int index]
        {
            get { return NexOS.Sys.StrCharAt(this, index); }
        }

        // Roslyn emits calls to these operator methods for `s == "..."`.
        public static bool op_Equality(string a, string b)
        {
            return NexOS.Sys.StrEq(a, b);
        }

        public static bool op_Inequality(string a, string b)
        {
            return NexOS.Sys.StrEq(a, b) == false;
        }

        public bool Equals(string other)
        {
            return NexOS.Sys.StrEq(this, other);
        }
    }

    public abstract class Array : Object
    {
        public readonly int Length;
    }

    public abstract class Delegate : Object { }
    public abstract class MulticastDelegate : Delegate { }

    public class Attribute : Object { public Attribute() { } }

    public enum AttributeTargets
    {
        Assembly = 1, Module = 2, Class = 4, Struct = 8, Enum = 16,
        Constructor = 32, Method = 64, Property = 128, Field = 256,
        Event = 512, Interface = 1024, Parameter = 2048, Delegate = 4096,
        ReturnValue = 8192, GenericParameter = 16384, All = 32767
    }

    [AttributeUsage(AttributeTargets.Class, Inherited = true)]
    public sealed class AttributeUsageAttribute : Attribute
    {
        public AttributeUsageAttribute(AttributeTargets validOn) { }
        public bool Inherited { get; set; }
        public bool AllowMultiple { get; set; }
    }

    public sealed class ParamArrayAttribute : Attribute { }

    public struct RuntimeTypeHandle { }
    public struct RuntimeFieldHandle { }
    public struct RuntimeMethodHandle { }

    public abstract class Type : Object { }

    public class Exception : Object
    {
        public Exception() { }
        public Exception(string message) { }
    }

    public interface IDisposable { void Dispose(); }
}

namespace System.Reflection
{
    // Required by Roslyn whenever a type declares an indexer.
    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Interface)]
    public sealed class DefaultMemberAttribute : Attribute
    {
        public DefaultMemberAttribute(string memberName) { }
    }
}

namespace System.Runtime.CompilerServices
{
    // Roslyn recognises this attribute by full name and folds the value
    // into the MethodDef ImplFlags column -- that is how mex_pack.py
    // detects which methods are internal calls.
    public enum MethodImplOptions
    {
        Unmanaged = 4,
        NoInlining = 8,
        ForwardRef = 16,
        Synchronized = 32,
        NoOptimization = 64,
        PreserveSig = 128,
        AggressiveInlining = 256,
        InternalCall = 4096
    }

    [AttributeUsage(AttributeTargets.Method | AttributeTargets.Constructor)]
    public sealed class MethodImplAttribute : Attribute
    {
        public MethodImplAttribute(MethodImplOptions methodImplOptions) { }
    }

    public static class RuntimeHelpers { }
}
