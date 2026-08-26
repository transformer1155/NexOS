using System;
using System.Windows.Forms;
using NexOS.Forms;

namespace WinHost.Terminal
{
    // Standalone .exe for the Terminal managed application.
    // Launches the app directly via AppHost - no desktop / taskbar / start menu.
    static class Program
    {
        [STAThread]
        static void Main()
        {
            AppHost.Run(Kind.Terminal);
        }
    }
}
