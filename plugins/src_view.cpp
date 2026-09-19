/* src_view.cpp - Phase 3: view plugin source stored in MKFS.
 * Registers "src.view": pass a filename (args = const char*) and it vfs-reads
 * the .skill file and prints it to serial.  Proves source is readable in NexOS.
 */
#include "plugin_manager.h"

int  vfs_open(const char* path, int flags);
int  vfs_read(int fd, void* buf, int count);
int  vfs_close(int fd);

#define SRC_VIEW_CAP 1024

static int src_view(void* args, void* out, int outcap) {
    (void)out; (void)outcap;
    const char* name = (const char*)args;
    if (!name) return -1;
    int fd = vfs_open(name, 0);
    if (fd < 0) { pm_serial("[SRC] not found: "); pm_serial(name); pm_serial("\n"); return -1; }
    uint8_t buf[SRC_VIEW_CAP];
    int n = vfs_read(fd, buf, SRC_VIEW_CAP - 1);
    vfs_close(fd);
    if (n <= 0) { pm_serial("[SRC] empty: "); pm_serial(name); pm_serial("\n"); return 0; }
    pm_serial("[SRC] "); pm_serial(name); pm_serial(":\n");
    for (int i = 0; i < n; i++) pm_serial_byte(buf[i]);
    pm_serial("\n[SRC] --- end ---\n");
    return n;
}

static int src_view_init(Plugin* self) {
    (void)self;
    svc_register("src.view", src_view);
    return 0;
}
static void src_view_exit(Plugin* self) { (void)self; svc_unregister("src.view"); }
static int src_view_call(Plugin* self, const char* m, void* a, void* o, int c) {
    (void)self; (void)m; (void)a; (void)o; (void)c; return -1;
}
extern const Plugin g_src_view = {
    "src_view", 0x0100, "src.view", "", src_view_init, src_view_exit, src_view_call
};
