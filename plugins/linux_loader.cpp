/* linux_loader plugin - boot a real Linux kernel (bzImage) as a second kernel.
 *
 * This is NOT a facade: init() reports what it can find, and call("boot")
 * actually chain-loads Linux.  The heavy lifting (parse bzImage, build the
 * zero page, drop paging, far-jump) belongs to the 32-bit loader and is
 * reached through two kernel-provided primitives so there is exactly one
 * implementation of the trampoline:
 *
 *     kern_linux_probe()   - read vmlinuz/initrd.img sizes from SFS
 *     kern_linux_boot()    - perform the kexec() handoff (never returns)
 *
 * The plugin owns the POLICY: which files, which command line, whether an
 * initrd is used, and whether the current CPU mode can host a Linux entry
 * (Linux's 32-bit entry needs protected mode with paging OFF, so the
 * chain-load is refused in long mode).
 *
 * Services registered:
 *   linux.boot     - args: char* cmdline suffix (may be null), out unused
 *   linux.probe    - reports vmlinuz/initrd sizes via out
 *   linux.cmdline  - returns the default kernel command line into out
 *
 * deps: none.  Freestanding: no stdlib, no new/delete, no exceptions.
 */
#include "plugin_manager.h"

/* ---- kernel primitives (defined in kernel.cpp) ------------------------- */
extern "C" int kern_linux_probe(int* vmlinuz_size, int* initrd_size,
                                int* can_boot, int* is_long_mode);
extern "C" int kern_linux_boot(const char* cmdline_extra, int skip_initrd);
extern "C" const char* kern_linux_default_cmdline(void);

/* The default command line is deliberately conservative: serial console so
 * the guest kernel's boot log lands in the same QEMU -serial stream as ours,
 * and no ACPI/APIC so it does not fight NexOS's own interrupt setup. */
#define LINUX_DEFAULT_CMDLINE \
    "console=ttyS0,115200n8 earlycon=uart8250,io,0x3f8,115200 " \
    "ignore_loglevel loglevel=8 acpi=off maxcpus=1 noapic nokaslr"

static int g_linux_boots = 0;      /* successful handoffs attempted */
static int g_last_vmlinuz = -1;    /* bytes seen at last probe        */
static int g_last_initrd  = -1;

/* ---- linux.probe ------------------------------------------------------- */
static int linux_probe_svc(void* args, void* out, int outcap) {
    (void)args;
    int vsz = -1, isz = -1, can = 0, lmode = 0;
    int r = kern_linux_probe(&vsz, &isz, &can, &lmode);
    g_last_vmlinuz = vsz;
    g_last_initrd  = isz;
    /* Pack a short human-readable report so `plugin pm call linux_loader probe`
     * says something useful.  Sizes are reported as present/absent: SFS read()
     * caps the copy at the caller's buffer, so a cheap probe cannot know the
     * true file size (kern_linux_boot logs it from the full image). */
    if (out && outcap > 0) {
        char* o = (char*)out;
        int n = 0;
        const char* p;
        p = "vmlinuz=";
        while (*p && n < outcap - 1) o[n++] = *p++;
        p = (vsz > 0) ? "valid-bzImage" : "missing";
        while (*p && n < outcap - 1) o[n++] = *p++;
        p = " initrd=";
        while (*p && n < outcap - 1) o[n++] = *p++;
        p = (isz > 0) ? "present" : "none";
        while (*p && n < outcap - 1) o[n++] = *p++;
        p = can ? " bootable=yes" : " bootable=no";
        while (*p && n < outcap - 1) o[n++] = *p++;
        p = lmode ? " mode=long" : " mode=protected";
        while (*p && n < outcap - 1) o[n++] = *p++;
        o[n] = 0;
    }
    return r;
}

/* ---- linux.cmdline ----------------------------------------------------- */
static int linux_cmdline_svc(void* args, void* out, int outcap) {
    (void)args;
    const char* cl = kern_linux_default_cmdline();
    if (!cl) cl = LINUX_DEFAULT_CMDLINE;
    if (out && outcap > 0) {
        char* o = (char*)out;
        int n = 0;
        while (cl[n] && n < outcap - 1) { o[n] = cl[n]; n++; }
        o[n] = 0;
        return n;
    }
    return 0;
}

/* ---- linux.boot -------------------------------------------------------- */
/* args: if non-null and non-empty, a NUL-terminated string of extra kernel
 *       parameters appended to the default command line.
 * out:  optional int[2] = {vmlinuz_present(1/-1), initrd_present(1/-1)}
 * Returns non-zero if the handoff was refused (see serial log); on success it
 * never returns because the CPU is now running Linux. */
static int linux_boot_svc(void* args, void* out, int outcap) {
    const char* extra = (const char*)args;

    int vsz = -1, isz = -1, can = 0, lmode = 0;
    kern_linux_probe(&vsz, &isz, &can, &lmode);
    g_last_vmlinuz = vsz;
    g_last_initrd  = isz;

    if (out && outcap >= (int)(2 * sizeof(int))) {
        ((int*)out)[0] = vsz > 0 ? 1 : -1;
        ((int*)out)[1] = isz > 0 ? 1 : -1;
    }

    if (vsz <= 0) {
        pm_serial("[LX] boot refused: no valid vmlinuz in SFS\n");
        return -1;
    }
    if (!can) {
        /* Linux's 32-bit entry demands protected mode with paging off. */
        pm_serial("[LX] boot refused: CPU not in 32-bit protected mode\n");
        return -1;
    }

    /* kern_linux_boot logs the real bzImage size once it has the full image. */
    pm_serial(isz > 0 ? "[LX] booting Linux (initrd present)\n"
                      : "[LX] booting Linux (no initrd)\n");
    if (extra && *extra) { pm_serial("[LX] cmdline extra: "); pm_serial(extra); pm_serial("\n"); }

    g_linux_boots++;
    /* Never returns on success: kexec_enter far-jumps into Linux startup_32. */
    int r = kern_linux_boot(extra, 0);
    /* Only reached if the handoff was refused. */
    pm_serial("[LX] handoff returned; Linux did not take control\n");
    return r ? r : -1;
}

/* ---- method dispatch --------------------------------------------------- */
static const char* lx_svc_name(const char* method) {
    if (!pm_strcmp(method, "boot") || !pm_strcmp(method, "kexec")) return "linux.boot";
    if (!pm_strcmp(method, "probe") || !pm_strcmp(method, "status")) return "linux.probe";
    if (!pm_strcmp(method, "cmdline")) return "linux.cmdline";
    return 0;
}

static int lx_init(Plugin* self) {
    (void)self;
    svc_register("linux.boot", linux_boot_svc);
    svc_register("linux.probe", linux_probe_svc);
    svc_register("linux.cmdline", linux_cmdline_svc);

    /* Report at boot what the loader can see, so `make run` serial shows
     * whether a Linux kernel is actually available without any interaction. */
    {
        int vsz = -1, isz = -1, can = 0, lmode = 0;
        kern_linux_probe(&vsz, &isz, &can, &lmode);
        g_last_vmlinuz = vsz;
        g_last_initrd  = isz;
        if (vsz > 0) {
            pm_serial("[LX] vmlinuz found (valid bzImage, HdrS ok)");
            pm_serial(can ? "; bootable now" : "; NOT bootable in this CPU mode");
            pm_serial(" -- `plugin pm call linux_loader boot` chain-loads it\n");
        } else {
            pm_serial("[LX] no vmlinuz in SFS; Linux boot unavailable\n");
        }
    }
    return 0;
}

static void lx_exit(Plugin* self) {
    (void)self;
    svc_unregister("linux.boot");
    svc_unregister("linux.probe");
    svc_unregister("linux.cmdline");
}

static int lx_call(Plugin* self, const char* method, void* args, void* out, int outcap) {
    (void)self;
    const char* svc = lx_svc_name(method);
    if (!svc) return -1;                       /* unknown method */
    svc_fn f = svc_lookup(svc);
    if (!f) { pm_serial("[LX] no backend for "); pm_serial(svc); pm_serial("\n"); return -1; }
    int r = f(args, out, outcap);
    pm_serial("[LX] "); pm_serial(method); pm_serial(" ok\n");
    return r;
}

extern const Plugin g_linux_loader = {
    "linux_loader", 0x0100,
    "linux.boot,linux.probe,linux.cmdline",
    "",                                        /* no plugin deps */
    lx_init, lx_exit, lx_call
};
