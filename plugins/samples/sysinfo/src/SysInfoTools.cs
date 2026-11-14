// =====================================================================
//  SysInfoTools.cs  -  “系统信息”插件（MiniCLR / McpServerBase）
// ---------------------------------------------------------------------
//  这是 stages 4 的完整示例：一个只读插件，向 MCP 总线暴露两个工具：
//
//      sys_info  -> CPU 占用 / 内存 / OS / 厂商 / 位数 / uptime
//      mem_info  -> 物理页与 CLR 堆的细分数据
//
//  MiniCLR 约束（违反任何一条都会让 mex_pack / 内核加载失败）：
//    * 无静态构造函数     -> 状态只在 OnInit() 里初始化
//    * 无泛型/集合        -> 只用 int/string
//    * 无异常             -> 所有失败都返回 {"code":N,"error":"..."}
//    * 无浮点             -> 百分比用整数（0..100）
//    * 无 string '+'      -> 一律走 NexOS.Sys.StrConcat（U.Cat 同源）
//    * 堆按帧回绕         -> 不在循环里拼接字符串
//
//  数据来源全部是 NexOS.Forms.Host 的 InternalCall，只读：
//      Host.CpuLoad()     0..100（GUI 循环的 TSC 空闲核算）
//      Host.PagesUsed()   已用物理页（4 KiB/页）
//      Host.PagesTotal()  物理页总数
//      Host.OsName()      内核报告的系统名
//      Host.CpuVendor()   CPU 厂商串
//      Host.Is64Bit()     1/0
//      Host.TickMs()      单调毫秒（约 49 天回绕）
//
//  本文件由 tools/misc/mcp_scan.py 扫描 [McpTool]，生成
//  SysInfoTools.Dispatch.g.cs（Invoke 的 partial 实现）。
// =====================================================================
using System;
using NexOS.Mcp;
using NexOS.Forms;

namespace SysInfo
{
    public sealed partial class SysInfoTools : McpServerBase
    {
        // MCP 总线身份。必须等于 bundle 里的 plugin id 与 .mex basename。
        public override string AppId { get { return "sysinfo"; } }

        // MiniCLR 没有静态构造函数：一次性的常量/缓存放这里。
        public override void OnInit()
        {
            // 本插件无持久状态。保留方法是为了让 SDK 合同完整。
        }

        // -----------------------------------------------------------------
        //  工具 1：sys_info
        // -----------------------------------------------------------------
        //  args 允许一个可选参数 "verbose": true。
        //  返回 {"code":0,"data":{...}}；bundle 宿主把 data 映射到 MCP 的
        //  structuredContent（见 docs/MCP_PLUGIN_PROTOCOL.md §6.3）。
        [McpTool("sys_info",
                 Description = "Read-only snapshot of CPU load, memory and OS identity.")]
        public static string GetSysInfo(string json_args)
        {
            int load      = Host.CpuLoad();
            int mem_total = Host.MemTotalKb();
            int pages_tot = Host.PagesTotal();
            int pages_use = Host.PagesUsed();

            // 4 KiB/页；先乘页数再右移，避免中间值溢出（页数 * 4 <= 32 位）。
            int mem_used = pages_use * 4;
            int mem_free = mem_total - mem_used;
            if (mem_free < 0) mem_free = 0;

            int used_pct = 0;
            if (pages_tot > 0) used_pct = (pages_use * 100) / pages_tot;

            string body = Cat("{\"cpu_load\":", I(load));
            body = Cat(body, Cat(",\"mem_total_kb\":", I(mem_total)));
            body = Cat(body, Cat(",\"mem_used_kb\":", I(mem_used)));
            body = Cat(body, Cat(",\"mem_free_kb\":", I(mem_free)));
            body = Cat(body, Cat(",\"mem_used_pct\":", I(used_pct)));
            body = Cat(body, Cat(",\"pages_total\":", I(pages_tot)));
            body = Cat(body, Cat(",\"pages_used\":", I(pages_use)));
            body = Cat(body, Cat(",\"uptime_ms\":", I(Host.TickMs())));
            body = Cat(body, Cat(",\"is_64bit\":", I(Host.Is64Bit())));
            body = Cat(body, ",\"os\":\"");
            body = Cat(body, Cat(Escape(Host.OsName()), "\""));
            body = Cat(body, ",\"cpu_vendor\":\"");
            body = Cat(body, Cat(Escape(Host.CpuVendor()), "\""));
            body = Cat(body, "}");

            return Ok(body);
        }

        // -----------------------------------------------------------------
        //  工具 2：mem_info
        // -----------------------------------------------------------------
        [McpTool("mem_info",
                 Description = "Physical page counters and the CLR bump-heap high-water marks.")]
        public static string GetMemInfo(string json_args)
        {
            int pages_tot = Host.PagesTotal();
            int pages_use = Host.PagesUsed();
            int pages_free = Host.PagesFree();
            int heap_alloc = Host.HeapAlloc();
            int heap_free  = Host.HeapFree();

            string body = Cat("{\"pages_total\":", I(pages_tot));
            body = Cat(body, Cat(",\"pages_used\":", I(pages_use)));
            body = Cat(body, Cat(",\"pages_free\":", I(pages_free)));
            body = Cat(body, Cat(",\"page_size_kb\":", I(4)));
            body = Cat(body, Cat(",\"heap_alloc_bytes\":", I(heap_alloc)));
            body = Cat(body, Cat(",\"heap_free_bytes\":", I(heap_free)));
            body = Cat(body, Cat(",\"heap_alloc_count\":", I(Host.HeapAllocCnt())));
            body = Cat(body, Cat(",\"heap_free_count\":", I(Host.HeapFreeCnt())));
            body = Cat(body, "}");
            return Ok(body);
        }

        // ---- 最小字符串辅助（MiniCLR 没有 StringBuilder） ----------------
        static string Cat(string a, string b)
        {
            return NexOS.Sys.StrConcat(a, b);
        }

        static string I(int v)
        {
            return NexOS.Sys.IntToStr(v);
        }
    }
}
