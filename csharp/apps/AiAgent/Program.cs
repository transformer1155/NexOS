using System;
using System.Windows.Forms;
using NexOS.Forms;

namespace WinHost.AiAgent
{
    // Standalone .exe for the AiAgent managed application.
    // Launches the app directly via AppHost - no desktop / taskbar / start menu.
    static class Program
    {
        [STAThread]
        static void Main()
        {
            AppHost.Run(Kind.AiAgent);
        }
    }
}
