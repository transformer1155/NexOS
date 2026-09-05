/* Minimal config.h for MiniOS freestanding Mono port (PAL).
 * Stands in for the autoconf-generated top-level config.h.
 *
 * IMPORTANT: absent features must be `#undef`, never `#define X 0`.
 * Mono/eglib test most of them with `#ifdef HAVE_X`, so a 0-valued
 * define still counts as "present" and drags in host headers
 * (this is exactly how giconv.c ended up including the host <iconv.h>).
 *
 * Type sizes are for 32-bit x86.
 */
#ifndef MONO_CONFIG_H
#define MONO_CONFIG_H

/* ---- target properties ------------------------------------------- */
#undef  WORDS_BIGENDIAN

/* configure normally emits these; the interpreter + utils select their
 * arch backend from them (see mono-hwcap-vars.h, mono-context.h). */
#define TARGET_X86           1
#define HOST_X86             1
#define MONO_ARCHITECTURE    "x86"
#define TARGET_SIZEOF_VOID_P 4
#define SIZEOF_REGISTER      4

#define SIZEOF_VOID_P    4
#define SIZEOF_INT       4
#define SIZEOF_LONG      4
#define SIZEOF_SHORT     2
#define SIZEOF_LONG_LONG 8
#define SIZEOF___INT64   8
#define SIZEOF_FLOAT     4
#define SIZEOF_DOUBLE    8
#define SIZEOF_SIZE_T    4

/* ---- what the PAL actually provides ------------------------------ */
/* Only turn a HAVE_* on once pal/include/<hdr> exists AND libc_impl.c
 * (or a Mono-side stub) actually defines every symbol it unlocks. */
#define HAVE_STDINT_H    1
/* 故意不开 HAVE_C99_SUPPORT：eglib/glib.h 的 C99 分支把 g_error 写成
 *   g_log (dom, lvl, format, __VA_ARGS__)
 * 遇到 g_error ("literal") 这类零变参调用就会展开成 "…, )" 而编译失败
 * （metadata/ 里有 15 个文件踩这个）。#else 分支的 g_error(...) 写法
 * 对零变参和 N 变参都成立，所以保持 #undef。 */
#undef  HAVE_C99_SUPPORT
#define HAVE_UNISTD_H    1
#define HAVE_STDLIB_H    1
#define HAVE_STRING_H    1
#define HAVE_MEMORY_H    1
#define HAVE_INTTYPES_H  1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H  1
#define HAVE_SYS_TIME_H  1
#define HAVE_SIGNAL_H    1
#define HAVE_SEMAPHORE_H 1   /* else mono-os-semaphore.h takes the Win32 HANDLE path */
#define HAVE_TERMIOS_H   1   /* console-unix.c 的 struct termios 局部变量 */
#define HAVE_UTIME_H     1   /* appdomain.c 影子拷贝要 struct utimbuf */
#define HAVE_SYS_WAIT_H  1   /* w32process-unix.c 的 WIFSIGNALED/waitpid */
#define HAVE_LOCALE_H    1
#define HAVE_SCHED_H     1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SELECT_H 1  /* console-unix.c 的 fd_set/select */
/* mono-poll.h：HAVE_POLL 打开却没打开 HAVE_POLL_H / HAVE_SYS_POLL_H 时，
 * `#include <poll.h>` 会被跳过，但 `typedef struct pollfd mono_pollfd;`
 * 照样执行 → threadpool-io.c 里 "invalid use of incomplete typedef"。
 * 两个必须成对开。 */
#define HAVE_POLL        1
#define HAVE_POLL_H      1
/* DriveInfo 走 w32file-unix.c 的 statfs/statvfs 两条路：
 *   statvfs -> TotalSize / AvailableFreeSpace
 *   statfs  -> DriveType / DriveFormat（靠 f_type magic 查表）
 * GetDriveTypeFromPath() 在非 _AIX 分支里无条件用 struct statfs，
 * 所以两个头必须一起提供，不能只开一个。 */
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_STATFS_H  1
#define HAVE_STATVFS       1
#define HAVE_STATFS        1
/* mono-threads-posix.c 的 mono_thread_info_get_system_max_stack_size()
 * 用 getrlimit(RLIMIT_STACK) 反推栈上限，GC 扫栈根要靠它。
 * 头是条件包含的（#ifdef HAVE_SYS_RESOURCE_H），所以光放一个
 * pal/include/sys/resource.h 在那里是不够的，宏必须一起开。 */
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_GETRLIMIT      1
/* MiniOS 没有动态链接器。但 mono-dl-posix.c 是 P/Invoke 的地基，
 * 不能不编；PAL 的 dlopen 永远失败并给一句人话，见 pal/dlfcn.h。 */
#define HAVE_DLFCN_H     1
#define HAVE_STRTOK_R    1
#define HAVE_STRERROR_R  1
#define HAVE_ACCESS      1

/* ---- everything else is absent (autoconf-style #undef) ----------- */
/* 注意：这一段只能列上面【没有】出现过的宏。C 预处理器按行序生效，
 * 在这里重复 #undef 一个上面刚 #define 的名字，赢的是 #undef——
 * 曾经因此让 HAVE_SYS_WAIT_H / HAVE_LOCALE_H / HAVE_SYS_SELECT_H
 * 三个"已经补好头文件"的特性静默失效。改这个文件前先 grep 一遍重名。 */
#undef HAVE_ALLOCA_H
#undef HAVE_STRINGS_H
#undef HAVE_SYS_MMAN_H
#undef HAVE_SYS_POLL_H
#undef HAVE_PWD_H
#undef HAVE_ICONV_H
#undef HAVE_LIBICONV
#undef HAVE_PTHREAD_H
#undef HAVE_LANGINFO_H
#undef HAVE_CLOCK_NANOSLEEP
#undef HAVE_DLADDR
#undef HAVE_EXECV
#undef HAVE_EXECVE
#undef HAVE_FORK
#undef HAVE_GETDTABLESIZE
#undef HAVE_GETPWUID_R
#undef HAVE_LSTAT
#undef HAVE_MKDTEMP
#undef HAVE_NL_LANGINFO
#undef HAVE_REWINDDIR
#undef HAVE_STPCPY
#undef HAVE_STRLCPY
#undef HAVE_STRNDUP
#undef HAVE_VASPRINTF
#undef HAVE_MMAP
#undef HAVE_MPROTECT
#undef HAVE_GETRUSAGE
/* 这两个是 "#define X 0 不等于 #undef X" 的反例，必须照抄上游：
 * eglib 的 G_HAVE_API_SUPPORT(x) 就是 (x)，在 #if 里求值。上游
 * configure.ac 对所有平台（含 Unix）都 AC_DEFINE(HAVE_CLASSIC_WINAPI_SUPPORT,1)
 * 和 AC_DEFINE(HAVE_UWP_WINAPI_SUPPORT,0)。#undef 掉会让 icall.c 落到
 * UWP 分支：mono_icall_get_logical_drives 的定义被跳过，只剩一个签名
 * 完全不同的声明，于是 "too many arguments to function"。 */
#define HAVE_CLASSIC_WINAPI_SUPPORT 1
#define HAVE_UWP_WINAPI_SUPPORT     0

/* ---- garbage collector selection --------------------------------- *
 * 必须显式选一个，否则 sgen 目录下的每个 .c 和 metadata/sgen-mono.c 的整份文件体
 * 都在 #ifdef HAVE_SGEN_GC 里面，会安静地编成 0 符号的空目标文件——
 * 编译器不报一个字，普查还显示 25/25 PASS，直到链接时才发现所有
 * mono_gc_* 都缺。（我们踩过这个坑。tools/probe.sh 现在会对空 .o 告警。）
 * Boehm 需要 external/bdwgc，SGen 是 in-tree 的，所以选 SGen。
 *
 * 但 HAVE_SGEN_GC 【不能】写在这里！它在上游是 per-directory 的编译
 * 命令行宏，不是 config.h 宏：
 *     configure.ac:4985      SGEN_DEFINES="-DHAVE_SGEN_GC"
 *     metadata/Makefile.am:431  libmonoruntimesgen_la_CFLAGS = $(SGEN_DEFINES) ...
 *     sgen/Makefile.am:67       libmonosgen_la_CFLAGS      = $(SGEN_DEFINES)
 * mini/ 拿不到它，而且 mini/mini.h:70 有一道硬门：
 *     #if defined(HAVE_BOEHM_GC) || defined(HAVE_SGEN_GC)
 *     #error "The code in mini/ should not depend on these defines."
 * 一旦写进全局 config.h，mini/interp 下的三个 .c 全部编不过。
 * 所以：定义在 mono_port/Makefile 的 $(SGEN_DEFINES)，只给 meta/sgen。
 *
 * 进 config.h 的只有 AC_DEFINE 出来的那个：
 *     configure.ac:4984  AC_DEFINE(HAVE_MOVING_COLLECTOR, 1, ...)
 * 它是全局的（mini/ 也读），因为它描述的是"GC 会移动对象"这一事实，
 * 而不是"选了哪个 GC"。 */
#define HAVE_MOVING_COLLECTOR 1
#undef  HAVE_BOEHM_GC
#undef  HAVE_NULL_GC
/* HAVE_CONC_GC_AS_DEFAULT: 上游 --with-sgen-default-concurrent 默认 no。 */
#undef  HAVE_CONC_GC_AS_DEFAULT

/* No TLS / JIT / netcore in this port. */
#undef ENABLE_NETCORE
#undef ENABLE_OVERRIDABLE_ALLOCATORS

/* corlib 契约版本。上游由 configure.ac 的 MONO_CORLIB_VERSION 生成，
 * appdomain.c 的 mono_check_corlib_version() 会把它和 BCL 里的
 * Consts.MonoCorlibVersion 逐字符比较。
 * 下面这个 GUID 是从 vendor/mono.tar.gz 里 6.12.0.206 的 configure.ac
 * 原样抄出来的，所以我们能直接吃官方 6.12 的 mscorlib.dll，不用自己造。 */
#define MONO_CORLIB_VERSION "1A5E0066-58DC-428A-B21C-0AD6CDAE2789"

/* 平台名字串（metadata/mono-config.c 等处会用到） */
#define MONO_ASSEMBLIES  "/lib"
#define MONO_CFG_DIR     "/etc"
#define MONO_BINDIR      "/bin"

#endif /* MONO_CONFIG_H */
