// =====================================================================
//  Program.cs  -  MiniCLR milestone 1 smoke test
// ---------------------------------------------------------------------
//  Exercises the opcodes the interpreter must support first:
//  ldstr / call / ret / ldc.i4 / stloc / ldloc / add / mul / blt / br.
// =====================================================================
using NexOS;

public static class Program
{
    private static int Square(int x)
    {
        return x * x;
    }

    public static void Main()
    {
        Sys.Print("CSHARP: hello from managed code\n");

        int sum = 0;
        for (int i = 1; i <= 10; i++)
        {
            sum = sum + i;
        }

        Sys.Print("CSHARP: sum(1..10)=");
        Sys.PrintInt(sum);
        Sys.Print("\n");

        Sys.Print("CSHARP: square(7)=");
        Sys.PrintInt(Square(7));
        Sys.Print("\n");

        Sys.Print("CSHARP_OK\n");
    }
}
