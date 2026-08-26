using System;
using System.Windows.Forms;
using NexOS.Forms;

namespace WinHost.About
{
    // Standalone .exe for the About managed application.
    // Launches the app directly via AppHost - no desktop / taskbar / start menu.
    static class Program
    {
        [STAThread]
        static void Main()
        {
            AppHost.Run(Kind.About);
        }
    }
}
