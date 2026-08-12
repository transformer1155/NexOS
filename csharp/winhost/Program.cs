// =====================================================================
//  Program.cs  -  application entry point (Visual Studio WinForms template)
// ---------------------------------------------------------------------
//  Mirrors the WinFormsApp1 template: ApplicationConfiguration.Initialize()
//  sets up high-DPI + default font, then we run the shell form.  The form
//  itself parses "--shot <path> [delay] [kinds,...]" for headless frame
//  capture (used by "make winhost-shot").
// =====================================================================
using System;
using System.Windows.Forms;

namespace NexOS.WinHost
{
    internal static class Program
    {
        /// <summary>
        ///  The main entry point for the application.
        /// </summary>
        [STAThread]
        static void Main(string[] args)
        {
            // To customize application configuration such as set high DPI
            // settings or default font, see https://aka.ms/applicationconfiguration.
            ApplicationConfiguration.Initialize();
            Application.Run(new ShellForm(args));
        }
    }
}
