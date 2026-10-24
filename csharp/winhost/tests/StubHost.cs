// NetStack.cs only touches one harness symbol (the bus diagnostic logger).
// In WinHost that is NexOS.Forms.Host.Log; the functional harness routes it to
// the console instead of dragging in the entire WinForms backend.
namespace NexOS.Forms
{
    public static class Host
    {
        public static void Log(string s) { System.Console.WriteLine("[host] " + s); }
    }
}
