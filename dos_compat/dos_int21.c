// DOS INT 21h translation layer — minimal subset
// 全部函数体由 micro-task-pipeline 按最小上下文生成后拼回。
#include <stdint.h>
#include "dos_int21.h"
#include "dos_prim.h"
#include "vfs.h"

// AH=02h: 输出单个字符到控制台
// @micro-task
int dos_int21_print_char(int ch) {
    return dos_console_putc(ch);
}

// AH=09h: 输出以 NUL 结尾的字符串（调用方已去掉 DOS 的 '$' 终止符）
// @micro-task
int dos_int21_print_string(const char *s) {
    if (!s) return -1;
    while (*s) dos_console_putc(*s++);
    return 0;
}

// AH=4Ch: 终止程序，code 为退出码
// @micro-task
int dos_int21_exit(int code) {
    return dos_process_exit(code);
}

// AH=01h: 读取一个字符并回显；无输入时返回 -1
// @micro-task
int dos_int21_read_char_echo(void) {
    return dos_console_getc();
}

// AH=3Dh: 打开文件，mode 为 DOS 访问模式位；成功返回句柄(>=0)
// 提示：可通过 vfs_open(path, flags) 实现，flags 使用 VFS_ACC_READ/VFS_ACC_WRITE
// @micro-task
int dos_int21_open_file(const char *name, int mode) {
    int flags = (mode & 0x3) ? VFS_ACC_READ : VFS_ACC_READ;
    return vfs_open(name, flags);
}

// AH=3Fh: 从句柄读取 count 字节；实际读取数写入 *bytes_read
// 提示：vfs_read(fd, buf, count) 返回读取字节数
// @micro-task
int dos_int21_read_file(int handle, void *buf, int count, int *bytes_read) {
    int n = vfs_read(handle, buf, count);
    if (n < 0) return -1;
    if (bytes_read) *bytes_read = n;
    return 0;
}

// AH=40h: 向句柄写入 count 字节；实际写入数写入 *bytes_written
// @micro-task
int dos_int21_write_file(int handle, const void *buf, int count, int *bytes_written) {
    int n = vfs_write(handle, buf, count);
    if (n < 0) return -1;
    if (bytes_written) *bytes_written = n;
    return 0;
}

// AH=3Eh: 关闭句柄
// @micro-task
int dos_int21_close_file(int handle) {
    return vfs_close(handle);
}

// AH=3Ch: 创建文件，attr 为 DOS 属性字节；成功返回句柄(>=0)
// 注意：SFS 引导介质只读，vfs_create 在只读介质上会失败(-1)
// @micro-task
int dos_int21_create_file(const char *name, int attr) {
    (void)attr;
    return vfs_create(name, 0x40);
}