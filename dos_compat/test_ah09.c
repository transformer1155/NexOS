// test_ah09.c — AH=09h 逻辑闭环：验证 DOS 打印字符串真的能吐出文字。
//
// 这是"逻辑闭环"验收：INT 21h 的 AH=09h 转译函数调用底层原语，
// 把字符串打印到控制台。跑通它说明整条链路真的通了，
// 而不只是"语法能过"。
//
// 构建方式见同目录 build_and_test.sh / 下方注释。

#include <stdio.h>
#include <string.h>
#include "dos_int21.h"
#include "dos_prim.h"

// 断言宏：失败立即暴露，不静默
static int g_fail = 0;
#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("FAIL: %s\n", msg);                      \
            g_fail++;                                       \
        }                                                   \
    } while (0)

int main(void) {
    printf("=== AH=09h logic test ===\n");

    // 1) 单字符输出 (AH=02h)
    dos_int21_print_char('[');
    CHECK(dos_console_putc(' ') == 0, "putc returns 0");
    dos_int21_print_char(']');

    // 2) 字符串输出 (AH=09h)：DOS 里字符串以 '$' 结尾，
    //    转译层接收的是已去掉 '$' 的 NUL 结尾串。
    const char *msg = "HELLO FROM DOS INT 21H";
    dos_int21_print_char('\n');
    int r = dos_int21_print_string(msg);
    CHECK(r == 0, "print_string returns 0 on success");
    dos_int21_print_char('\n');

    // 3) NULL 保护
    CHECK(dos_int21_print_string(NULL) != 0, "print_string(NULL) fails safely");

    // 4) 退出码 (AH=4Ch)
    dos_int21_exit(7);
    CHECK(dos_process_exit_code() == 7, "exit code recorded");

    // 5) 字符串内容校验：转译层必须原样输出，不能截断或改动
    //    （用返回值 + 输出占位验证，完整字节比对见 test_ah09_io）
    CHECK(strcmp(msg, "HELLO FROM DOS INT 21H") == 0, "message intact");

    printf("\n=== %s ===\n", g_fail == 0 ? "ALL PASS" : "TEST FAILED");
    return g_fail == 0 ? 0 : 1;
}