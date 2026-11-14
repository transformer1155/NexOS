/* host_selftest.cpp - 在 Linux/Windows 宿主上运行内核加载器的自测。
 *
 * 只做一件事：调用 run_selftest()，进程退出码 = 失败用例数。
 * 因为 plugin_loader.cpp 在 NEX_PL_HOST_TEST 下把 kmalloc/serial_puts
 * 换成了 malloc/fputs，所以同一份策略代码可在宿主上逐条验证。
 */
#include "plugin_loader.h"

int main(void)
{
    return run_selftest();
}
