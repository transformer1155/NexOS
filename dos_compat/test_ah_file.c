// test_ah_file.c — AH=3Ch/3Dh/3Fh/40h/3Eh 文件操作逻辑闭环
//
// 直连真实 NexOS vfs.cpp（-DVFS_HOST_TEST），SFS 仍只读，写入走 RAMFS overlay。
// 用 vfs_create/vfs_write 预置可读文件，去除对已退役的 dos_stub_seed 的依赖。
#include <stdio.h>
#include <string.h>
#include "dos_int21.h"
#include "dos_prim.h"
#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif
void host_vfs_init(void);
void host_set_identity(int pid, int uid, int ring, const char* root);
#ifdef __cplusplus
}
#endif

static int g_fail = 0;
#define CHECK(cond, msg)                        \
    do {                                        \
        if (!(cond)) {                          \
            printf("FAIL: %s\n", msg);          \
            g_fail++;                           \
        } else {                                \
            printf("ok  : %s\n", msg);          \
        }                                       \
    } while (0)

int main(void) {
    printf("=== AH=3Ch/3Dh/3Fh/40h/3Eh file logic test ===\n");

    // 初始化真实 VFS（SFS+RAMFS）
    host_vfs_init();
    host_set_identity(1, 0, 0, "/"); // ring 0 bypass sandbox for test

    // ---- 用 RAMFS 预置一个可读文件（替代已退役的 dos_stub_seed） ----
    {
        int c = vfs_create("READ.TXT", 0);
        CHECK(c >= 0, "seed READ.TXT via vfs_create");
        if (c >= 0) {
            const char *seed = "abcdef";
            int wn = vfs_write(c, seed, (int)strlen(seed));
            CHECK(wn == 6, "seed READ.TXT via vfs_write");
            int cc = vfs_close(c);
            CHECK(cc == 0, "close seeded READ.TXT");
        }
    }

    // ---- AH=3Ch create -> AH=40h write -> AH=3Eh close ----
    int h = dos_int21_create_file("TEST.TXT", 0x00);
    CHECK(h >= 0, "AH=3Ch create returns a handle");

    const char *payload = "written via INT 21h AH=40h";
    int written = 0;
    int w = dos_int21_write_file(h, payload, (int)strlen(payload), &written);
    CHECK(w == 0, "AH=40h write returns success");
    CHECK(written == (int)strlen(payload), "AH=40h reports full byte count");

    int c2 = dos_int21_close_file(h);
    CHECK(c2 == 0, "AH=3Eh close returns success");

    // ---- AH=3Dh open -> AH=3Fh read ----
    int h2 = dos_int21_open_file("READ.TXT", 0x00);
    CHECK(h2 >= 0, "AH=3Dh open returns a handle");

    char buf[16];
    memset(buf, 0, sizeof(buf));
    int got = 0;
    int r = dos_int21_read_file(h2, buf, 6, &got);
    CHECK(r == 0, "AH=3Fh read returns success");
    CHECK(got == 6, "AH=3Fh reports 6 bytes");
    CHECK(memcmp(buf, "abcdef", 6) == 0, "AH=3Fh read the correct bytes");

    dos_int21_close_file(h2);

    // ---- 错误路径 ----
    int bad = dos_int21_open_file("NOPE.MISSING", 0x00);
    CHECK(bad < 0, "AH=3Dh open of missing file fails");

    int badc = dos_int21_close_file(999);
    CHECK(badc != 0, "AH=3Eh close of invalid handle fails");

    printf("\n=== %s ===\n", g_fail == 0 ? "ALL PASS" : "TEST FAILED");
    return g_fail == 0 ? 0 : 1;
}