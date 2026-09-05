/* Freestanding shim <dlfcn.h> for MiniOS Mono port (PAL).
 *
 * MiniOS 没有动态链接器，也不会有：ring-3 程序是静态 ELF，Mono 自己
 * 也会被静态链进去。但 utils/mono-dl-posix.c 是 mono-dl.c 的平台后端，
 * 而 mono-dl.c 又是 metadata/native-library.c（P/Invoke 的实现）的地基，
 * 整条链子不能删。
 *
 * 所以这里不是"假装支持"，而是【明确地不支持】：
 *   dlopen  永远返回 NULL
 *   dlerror 返回一句人能看懂的话
 * 于是 P/Invoke 到外部 .so 时，Mono 会抛
 * DllNotFoundException("... MiniOS has no dynamic loader")，
 * 而不是段错误或静默返回空指针。
 *
 * 内部调用（[DllImport("__Internal")]）走的是 mono_dl_open_self /
 * mono_add_internal_call 那条路，不经过这里，所以仍然可用——将来把
 * BCL 的 native 部分注册成 internal call 就是靠它。
 */
#ifndef PAL_DLFCN_H
#define PAL_DLFCN_H

#define RTLD_LAZY    0x0001
#define RTLD_NOW     0x0002
#define RTLD_LOCAL   0x0000
#define RTLD_GLOBAL  0x0100
#define RTLD_NOLOAD  0x0004
#define RTLD_NODELETE 0x1000

#define RTLD_DEFAULT ((void *) 0)
#define RTLD_NEXT    ((void *) -1)

void       *dlopen  (const char *filename, int flags);
void       *dlsym   (void *handle, const char *symbol);
int         dlclose (void *handle);
char       *dlerror (void);

#endif /* PAL_DLFCN_H */
