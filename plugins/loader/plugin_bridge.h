/* =====================================================================
 *  plugin_bridge.h  -  内核 ↔ 插件核心 ↔ 托管宿主(C#) 的接线层
 * ---------------------------------------------------------------------
 *  这一层只做三件事：
 *
 *   1. 开机自检      pc_bridge_boot()
 *      - pc_core_init()，把串口当 emit hook；
 *      - 现场加载一个**真实的未签名恶意插件**（malicious_plugin.cpp，
 *        与宿主破坏性测试同一个目标文件），让它自己撞指针闸门/沙盒闸门；
 *      - 把每次拦截打到 COM1，最后打印 passed/failed。
 *
 *   2. 托管系统调用  osapi_ic_plugin_dispatch(int32_t* a)
 *      a[0] = 插件句柄，a[1] = 工具名(托管字符串)，a[2] = args JSON(托管字符串)
 *      返回 = 托管字符串（JSON 结果；失败时是 JSON-RPC 2.0 error）
 *      内核侧 icall 名字：NexOS.Plugin::Dispatch
 *      注册点：.attic64/kernel64.cpp（弱符号，未启用插件核心时不注册）
 *
 *   3. 共享缓冲      pc_bridge_result_buffer()
 *      托管侧若需要零拷贝读回，可直接拿这块 PC_MAX_OUT 字节的静态缓冲；
 *      它是 icall 的同一块内存，写完即可读。
 *
 *  线程模型
 *  -------------------------------------------------------------------
 *  icall 只在 GUI 线程被调用（MiniCLR 是单线程解释器），所以结果缓冲
 *  是 static，不加锁 —— 与既有 osapi_ic_fs_glob / osapi_ic_ai_* 一致。
 *  真正的并发安全由 plugin_core 的自旋锁 + 引用计数负责。
 * ===================================================================== */
#ifndef NEX_PLUGIN_BRIDGE_H
#define NEX_PLUGIN_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 开机初始化 + 内核侧破坏性自检（打印到 COM1）。可重复调用（幂等）。 */
void pc_bridge_boot(void);

/* 托管系统调用入口。返回托管字符串句柄；0 = 失败（已 osapi_set_last_error）。 */
int32_t osapi_ic_plugin_dispatch(int32_t* a);

/* 当前插件数量 / 句柄是否存活（供 shell / 诊断用）。 */
int32_t osapi_ic_plugin_count(int32_t* a);
int32_t osapi_ic_plugin_alive(int32_t* a);

/* icall 使用的静态结果缓冲（PC_MAX_OUT 字节，NUL 结尾）。 */
const char* pc_bridge_result_buffer(void);

/* 内核侧自检结果：0 = 全过，>0 = 失败项。 */
int pc_bridge_last_failures(void);

#ifdef __cplusplus
}
#endif
#endif /* NEX_PLUGIN_BRIDGE_H */
