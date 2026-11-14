// =====================================================================
//  PluginMain.cs  -  .nexbundle 插件的 MiniCLR 入口
// ---------------------------------------------------------------------
//  bundle 安装器把 plugin.json 的 entry.symbol 映射到这两个静态方法：
//
//      Init()                          -> 加载 .mex 后调用一次
//      Dispatch(tool, args_json)       -> 每次 MCP tools/call 调用
//
//  内核插件加载器（plugins/loader/plugin_loader.cpp）的入口是原生函数
//  指针 entry_point；托管插件没有函数指针，所以由 bundle 宿主做桥接：
//
//      pl_call_fn  ->  PluginMain::Dispatch(tool, args)
//
//  返回值约定与 docs/MCP_PLUGIN_PROTOCOL.md §6.3 一致：
//      {"code":0,"data":{...}}  成功
//      {"code":N,"error":"..."}  失败（N 为插件自定义业务码）
// =====================================================================
namespace SysInfo
{
    public static class PluginMain
    {
        // MiniCLR 没有静态构造函数；懒加载放在 Init() 里。
        static SysInfoTools s_tools;

        public static void Init()
        {
            if (s_tools == null)
            {
                s_tools = new SysInfoTools();
                s_tools.OnInit();
            }
        }

        public static string Dispatch(string tool_name, string json_args)
        {
            Init();
            return s_tools.Invoke(tool_name, json_args);
        }

        // 任务栏 / 窗口标题。UI 也可以从 ui/layout.json 读，二者保持一致。
        public static string Title()
        {
            return "系统信息";
        }
    }
}
