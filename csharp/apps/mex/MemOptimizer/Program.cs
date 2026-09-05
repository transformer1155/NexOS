using NexOS.Forms;

namespace App
{
    // Standalone managed application entry point.
    // The kernel runs this via clrapp MemOptimizer.mex:
    //   - Shell.Init() primes the managed static fields (they never
    //     auto-initialise under MiniCLR),
    //   - Host.OpenApp(kind) asks the kernel to open this one app window
    //     (native mforms_open -> resident NexOS.Forms.Shell::Open).
    // The kernel's native GUI loop then paints and forwards input to it.
    public static class Program
    {
        public static void Main()
        {
            Shell.Init();
            Host.OpenApp(Kind.MemOptimizer);
        }
    }
}
