using System;
using System.Windows.Forms;
using NexOS.Forms;

namespace WinHost.AiSetup
{
    // Standalone .exe for the AiSetup managed application.
    // Launches the app directly via AppHost - no desktop / taskbar / start menu.
    static class Program
    {
        [STAThread]
        static void Main()
        {
            AppHost.Run(Kind.AiSetup);
        }
    }
}
