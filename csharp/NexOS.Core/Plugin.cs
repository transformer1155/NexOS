// NexOS.Plugin -- managed surface for the microkernel plugin bus.
//
// Kernel MiniCLR build: the two entry points are InternalCalls registered by
//   the kernel itself in .attic64/kernel64.cpp (plugin_core_try_boot ->
//   clr_register_icall "NexOS.Plugin::Dispatch" / "NexOS.Plugin::Count"),
//   implemented in plugins/loader/plugin_bridge.cpp.
//
// WinHost build (#if WINHOST): there is no kernel plugin bus, so the same
//   names are backed by inert stubs.  Keeping the surface identical lets
//   shared app code compile on both sides without #if sprinkling.
//
// Contract (kernel side, osapi_ic_plugin_dispatch):
//   a[0] = plugin handle (int)      -- from a load performed by the kernel
//   a[1] = tool name  (string)      -- "fs.read", "evil.entry", ...
//   a[2] = args JSON  (string)      -- {"path":"/sandbox/app/notes.txt"}
//   returns a JSON string; on failure the string is a JSON error envelope
//   AND osapi_get_last_error() carries the mapped ERR_* code.
//
// Managed code never sees a plugin function pointer, a sandbox path prefix
// or a file descriptor: every capability request goes through this one call
// and is re-checked by dispatch_mcp() (path sandbox, user pointers, caps,
// inflight quota) before anything runs.
namespace NexOS.Plugin
{
    public static class Bus
    {
#if !WINHOST
        // ---- kernel MiniCLR: bare InternalCalls only ----
        // Dispatch one MCP tool call against plugin `handle`.  Never throws
        // in the kernel interpreter: inspect the returned JSON and/or
        // NexOS.Sys.GetLastError() for the failure reason.
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
        public static extern string Dispatch(int handle, string tool, string argsJson);

        // Number of live plugins in the core's slot table.
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
        public static extern int Count();
#else
        // ---- winhost: no kernel plugin bus ----
        public static string Dispatch(int handle, string tool, string argsJson)
        {
            return "{\"error\":{\"code\":-3,\"message\":\"no plugin bus on WinHost\"}}";
        }

        public static int Count()
        {
            return 0;
        }
#endif

        // Convenience: a call with no arguments.  Kept here (not an
        // InternalCall) so the kernel only ever registers two names.
        public static string Dispatch(int handle, string tool)
        {
            return Dispatch(handle, tool, "{}");
        }
    }
}
