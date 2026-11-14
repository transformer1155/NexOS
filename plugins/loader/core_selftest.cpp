/* core_selftest.cpp - 在宿主上运行 plugin_core 的模块五自测。
 *
 * 只做四件事：
 *   1) 提供 osapi 错误通道的宿主桩（内核里由 osapi.cpp 提供）；
 *   2) 把串口输出重定向到 stdout；
 *   3) 调用 run_core_selftest()；
 *   4) 退出码 = 失败用例数。
 *
 * 注意：这个文件是测试脚手架，可以用 stdio；plugin_core.cpp 本身
 * 仍然保持 freestanding，不含任何标准库头。
 */
#include "plugin_core.h"
#include "osapi.h"

#include <stdio.h>

static int g_host_last_error = ERR_OK;

int osapi_set_last_error(int code)
{
    g_host_last_error = code;
    return code;
}

int osapi_get_last_error(void)
{
    return g_host_last_error;
}

void osapi_clear_last_error(void)
{
    g_host_last_error = ERR_OK;
}

static void host_emit(const char* s)
{
    if (!s) return;
    fputs(s, stdout);
    fflush(stdout);
}

int main(void)
{
    pc_set_emit_hook(host_emit);
    return run_core_selftest();
}
