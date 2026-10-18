// =====================================================================
//  McpSdk.cs  -  NexOS MCP SDK for third-party applications
// ---------------------------------------------------------------------
//  第三方开发者写一个 MCP 服务端的全部工作是：
//
//      public sealed class WeatherTools : McpServerBase
//      {
//          public override string AppId { get { return "weather"; } }
//
//          [McpTool("get_weather", Description = "Return current weather.")]
//          public static string GetWeather(string json_args)
//          {
//              return "{\"temp\":21,\"loc\":\"Beijing\"}";
//          }
//      }
//
//  然后 `python tools/mcp_scan.py` 在**构建期**扫描 [McpTool] 方法，
//  生成 manifest.json 的 mcp_server[] 与一个 Invoke 派发表。系统据此
//  把接口暴露给 MCP 总线 —— 全程不需要运行时反射。
//
//  为什么是构建期扫描而不是运行时反射？
//    nexos_ai.h 的 McpTool 结构要求 tool_name/description/schema/dispatch
//    在 AppRegistry 注册时就是确定的；而 MiniCLR 把整个程序集扁平化成
//    数组（tools/mex_pack.py），没有 ECMA-335 元数据可供反射。所以工具
//    清单必须在打包前算出，随 manifest.json 一起进包。
//
//  MiniCLR 约束（务必遵守，否则内核构建失败）：
//    * 无静态构造函数 —— 状态在 OnInit() 里初始化
//    * 无 ref/out 参数、无浮点数
//    * 字符串是 UTF-8 字节数组；不要用 '+' 拼接（用 U.Cat / 常量串联）
//    * 堆按帧回绕，不要长期持有大对象
// =====================================================================
// System is needed in BOTH builds: [McpTool] derives from System.Attribute,
// and MiniCLR's Corelib.cs provides Attribute / AttributeUsage there too.
using System;

namespace NexOS.Mcp
{
    // -----------------------------------------------------------------
    //  [McpTool] - 标记一个静态方法为对 AI / 其它应用暴露的 MCP 工具
    // -----------------------------------------------------------------
    //  Name        : 工具名（snake_case）。为 null 时由 mcp_scan.py 从方法名
    //                推导（GetWeather -> get_weather）。
    //  Description : 给 LLM 看的说明。强烈建议填写，否则模型难以正确选用。
    //  Schema      : 可选的 JSON Schema 字面量；留空则由扫描器从
    //                McpParam 特性 / 方法签名的字符串参数推导。
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public sealed class McpToolAttribute : Attribute
    {
        public string Name;
        public string Description;
        public string Schema;

        public McpToolAttribute(string name) { Name = name; }
        public McpToolAttribute() { Name = null; }
    }

    // -----------------------------------------------------------------
    //  [McpParam] - 描述一个参数（可选，用于自动生成 parameters_schema）
    // -----------------------------------------------------------------
    [AttributeUsage(AttributeTargets.Parameter, AllowMultiple = false)]
    public sealed class McpParamAttribute : Attribute
    {
        public string Name;
        public string Description;
        public bool Required;

        public McpParamAttribute(string name) { Name = name; Required = true; }
        public McpParamAttribute() { Name = null; Required = true; }
    }

    // -----------------------------------------------------------------
    //  McpServerBase - 第三方 MCP 服务端基类
    // -----------------------------------------------------------------
    //  子类只需：
    //    1. 覆写 AppId 返回自己的 app_id（必须等于 .mex basename）
    //    2. 用 [McpTool] 标记静态工具方法
    //    3. （可选）覆写 OnInit 做一次性初始化
    //
    //  Invoke 的派发实现由 tools/mcp_scan.py --emit-dispatch 生成。
    //  生成的是一个 **partial class**，与用户的 McpServerBase 子类同名：
    //
    //      namespace Weather
    //      {
    //          public sealed partial class WeatherTools
    //          {
    //              public override string Invoke(string tool_name, string json_args)
    //              {
    //                  if (tool_name == "get_weather") return GetWeather(json_args);
    //                  ...
    //              }
    //          }
    //      }
    //
    //  因此子类**必须**声明为 `partial`（见 docs/NEXPACK.md §7）。
    //  这样开发者不必手写 if-else 派发链，也不需要运行时反射。
    public abstract class McpServerBase
    {
        // 本应用在 MCP 总线上的身份。内核会用常驻 .mex 的 basename 校验，
        // 所以这里返回的值必须与文件名一致，否则 ACL 拒绝。
        public abstract string AppId { get; }

        // 一次性初始化（MiniCLR 没有静态构造函数，子类在这里分配静态状态）。
        public virtual void OnInit() { }

        // 工具派发入口。工具名 -> 具体方法。
        // 实现由 tools/mcp_scan.py 生成的 partial 文件提供。
        // 返回 JSON 字符串；失败返回 {"code":N,"error":"..."}。
        public abstract string Invoke(string tool_name, string json_args);

        // ---- 工具作者可用的辅助方法 --------------------------------
        // MiniCLR 的 String 极其精简：只有 Concat / 索引器 / == （见
        // Corelib.cs），没有 StringBuilder、没有 char[] 构造函数、没有
        // instance Substring。所有字符串操作都走 NexOS.Sys 的 InternalCall：
        //   Sys.StrConcat / Sys.StrSub / Sys.StrLen / Sys.StrCharAt / Sys.StrEq
        // 这几个在 MiniCLR（Sys.cs 声明）和 WinHost（Backend.cs 实现）里
        // 签名一致，所以本 SDK 在三种构建下都能编译：
        //   * 内核 MiniCLR 构建
        //   * WinHost 预览构建
        //   * 第三方应用独立构建（需要引用 NexOS.Core/Sys.cs）
        //
        // 性能注意：不要在**循环**里用 StrConcat 逐字符累积 —— 每次都在
        // bump 堆上新建对象，O(n^2) 会耗尽帧堆（Lang.cs 头部有血泪注释）。
        // 下面所有实现都保证拼接次数是 O(1) 或 O(定长小数字)。

        static string Cat(string a, string b) { return NexOS.Sys.StrConcat(a, b); }

        public static string Ok(string json_body)
        {
            if (json_body == null) return "{\"code\":0}";
            return Cat("{\"code\":0,\"data\":", Cat(json_body, "}"));
        }

        public static string Error(int code, string message)
        {
            return Cat(Cat("{\"code\":", IntToStr(code)),
                       Cat(",\"error\":\"", Cat(Escape(message), "\"}")));
        }

        // JSON 字符串转义。
        // 快速路径零分配：不需要转义时直接返回原串。
        // 慢路径按「安全段」切片，拼接次数 = 命中数 + 1。
        public static string Escape(string s)
        {
            if (s == null) return "";
            int n = s.Length;
            bool need = false;
            for (int i = 0; i < n; i++)
            {
                char c = s[i];
                if (c == '"' || c == '\\' || c == '\n' || c == '\r' ||
                    c == '\t' || c < 0x20) { need = true; break; }
            }
            if (!need) return s;

            string out_s = "";
            int runStart = 0;
            for (int i = 0; i < n; i++)
            {
                char c = s[i];
                string rep = null;
                if (c == '"') rep = "\\\"";
                else if (c == '\\') rep = "\\\\";
                else if (c == '\n') rep = "\\n";
                else if (c == '\r') rep = "\\r";
                else if (c == '\t') rep = "\\t";
                else if (c < 0x20) rep = " ";
                if (rep == null) continue;

                // 安全段 [runStart, i) 一次性切出来（Sys.StrSub 是原生实现）
                if (i > runStart)
                    out_s = Cat(out_s, NexOS.Sys.StrSub(s, runStart, i - runStart));
                out_s = Cat(out_s, rep);
                runStart = i + 1;
            }
            if (runStart < n)
                out_s = Cat(out_s, NexOS.Sys.StrSub(s, runStart, n - runStart));
            return out_s;
        }

        // 数字字符表：用它切出单字符字符串（StrSub 一次调用，无循环拼接）。
        const string DIGITS = "0123456789";

        static string IntToStr(int v)
        {
            if (v == 0) return "0";
            bool neg = v < 0;
            // 注意：int.MinValue 取负会溢出，这里按 unsigned 语义处理。
            long u = v;
            if (neg) u = -u;

            // 先取十进制位（最多 20 位，long 足够）。
            int[] d = new int[20];
            int k = 0;
            while (u > 0) { d[k++] = (int)(u % 10); u = u / 10; }

            string s = neg ? "-" : "";
            for (int i = k - 1; i >= 0; i--)
                s = Cat(s, NexOS.Sys.StrSub(DIGITS, d[i], 1));
            return s;
        }
    }
}
